#!/usr/bin/env python3
"""Local accounting/SSH protocol tests; no MPI, RDMA or remote hosts needed."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("measure_osu_memory.py")
spec = importlib.util.spec_from_file_location("measure_osu_memory", str(SCRIPT))
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class AccountingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def rank(self, pid, rank, rss=100, pss=60, job="job", exe="osu_allreduce", fallback=False):
        path = self.root / str(pid)
        path.mkdir()
        (path / "exe").symlink_to("/fake/" + exe)
        (path / "stat").write_text(str(pid) + " (osu) S " + "0 " * 18 + "123\n")
        (path / "environ").write_bytes(("MV2_MEM_RUN_ID=%s\0PMI_RANK=%d\0" % (job, rank)).encode())
        (path / "status").write_text("VmRSS: %d kB\nVmSwap: 0 kB\nHugetlbPages: 2048 kB\n" % rss)
        (path / ("smaps" if fallback else "smaps_rollup")).write_text(
            "Pss: %d kB\nPss_Anon: 999 kB\n" % pss + ("Pss: 10 kB\n" if fallback else ""))
        return path

    def test_job_filter_smaps_fallback_and_shared_rss(self):
        self.rank(10, 0)
        self.rank(11, 1, fallback=True)
        self.rank(12, 0, rss=9000, job="other-job")
        self.rank(13, 0, rss=9000, exe="hydra_pmi_proxy")
        row = measure.sample(self.root, "job", "osu_allreduce", 2, True)
        self.assertTrue(row["complete"])
        self.assertEqual(row["pids"], [10, 11])
        self.assertEqual(row["rss_kib"], 200)
        self.assertEqual(row["pss_kib"], 130)
        self.assertEqual(row["hugetlb_kib"], 4096)  # separate; never added to RSS

    def test_missing_pss_preserves_rss(self):
        path = self.rank(10, 0)
        (path / "smaps_rollup").unlink()
        row = measure.sample(self.root, "job", "osu_allreduce", 1, True)
        self.assertTrue(row["complete"])
        self.assertEqual(row["rss_kib"], 100)
        self.assertIsNone(row["pss_kib"])
        self.assertEqual(row["pss_read_errors"], 1)

    def test_duplicate_or_missing_rank_is_incomplete(self):
        self.rank(10, 0)
        row = measure.sample(self.root, "job", "osu_allreduce", 2, False)
        self.assertFalse(row["complete"])
        self.rank(11, 0)
        row = measure.sample(self.root, "job", "osu_allreduce", 2, False)
        self.assertFalse(row["complete"])

    def test_pid_reuse_is_rejected(self):
        self.rank(10, 0)
        with patch.object(measure, "identity", side_effect=["123", "124"]):
            row = measure.sample(self.root, "job", "osu_allreduce", 1, False)
        self.assertEqual(row["n_ranks"], 0)
        self.assertEqual(row["read_errors"], 1)

    def test_peak_of_sum_and_incomplete_samples(self):
        # Per-process peaks would sum to 200, while the observed node peak is 110.
        rows = [{"round": 0, "complete": True, "rss_kib": 100 + 10},
                {"round": 1, "complete": True, "rss_kib": 10 + 100},
                {"round": 2, "complete": False, "rss_kib": 150}]
        summary = measure.metric_summary(rows, "rss_kib")
        self.assertEqual(summary["peak_complete_kib"], 110)
        self.assertEqual(summary["peak_observed_kib"], 150)


class IntegrationTests(unittest.TestCase):
    def test_sudo_rejected_before_creating_results(self):
        with patch.object(measure.os, "geteuid", return_value=0), patch.dict(os.environ, SUDO_USER="test"):
            with self.assertRaisesRegex(ValueError, "without sudo"):
                measure.run(["hollow", "allreduce"])

    def test_local_collector_needs_no_ssh(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, MEM_SSH_BIN="/does/not/exist"):
            collector = measure.Collector("127.0.0.1", "127.0.0.1", "no-such-job",
                                          "osu_allreduce", 2, Path(tmp), 0)
            try:
                self.assertTrue(collector.local)
                self.assertTrue(collector.receive(measure.time.monotonic() + 5)["ready"])
                collector.send({"round": 0, "pss": False})
                self.assertEqual(collector.receive(measure.time.monotonic() + 5)["n_ranks"], 0)
            finally:
                collector.close()
        self.assertFalse(measure.is_local_target("127.0.0.1", "not-the-current-user@127.0.0.1"))

    def test_ssh_failure_reports_cause_without_starting_benchmark(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ssh = root / "fake-ssh"
            ssh.write_text("#!/bin/sh\necho 'Host key verification failed.' >&2\nexit 255\n")
            ssh.chmod(0o755)
            env = {"HOSTS": "fixture", "USER_MAP": "fixture=test", "NP": "2", "PPN": "2",
                   "MEM_SSH_BIN": str(ssh), "MEM_OUT": str(root / "results")}
            stderr = io.StringIO()
            with patch.dict(os.environ, env), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                code = measure.run(["hollow", "allreduce"], launcher=root / "must-not-start")
            summary = json.loads((root / "results/summary.json").read_text())
            self.assertEqual(code, 1)
            self.assertIsNone(summary["benchmark_exit_code"])
            self.assertEqual(summary["warnings"], [])
            self.assertIn("Host key verification failed.", stderr.getvalue())
            self.assertIn("benchmark was not started", stderr.getvalue())

    def test_controller_worker_protocol_and_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ssh = root / "fake-ssh"
            ssh.write_text("#!/usr/bin/env python3\nimport os,sys\nos.execv('/bin/sh', ['sh','-c',sys.argv[-1]])\n")
            ssh.chmod(0o755)
            # An ordinary sleep binary with the OSU executable name exercises
            # real /proc discovery and RSS/PSS without launching an MPI job.
            binary = root / "osu_allreduce"
            shutil.copy2(shutil.which("sleep"), str(binary))
            launcher = root / "launcher.sh"
            launcher.write_text("#!/bin/bash\n"
                                "PMI_RANK=0 ./osu_allreduce 0.8 &\n"
                                "PMI_RANK=1 ./osu_allreduce 0.8 &\n"
                                "MV2_MEM_RUN_ID=unrelated PMI_RANK=2 ./osu_allreduce 0.8 &\n"
                                "echo fixture-benchmark\nwait\n")
            env = {"HOSTS": "fixture", "USER_MAP": "fixture=test", "NP": "2", "PPN": "2",
                   "MEM_INTERVAL": "0.05", "MEM_PSS": "1", "MEM_PSS_INTERVAL": "0.1",
                   "MEM_SSH_BIN": str(ssh), "MEM_REMOTE_PYTHON": shutil.which("python3"),
                   "MEM_OUT": str(root / "results")}
            previous = os.getcwd()
            try:
                os.chdir(str(root))
                with patch.dict(os.environ, env), contextlib.redirect_stdout(io.StringIO()):
                    code = measure.run(["hollow", "allreduce"], launcher=launcher)
                self.assertEqual(code, 0)
                summary = json.loads((root / "results/summary.json").read_text())
                self.assertGreater(summary["nodes"]["fixture"]["rss_kib"]["peak_complete_kib"], 0)
                self.assertGreater(summary["cluster"]["pss_kib"]["complete_samples"], 0)
                self.assertIn("fixture-benchmark", (root / "results/benchmark.log").read_text())
                launcher.write_text("#!/bin/bash\nexit 7\n")
                env["MEM_OUT"] = str(root / "failure")
                with patch.dict(os.environ, env), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    code = measure.run(["hollow", "allreduce"], launcher=launcher)
                self.assertEqual(code, 7)
                summary = json.loads((root / "failure/summary.json").read_text())
                self.assertIsNone(summary["cluster"]["rss_kib"]["peak_complete_kib"])
            finally:
                os.chdir(previous)

    def test_launcher_explicit_job_marker(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "bin").mkdir()
            mpiexec = root / "bin/mpiexec"
            mpiexec.write_text("#!/bin/bash\nprintf '%s\\n' \"$@\"\n")
            mpiexec.chmod(0o755)
            env = dict(os.environ, HOSTS="fixture", HCA_MAP="fixture=mlx5_0", USER_MAP="",
                       NP="2", PPN="2", MVAPICH2_HOME=str(root), MV2_MEM_RUN_ID="test-marker")
            result = subprocess.run(["bash", str(SCRIPT.with_name("run_osu_collective.sh")),
                                     "hollow", "allreduce", "-i", "10"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("-genv\nMV2_MEM_RUN_ID\ntest-marker\n", result.stdout)
            del env["MV2_MEM_RUN_ID"]
            result = subprocess.run(["bash", str(SCRIPT.with_name("run_osu_collective.sh")),
                                     "ordinary", "allreduce"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("MV2_MEM_RUN_ID", result.stdout)


if __name__ == "__main__":
    unittest.main()
