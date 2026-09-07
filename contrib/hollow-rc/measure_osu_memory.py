#!/usr/bin/env python3
"""Sample this job's OSU ranks on each host; see MEMORY_MEASUREMENT.md.

Usage: python3 measure_osu_memory.py MODE BENCHMARK [OSU options ...]
Uses the same HOSTS/USER_MAP/NP/PPN/MV2_* environment as run_osu_collective.sh.
Optional: MEM_OUT, MEM_INTERVAL=0.2, MEM_PSS=0, MEM_PSS_INTERVAL=1,
MEM_SSH_BIN (defaults to MV2_SSH_BIN or ssh), MEM_REMOTE_PYTHON=python3,
MEM_TIMEOUT=30. Python >= 3.6 is required locally and on each host.
"""

import csv
import json
import math
import os
from pathlib import Path
import pwd
import queue
import shlex
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid


def fields(path, wanted):
    """Sum exact field names; works for both status and multi-VMA smaps."""
    result = {}
    with open(str(path)) as stream:
        for line in stream:
            key, sep, value = line.partition(":")
            if sep and key in wanted:
                result[key] = result.get(key, 0) + int(value.split()[0])
    return result


def identity(path):
    # Field 22, after the comm field (which can itself contain spaces/parens).
    return (path / "stat").read_text().rsplit(")", 1)[1].split()[19]


def sample(proc_root, job_id, executable, expected, with_pss):
    started = time.monotonic()
    wall_started = time.time()
    marker = ("MV2_MEM_RUN_ID=" + job_id).encode()
    rows, errors, pss_errors = [], 0, 0
    for path in proc_root.iterdir():
        if not path.name.isdigit():
            continue
        try:
            if path.stat().st_uid != os.getuid():
                continue
            exe = os.path.basename(os.readlink(str(path / "exe")))
            if exe.endswith(" (deleted)"):
                exe = exe[:-10]
            if exe != executable:
                continue
            birth = identity(path)
            env = (path / "environ").read_bytes().split(b"\0")
            if marker not in env:
                continue
        except (OSError, ValueError, IndexError):
            # Unrelated/vanished/inaccessible processes are never counted.
            continue
        try:
            rank = next(int(v.split(b"=", 1)[1]) for v in env
                        if v.startswith(b"PMI_RANK="))
            data = fields(path / "status", {"VmRSS", "VmSwap", "HugetlbPages"})
            rss = data["VmRSS"]
            detail = {}
            if with_pss:
                try:
                    try:
                        detail = fields(path / "smaps_rollup", {"Pss"})
                    except FileNotFoundError:
                        detail = fields(path / "smaps", {"Pss"})
                    if "Pss" not in detail:
                        raise ValueError("Pss unavailable")
                except (OSError, ValueError, IndexError):
                    pss_errors += 1
                    detail = {}
            if birth != identity(path):
                raise ValueError("PID reused")
            rows.append({"pid": int(path.name), "rank": rank, "rss_kib": rss,
                         "pss_kib": detail.get("Pss"),
                         "hugetlb_kib": data.get("HugetlbPages"),
                         "swap_kib": data.get("VmSwap")})
        except (OSError, ValueError, IndexError, KeyError, StopIteration):
            errors += 1
    ranks = [r["rank"] for r in rows]
    complete = len(rows) == expected and len(set(ranks)) == expected and errors == 0
    result = {"wall_start": wall_started, "scan_seconds": time.monotonic() - started,
              "n_ranks": len(rows), "expected_ranks": expected,
              "complete": complete, "read_errors": errors,
              "pss_read_errors": pss_errors,
              "pss_sampled": with_pss, "ranks": sorted(ranks),
              "pids": sorted(r["pid"] for r in rows)}
    for name in ("rss_kib", "pss_kib", "hugetlb_kib", "swap_kib"):
        values = [r[name] for r in rows]
        result[name] = (sum(values) if values and all(v is not None for v in values)
                        else None)
    return result


def worker(args):
    job_id, executable, expected = args
    print(json.dumps({"ready": True, "hostname": os.uname().nodename}), flush=True)
    # stdin is the control channel. EOF also stops the remote process if SSH
    # or the controller exits, so no remote files or persistent daemons remain.
    for line in sys.stdin:
        command = json.loads(line)
        if command.get("stop"):
            break
        result = sample(Path("/proc"), job_id, executable, int(expected), command["pss"])
        result["round"] = command["round"]
        print(json.dumps(result), flush=True)


def is_local_target(host, target):
    """Only bypass SSH for this machine AND the current operating-system user."""
    user = pwd.getpwuid(os.getuid()).pw_name
    if "@" in target and target.rsplit("@", 1)[0] != user:
        return False
    names = {"localhost", "127.0.0.1", "::1", socket.gethostname(), socket.getfqdn()}
    try:
        result = subprocess.run(["hostname", "-I"], stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, universal_newlines=True, timeout=2)
        if result.returncode == 0:
            names.update(result.stdout.split())
    except (OSError, subprocess.TimeoutExpired):
        pass
    return host in names


class Collector:
    def __init__(self, host, target, job_id, executable, ppn, out, index):
        self.host, self.index = host, index
        self.incoming = queue.Queue()
        self.log_path = out / ("collector-%d.log" % index)
        self.log = open(str(self.log_path), "w")
        # Quote every remote shell argument, including source code. The worker
        # runs entirely in memory: no install or matching home paths required.
        command = [os.environ.get("MEM_REMOTE_PYTHON", "python3"), "-u", "-c",
                   Path(__file__).read_text(), "--worker", job_id, executable, str(ppn)]
        ssh = os.environ.get("MEM_SSH_BIN", os.environ.get("MV2_SSH_BIN", "ssh"))
        self.local = is_local_target(host, target)
        if self.local:
            argv = [sys.executable, "-u", str(Path(__file__).resolve()),
                    "--worker", job_id, executable, str(ppn)]
        else:
            argv = [ssh, "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10",
                    "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
                    target, " ".join(shlex.quote(v) for v in command)]
        try:
            self.process = subprocess.Popen(
                argv,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log,
                universal_newlines=True, bufsize=1)
        except BaseException:
            self.log.close()
            raise
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        try:
            for line in self.process.stdout:
                self.incoming.put(json.loads(line))
        except Exception as exc:
            self.incoming.put({"error": str(exc)})
        finally:
            self.incoming.put({"error": "collector disconnected"})

    def send(self, command):
        self.process.stdin.write(json.dumps(command) + "\n")
        self.process.stdin.flush()

    def receive(self, deadline):
        try:
            result = self.incoming.get(timeout=max(0.001, deadline - time.monotonic()))
        except queue.Empty:
            raise RuntimeError("%s: collector timed out" % self.host)
        if "error" in result:
            detail = self.log_path.read_text(errors="replace").strip()[-2000:]
            raise RuntimeError("%s: %s%s; see collector-%d.log" %
                               (self.host, result["error"],
                                ("; " + detail) if detail else "", self.index))
        return result

    def close(self):
        try:
            self.process.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.thread.join(timeout=2)
        self.process.stdout.close()
        self.log.close()


def positive_float(name, default):
    value = float(os.environ.get(name, default))
    if not math.isfinite(value) or value <= 0:
        raise ValueError(name + " must be a finite positive number")
    return value


def metric_summary(rows, key):
    valid = [r for r in rows if r["complete"] and r.get(key) is not None]
    observed = [r for r in rows if r.get(key) is not None]
    peak = max(valid, key=lambda r: r[key]) if valid else None
    return {"complete_samples": len(valid),
            "peak_complete_kib": peak[key] if peak else None,
            "peak_complete_gib": peak[key] / 1048576.0 if peak else None,
            "peak_round": peak["round"] if peak else None,
            "sample_mean_complete_kib": sum(r[key] for r in valid) / len(valid) if valid else None,
            "peak_observed_kib": max(r[key] for r in observed) if observed else None}


def run(args, launcher=None):
    if os.geteuid() == 0 and os.environ.get("SUDO_USER", "root") != "root":
        raise ValueError("Run this sampler as your normal login user without sudo. "
                         "sudo changes the SSH identity/known_hosts and creates root-owned results.")
    if len(args) < 2 or args[0] not in ("ordinary", "xrc", "hollow") or args[1] not in (
            "allreduce", "alltoall", "bcast", "broadcast"):
        raise ValueError(__doc__)
    benchmark = "bcast" if args[1] == "broadcast" else args[1]
    hosts = os.environ.get("HOSTS", "").split(",")
    if not all(hosts) or len(set(hosts)) != len(hosts) or any(h.startswith("-") for h in hosts):
        raise ValueError("HOSTS must contain unique nonempty hosts")
    users = dict(item.split("=", 1) for item in os.environ.get("USER_MAP", "").split(",") if item)
    if users and any(not users.get(h) for h in hosts):
        raise ValueError("USER_MAP must provide a user for every host")
    targets = [(users[h] + "@" + h) if users else h for h in hosts]
    if any(t.startswith("-") for t in targets):
        raise ValueError("Invalid SSH target")
    np, ppn = int(os.environ.get("NP", "16")), int(os.environ.get("PPN", "8"))
    if ppn <= 0 or np != ppn * len(hosts):
        raise ValueError("This sampler requires NP = PPN * number of HOSTS (uniform placement)")
    interval = positive_float("MEM_INTERVAL", "0.2")
    pss_interval = positive_float("MEM_PSS_INTERVAL", "1")
    timeout = positive_float("MEM_TIMEOUT", "30")
    pss_flag = os.environ.get("MEM_PSS", "0")
    if pss_flag not in ("0", "1"):
        raise ValueError("MEM_PSS must be 0 or 1")
    job_id = uuid.uuid4().hex
    out = Path(os.environ.get("MEM_OUT", "memory-results/" +
               time.strftime("%Y%m%d-%H%M%S-") + args[0] + "-" + benchmark + "-" + job_id[:8])).resolve()
    out.mkdir(parents=True, exist_ok=False)
    launcher = Path(launcher) if launcher else Path(__file__).with_name("run_osu_collective.sh")
    metadata = {"job_id": job_id, "command": [str(launcher)] + args,
                "hosts": hosts, "targets": targets, "np": np, "ppn": ppn,
                "rss_interval_seconds": interval, "pss_enabled": pss_flag == "1",
                "pss_interval_seconds": pss_interval,
                "environment": {k: v for k, v in os.environ.items()
                                if k.startswith(("MV2_", "MEM_")) or k in
                                ("HOSTS", "HCA_MAP", "USER_MAP", "NP", "PPN", "MVAPICH2_HOME",
                                 "REMOTE_WORKSPACE", "RDMA_CORE_LIBDIR", "RUN_WDIR")}}
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print("Memory results: " + str(out), flush=True)
    collectors, node_rows, cluster_rows = [], {h: [] for h in hosts}, []
    process, tee_thread, error, exit_code = None, None, None, 1
    columns = ["round", "host", "elapsed_seconds", "wall_start", "scan_seconds",
               "n_ranks", "expected_ranks", "complete", "read_errors", "pss_read_errors", "pss_sampled",
               "rss_kib", "pss_kib", "hugetlb_kib", "swap_kib", "ranks", "pids"]
    csvfile = open(str(out / "samples.csv"), "w", newline="")
    writer = csv.DictWriter(csvfile, fieldnames=columns)
    writer.writeheader()
    clusterfile = open(str(out / "cluster_samples.csv"), "w", newline="")
    clusterwriter = csv.DictWriter(clusterfile, fieldnames=[
        "round", "elapsed_seconds", "round_seconds", "n_ranks", "complete",
        "rss_kib", "pss_kib", "hugetlb_kib", "swap_kib"])
    clusterwriter.writeheader()
    run_log = open(str(out / "benchmark.log"), "w")

    def tee():
        for line in process.stdout:
            run_log.write(line)
            run_log.flush()
            try:
                sys.stdout.write(line)
                sys.stdout.flush()
            except BrokenPipeError:
                pass

    old_term = signal.getsignal(signal.SIGTERM)

    def interrupted(signum, frame):
        raise KeyboardInterrupt()

    signal.signal(signal.SIGTERM, interrupted)
    try:
        for index, (host, target) in enumerate(zip(hosts, targets)):
            collectors.append(Collector(host, target, job_id, "osu_" + benchmark, ppn, out, index))
        deadline = time.monotonic() + timeout
        for collector in collectors:
            if not collector.receive(deadline).get("ready"):
                raise RuntimeError("collector did not report ready")
        env = dict(os.environ, MV2_MEM_RUN_ID=job_id)
        process = subprocess.Popen(["bash", str(launcher)] + args, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   universal_newlines=True, bufsize=1, start_new_session=True)
        tee_thread = threading.Thread(target=tee, daemon=True)
        tee_thread.start()
        origin, next_pss, round_id = time.monotonic(), 0.0, 0
        while True:
            tick = time.monotonic()
            with_pss = pss_flag == "1" and tick >= next_pss
            if with_pss:
                next_pss = tick + pss_interval
            for collector in collectors:
                collector.send({"round": round_id, "pss": with_pss})
            batch = []
            for collector in collectors:
                row = collector.receive(tick + timeout)
                if row.get("round") != round_id:
                    raise RuntimeError("collector round mismatch")
                row.update(host=collector.host, elapsed_seconds=tick - origin)
                batch.append(row)
                node_rows[collector.host].append(row)
                writer.writerow(row)
            csvfile.flush()
            ranks = [rank for row in batch for rank in row["ranks"]]
            cluster = {"round": round_id, "elapsed_seconds": tick - origin,
                       "round_seconds": time.monotonic() - tick,
                       "n_ranks": len(ranks),
                       "complete": all(r["complete"] for r in batch) and sorted(ranks) == list(range(np))}
            for key in ("rss_kib", "pss_kib", "hugetlb_kib", "swap_kib"):
                values = [r[key] for r in batch]
                cluster[key] = sum(values) if all(v is not None for v in values) else None
            cluster_rows.append(cluster)
            clusterwriter.writerow(cluster)
            clusterfile.flush()
            if process.poll() is not None:
                exit_code = process.returncode
                break
            # Waiting on the launcher allows prompt exit without delaying the
            # next sample if scanning itself exceeded the requested interval.
            remaining = interval - (time.monotonic() - tick)
            if remaining > 0:
                try:
                    process.wait(timeout=remaining)
                except subprocess.TimeoutExpired:
                    pass
            round_id += 1
    except KeyboardInterrupt:
        error, exit_code = "interrupted", 130
    except Exception as exc:
        error, exit_code = str(exc), 1
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if process is not None and process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            except ProcessLookupError:
                pass
        for collector in collectors:
            collector.close()
        if tee_thread:
            tee_thread.join(timeout=5)
        if process is not None and (tee_thread is None or not tee_thread.is_alive()):
            process.stdout.close()
        if tee_thread is None or not tee_thread.is_alive():
            run_log.close()
        csvfile.close()
        clusterfile.close()
        signal.signal(signal.SIGTERM, old_term)
    metrics = ("rss_kib", "pss_kib", "hugetlb_kib", "swap_kib")
    warnings = []
    for host, rows in node_rows.items():
        if process is not None and not any(r["complete"] for r in rows):
            warnings.append(host + ": no complete-rank sample; increase iterations or check collector logs/PID access")
        if process is not None and pss_flag == "1" and not any(r["complete"] and r["pss_kib"] is not None for r in rows):
            warnings.append(host + ": no complete PSS sample")
        if any((r["hugetlb_kib"] or 0) > 0 for r in rows):
            warnings.append(host + ": HugeTLB detected; RSS/PSS do not cover all hugepage memory; HugetlbPages may double-count shared mappings")
        if any((r["swap_kib"] or 0) > 0 for r in rows):
            warnings.append(host + ": VmSwap is nonzero; RSS excludes swapped-out memory")
        if any(r["scan_seconds"] > interval for r in rows):
            warnings.append(host + ": a scan exceeded MEM_INTERVAL; inspect actual scan/round durations")
    if process is not None and not any(r["complete"] for r in cluster_rows):
        warnings.append("No complete cluster round (all global ranks 0..NP-1)")
    if exit_code == 0 and (not any(r["complete"] for r in cluster_rows) or
                          (pss_flag == "1" and not any(r["complete"] and r["pss_kib"] is not None for r in cluster_rows))):
        exit_code = 2
    summary = {"exit_code": exit_code, "benchmark_exit_code": process.returncode if process else None,
               "error": error, "warnings": warnings,
               "scope": "OSU ranks only; full execution including MPI init, OSU warmup and finalize; no kernel memory",
               "nodes": {h: {key: metric_summary(rows, key) for key in metrics}
                         for h, rows in node_rows.items()},
               "cluster": {key: metric_summary(cluster_rows, key) for key in metrics}}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    if process is None:
        print("ERROR: benchmark was not started: " + str(error), file=sys.stderr)
        print("Summary: " + str(out / "summary.json"), flush=True)
        return exit_code
    for host in hosts:
        rss = summary["nodes"][host]["rss_kib"]["peak_complete_gib"]
        pss = summary["nodes"][host]["pss_kib"]["peak_complete_gib"]
        print("%s: peak RSS=%s GiB, peak PSS=%s GiB (complete ranks)" %
              (host, "%.6f" % rss if rss is not None else "N/A",
               "%.6f" % pss if pss is not None else "N/A"))
    value = summary["cluster"]["rss_kib"]["peak_complete_gib"]
    print("Cluster peak RSS (same sampling round): %s GiB" %
          ("%.6f" % value if value is not None else "N/A"))
    for warning in warnings:
        print("WARNING: " + warning, file=sys.stderr)
    if error:
        print("ERROR: " + error, file=sys.stderr)
    print("Summary: " + str(out / "summary.json"), flush=True)
    return exit_code


if __name__ == "__main__":
    if sys.argv[1:2] == ["--worker"]:
        worker(sys.argv[2:])
    elif sys.argv[1:2] in (["-h"], ["--help"]):
        print(__doc__)
    else:
        try:
            sys.exit(run(sys.argv[1:]))
        except (ValueError, OSError) as exc:
            print(str(exc), file=sys.stderr)
            sys.exit(2)
