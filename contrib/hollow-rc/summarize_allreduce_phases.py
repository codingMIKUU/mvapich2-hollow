#!/usr/bin/env python3
"""Summarize one job's MPI_Finalize phase records, without running MPI."""

import argparse
from collections import defaultdict
import math
from pathlib import Path
import sys

PREFIX = "MV2_ALLREDUCE_PHASE "
GROUP = ("mode", "world", "bytes", "datatype", "op", "inter_algo", "skip", "every")


def read_records(lines):
    records = []
    for line in lines:
        if PREFIX not in line:
            continue
        fields = dict(word.split("=", 1) for word in
                      line.split(PREFIX, 1)[1].split() if "=" in word)
        required = GROUP + ("rank", "host", "local_rank", "local_size", "samples",
                            "calls", "failed", "inter_samples")
        for name in required:
            if name not in fields:
                raise ValueError("incomplete diagnostic line: missing " + name)
        for name in ("world", "rank", "local_rank", "local_size", "samples",
                     "calls", "failed", "inter_samples", "bytes", "skip", "every"):
            fields[name] = int(fields[name])
            if fields[name] < 0:
                raise ValueError("negative diagnostic counter: " + name)
        for phase in ("reduce", "inter", "bcast", "total"):
            for metric in ("avg", "max"):
                name = phase + "_" + metric + "_us"
                fields[name] = float(fields[name])
                if not math.isfinite(fields[name]) or fields[name] < 0:
                    raise ValueError("invalid timing: " + name)
        records.append(fields)
    return records


def summarize(records, out=sys.stdout):
    groups = defaultdict(list)
    for record in records:
        groups[tuple(record[key] for key in GROUP)].append(record)
    if not groups:
        raise ValueError("no MV2_ALLREDUCE_PHASE records; enable diagnostics, "
                         "use MPI_COMM_WORLD two-level Allreduce, and exit normally")
    for key, rows in sorted(groups.items()):
        mode, world, size, datatype, op, algo, skip, every = key
        ranks = [r["rank"] for r in rows]
        if len(set(ranks)) != len(ranks):
            raise ValueError("duplicate rank records: use one job per log file")
        print("mode={} world={} bytes={} datatype={} op={} inter={} skip={} every={}"
              .format(*key), file=out)
        print("  ranks={}/{} samples={} failed={}"
              .format(len(rows), world, sum(r["samples"] for r in rows),
                      sum(r["failed"] for r in rows)), file=out)
        if set(ranks) != set(range(world)):
            print("  WARNING: incomplete rank coverage; averages cover only received records",
                  file=out)
        if len({(r["calls"], r["samples"]) for r in rows}) != 1:
            print("  WARNING: rank call/sample counts differ; compare sampling windows",
                  file=out)
        print("  phase       sample-weighted-avg-us  max-rank-avg-us  max-sampled-call-us",
              file=out)
        for phase in ("reduce", "inter", "bcast", "total"):
            count = "inter_samples" if phase == "inter" else "samples"
            valid = [r for r in rows if r[count]]
            if not valid:
                print("  {:10s} N/A (no samples)".format(phase), file=out)
                continue
            average = sum(r[phase + "_avg_us"] * r[count] for r in valid) / sum(
                r[count] for r in valid)
            print("  {:10s} {:22.3f} {:16.3f} {:20.3f}".format(
                phase, average, max(r[phase + "_avg_us"] for r in valid),
                max(r[phase + "_max_us"] for r in valid)), file=out)
        for r in sorted(rows, key=lambda row: row["rank"]):
            if r["local_rank"] == 0:
                print("  leader rank={} host={} samples={} reduce={:.3f} "
                      "inter={:.3f} bcast={:.3f} total={:.3f} us".format(
                          r["rank"], r["host"], r["samples"], r["reduce_avg_us"],
                          r["inter_avg_us"], r["bcast_avg_us"], r["total_avg_us"]), file=out)
        print("  Note: nonleader bcast includes waiting for the leader's exchange; "
              "inter includes peer readiness/progress waits. Do not add cross-rank maxima.",
              file=out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="stderr log from one completed MPI job")
    args = parser.parse_args()
    try:
        with args.log.open(errors="replace") as stream:
            summarize(read_records(stream))
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, "phase summary: {}\n".format(error))


if __name__ == "__main__":
    main()
