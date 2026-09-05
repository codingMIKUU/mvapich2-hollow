#!/usr/bin/env python3
"""Exercise launcher arguments without starting MPI, SSH, or RDMA."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPTS = Path(__file__).resolve().parents[1]


class BroadcastLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name)
        (self.prefix / "bin").mkdir()
        mock = self.prefix / "bin/mpiexec"
        mock.write_text(
            "#!" + sys.executable + "\n"
            "import json, os, sys\n"
            "print(json.dumps({'argv': sys.argv[1:], "
            "'inter_bcast': os.environ.get('MV2_INTER_BCAST_TUNING'), "
            "'intra_bcast': os.environ.get('MV2_INTRA_BCAST_TUNING')}))\n"
        )
        mock.chmod(0o755)
        self.env = {
            "PATH": os.defpath,
            "TMPDIR": self.temp.name,
            "MVAPICH2_HOME": self.temp.name,
            "HOSTS": "192.0.2.11,192.0.2.12",
            "HCA_MAP": "192.0.2.11=mlx5_1,192.0.2.12=mlx5_3",
            "USER_MAP": "192.0.2.11=user1,192.0.2.12=user2",
            "NP": "4",
            "PPN": "2",
        }

    def launch(self, mode, benchmark, *args):
        result = subprocess.run(
            ["bash", str(SCRIPTS / "run_osu_collective.sh"),
             mode, benchmark, *args],
            env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(list(self.prefix.glob("mv2-hollow-hosts.*")))
        return json.loads(result.stdout.splitlines()[-1])

    def test_modes_alias_and_existing_benchmarks(self):
        options = ["-m", "1024:32768", "-i", "10", "-x", "2", "-c"]
        for mode in ("ordinary", "xrc", "hollow"):
            for benchmark in ("bcast", "broadcast", "allreduce", "alltoall"):
                with self.subTest(mode=mode, benchmark=benchmark):
                    argv = self.launch(mode, benchmark, *options)["argv"]
                    expected = "bcast" if benchmark == "broadcast" else benchmark
                    tail = ["mv2-rank", expected] + options
                    self.assertEqual(argv[-len(tail):], tail)
                    pos = argv.index("MV2_TRANSPORT_MODE")
                    self.assertEqual(argv[pos + 1], mode)
                    self.assertEqual(argv[argv.index("-n") + 1], "4")
                    self.assertEqual(argv[argv.index("-ppn") + 1], "2")

    def test_default_options_and_native_tuning(self):
        self.env["MV2_INTER_BCAST_TUNING"] = "1"
        self.env["MV2_INTRA_BCAST_TUNING"] = "1"
        result = self.launch("ordinary", "bcast")
        self.assertEqual(result["argv"][-9:],
                         ["mv2-rank", "bcast", "-m", "1:1048576",
                          "-i", "1000", "-x", "200", "-f"])
        self.assertEqual(result["inter_bcast"], "1")
        self.assertEqual(result["intra_bcast"], "1")
        self.assertNotIn("MV2_INTER_BCAST_TUNING", result["argv"])
        self.assertNotIn("MV2_INTRA_BCAST_TUNING", result["argv"])

    def test_rank_wrapper_accepts_both_names(self):
        # An empty HCA map deliberately stops before library/device access.
        for benchmark in ("bcast", "broadcast"):
            with self.subTest(benchmark=benchmark):
                result = subprocess.run(
                    ["bash", str(SCRIPTS / "mv2_hca_rank_wrapper.sh"), benchmark],
                    env={"PATH": os.defpath, "HOLLOW_RC_HCA_MAP": ""},
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    universal_newlines=True, timeout=10,
                )
                self.assertEqual(result.returncode, 1)
                self.assertIn("No HCA mapping", result.stderr)
                self.assertNotIn("Usage:", result.stderr)


if __name__ == "__main__":
    unittest.main()
