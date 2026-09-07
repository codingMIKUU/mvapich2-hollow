#!/usr/bin/env python3
"""Compile a clock/MPI stub and test sampling; no MPI/SSH/RDMA is started."""
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "phase_summary", HERE.parent / "summarize_allreduce_phases.py")
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


class PhaseTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "phase"
        cls.disabled = Path(cls.temp.name) / "phase-disabled"
        for target, flags in ((cls.binary, []),
                              (cls.disabled, ["-DMV2_ENABLE_ALLREDUCE_PHASE_DIAG=0"])):
            subprocess.run(["cc", "-std=gnu99", "-O2", "-Wall", "-Wextra",
                            "-Wno-unused-function", "-Wno-unused-variable",
                            "-Wno-unused-parameter", *flags,
                            str(HERE / "allreduce_phase_diag_unit.c"), "-o", str(target)],
                           check=True, timeout=30)

    def run_case(self, scenario="leader", binary=None, **settings):
        env = {"PATH": os.defpath, "MV2_ALLREDUCE_PHASE_STATS": "1",
               "MV2_ALLREDUCE_PHASE_EVERY": "4", "MV2_ALLREDUCE_PHASE_SKIP": "2"}
        env.update(settings)
        result = subprocess.run([str(binary or self.binary), scenario], env=env,
                                capture_output=True, universal_newlines=True,
                                check=True, timeout=10)
        return result, summary.read_records(result.stderr.splitlines())

    def test_runtime_and_compile_time_off_do_not_read_clock(self):
        for binary, flag in ((self.binary, "0"), (self.disabled, "1")):
            result, rows = self.run_case(binary=binary, MV2_ALLREDUCE_PHASE_STATS=flag)
            self.assertEqual(result.stdout.strip(), "clock_reads=0 finalize_registrations=0")
            self.assertEqual(rows, [])

    def test_unset_settings_default_to_off_and_sample_every_64(self):
        env = {"PATH": os.defpath}
        off = subprocess.run([str(self.binary)], env=env, capture_output=True,
                             universal_newlines=True, check=True, timeout=10)
        self.assertEqual(off.stdout.strip(), "clock_reads=0 finalize_registrations=0")
        self.assertEqual(off.stderr, "")
        env["MV2_ALLREDUCE_PHASE_STATS"] = "1"
        on = subprocess.run([str(self.binary)], env=env, capture_output=True,
                            universal_newlines=True, check=True, timeout=10)
        rows = summary.read_records(on.stderr.splitlines())
        self.assertEqual((rows[0]["every"], rows[0]["skip"], rows[0]["samples"]),
                         (64, 0, 1))
        self.assertIn("clock_reads=4", on.stdout)

    def test_leader_sampling_skip_and_actual_dispatch(self):
        result, rows = self.run_case()
        self.assertIn("clock_reads=8", result.stdout)
        self.assertEqual(len(rows), 1)
        row = rows[0]
        self.assertEqual((row["calls"], row["samples"], row["inter_samples"]), (10, 2, 2))
        self.assertEqual(row["inter_algo"], "rs")
        self.assertEqual(row["total_avg_us"], 3)
        self.assertEqual(row["reduce_avg_us"], 1)
        self.assertEqual(row["inter_avg_us"], 1)
        self.assertEqual(row["bcast_avg_us"], 1)
        _, rd_rows = self.run_case("rd")
        self.assertEqual(rd_rows[0]["inter_algo"], "rd")

    def test_nonleader_and_single_node_do_not_count_network_samples(self):
        for scenario in ("nonleader", "local"):
            result, rows = self.run_case(scenario)
            self.assertIn("clock_reads=6", result.stdout)
            self.assertEqual(rows[0]["inter_samples"], 0)
            self.assertEqual(rows[0]["inter_avg_us"], 0)
            self.assertEqual(rows[0]["total_avg_us"], 2)

    def test_world_and_size_filters(self):
        result, rows = self.run_case("nonworld")
        self.assertIn("clock_reads=0", result.stdout)
        self.assertEqual(rows, [])
        result, rows = self.run_case("sizes", MV2_ALLREDUCE_PHASE_BYTES="512")
        self.assertEqual([row["bytes"] for row in rows], [512])
        self.assertEqual(rows[0]["samples"], 2)

    def test_per_size_skip_and_no_samples(self):
        _, rows = self.run_case("sizes")
        self.assertEqual([row["samples"] for row in rows], [2, 2])
        result, rows = self.run_case(MV2_ALLREDUCE_PHASE_SKIP="20")
        self.assertIn("clock_reads=0", result.stdout)
        self.assertEqual(rows[0]["samples"], 0)
        out = io.StringIO()
        summary.summarize(rows, out)
        self.assertIn("N/A (no samples)", out.getvalue())

    def test_failed_calls_are_not_reported_as_successful_timings(self):
        _, rows = self.run_case("error")
        self.assertEqual(rows[0]["failed"], 2)
        self.assertEqual(rows[0]["samples"], 0)

    def test_invalid_settings_fail_closed(self):
        for key, value in (("EVERY", "0"), ("EVERY", "3"), ("SKIP", "-1"),
                           ("BYTES", "xyz"), ("STATS", "2"),
                           ("SKIP", "18446744073709551616")):
            result, rows = self.run_case(**{"MV2_ALLREDUCE_PHASE_" + key: value})
            self.assertIn("clock_reads=0", result.stdout)
            self.assertIn("invalid diagnostic setting", result.stderr)
            self.assertEqual(rows, [])

    def test_summary_handles_rank_coverage_and_excludes_nonleader_inter(self):
        _, leaders = self.run_case()
        _, others = self.run_case("nonleader")
        out = io.StringIO()
        summary.summarize(leaders + others, out)
        self.assertIn("incomplete rank coverage", out.getvalue())
        inter_line = next(line for line in out.getvalue().splitlines()
                          if line.startswith("  inter "))
        self.assertEqual(float(inter_line.split()[1]), 1)
        with self.assertRaisesRegex(ValueError, "duplicate rank"):
            summary.summarize(leaders + leaders, io.StringIO())
        with self.assertRaisesRegex(ValueError, "no MV2_ALLREDUCE_PHASE"):
            summary.summarize([], io.StringIO())


if __name__ == "__main__":
    unittest.main()
