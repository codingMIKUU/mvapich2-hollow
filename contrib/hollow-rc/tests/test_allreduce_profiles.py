#!/usr/bin/env python3
"""Launcher-only regression tests: no MPI ranks, SSH, or RDMA operations."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


LAUNCHER = Path(__file__).resolve().parents[1] / "run_osu_collective.sh"
PROFILES = {
    "flat-rd": ("1", "0", "5"),
    "flat-rsag": ("2", "0", "5"),
    "twolevel-rd-shmem": ("1", "1", "5"),
    "twolevel-rsag-shmem": ("2", "1", "5"),
    "twolevel-rd-p2p": ("1", "1", "6"),
}
CVAR_ALIASES = (
    "MPICH_ALLREDUCE_COLLECTIVE_ALGORITHM",
    "MV2_ALLREDUCE_COLLECTIVE_ALGORITHM",
    "MPIR_PARAM_ALLREDUCE_COLLECTIVE_ALGORITHM",
    "MPIR_CVAR_ALLREDUCE_COLLECTIVE_ALGORITHM",
)


class AllreduceProfiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="mv2-launch-test-")
        self.addCleanup(self.tmp.cleanup)
        self.prefix = Path(self.tmp.name) / "MPI prefix with spaces"
        (self.prefix / "bin").mkdir(parents=True)
        # Even a broken dry-run guard cannot accidentally start real MPI.
        (self.prefix / "bin" / "mpiexec").symlink_to("/bin/echo")
        self.env = {
            "PATH": os.defpath,
            "TMPDIR": self.tmp.name,
            "HOSTS": "192.0.2.11,192.0.2.12",
            "HCA_MAP": "192.0.2.11=mlx5_1,192.0.2.12=mlx5_3",
            "USER_MAP": "192.0.2.11=user1,192.0.2.12=user2",
            "NP": "256",
            "PPN": "128",
            "MVAPICH2_HOME": str(self.prefix),
            "DRY_RUN": "1",
        }

    def launch(self, mode="ordinary", benchmark="allreduce", **updates):
        return subprocess.run(
            ["bash", str(LAUNCHER), mode, benchmark,
             "-m", "1024:32768", "-i", "1000", "-x", "500", "-f"],
            env={**self.env, **updates}, capture_output=True, text=True,
            timeout=10,
        )

    def command(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        line = next(line for line in result.stdout.splitlines()
                    if line.startswith("MV2_REMOTE_WORKSPACE="))
        return shlex.split(line)

    def settings(self, result):
        args = self.command(result)
        entries = [(args[i + 1], args[i + 2]) for i, arg in enumerate(args)
                   if arg == "-genv"]
        self.assertEqual(len(entries), len(dict(entries)), "duplicate -genv")
        return dict(entries)

    def test_all_profiles_use_identical_settings_for_all_transports(self):
        for profile, (inter, two_level, intra) in PROFILES.items():
            reference = None
            for mode in ("ordinary", "xrc", "hollow"):
                with self.subTest(profile=profile, mode=mode):
                    result = self.launch(mode, ALLREDUCE_PROFILE=profile)
                    settings = self.settings(result)
                    selected = {key: value for key, value in settings.items()
                                if "ALLREDUCE" in key}
                    if reference is None:
                        reference = selected
                    self.assertEqual(selected, reference)
                    self.assertEqual(settings["MV2_INTER_ALLREDUCE_TUNING"], inter)
                    self.assertEqual(settings["MV2_INTER_ALLREDUCE_TUNING_TWO_LEVEL"], two_level)
                    self.assertEqual(settings["MV2_INTRA_ALLREDUCE_TUNING"], intra)
                    self.assertEqual(settings["MV2_USE_SHARED_MEM"], "1")
                    self.assertEqual(settings["MV2_USE_SHMEM_ALLREDUCE"], "1")
                    self.assertEqual(settings["MV2_USE_OLD_ALLREDUCE"], "0")
                    self.assertEqual(settings["MV2_USE_MCAST"], "0")
                    self.assertEqual(settings["MV2_ENABLE_SHARP"], "0")
                    for alias in CVAR_ALIASES:
                        self.assertEqual(settings[alias], "-1")
                    self.assertIn("scope=requested", result.stdout)
                    self.assertIn("np=256 ppn=128", result.stdout)
                    self.assertEqual(self.command(result)[-7:],
                                     ["-m", "1024:32768", "-i", "1000", "-x", "500", "-f"])

    def test_auto_does_not_override_native_algorithm_or_transport_tuning(self):
        for mode in ("ordinary", "xrc", "hollow"):
            for profile in ({}, {"ALLREDUCE_PROFILE": "auto"}):
                with self.subTest(mode=mode, profile=profile):
                    result = self.launch(
                        mode, **profile, MV2_INTER_ALLREDUCE_TUNING="9",
                        MV2_USE_OLD_ALLREDUCE="1", MV2_RNDV_PROTOCOL="RGET",
                        MV2_IBA_EAGER_THRESHOLD="4096",
                    )
                    settings = self.settings(result)
                    self.assertFalse(any("ALLREDUCE" in name for name in settings))
                    self.assertNotIn("MV2_RNDV_PROTOCOL", settings)
                    self.assertNotIn("MV2_IBA_EAGER_THRESHOLD", settings)
                    self.assertIn("profile=auto", result.stdout)

    def test_fixed_profile_leaves_eager_rndv_binding_and_kqps_alone(self):
        result = self.launch(ALLREDUCE_PROFILE="flat-rd", MV2_RNDV_PROTOCOL="RGET",
                             MV2_IBA_EAGER_THRESHOLD="4096", MV2_CPU_MAPPING="144:148")
        settings = self.settings(result)
        for name in ("MV2_RNDV_PROTOCOL", "MV2_IBA_EAGER_THRESHOLD", "MV2_CPU_MAPPING"):
            self.assertNotIn(name, settings)

    def test_conflicting_overrides_fail_before_launch(self):
        conflicts = {
            "MV2_INTER_ALLREDUCE_TUNING": "2",
            "MV2_INTER_ALLREDUCE_TUNING_TWO_LEVEL": "0",
            "MV2_INTRA_ALLREDUCE_TUNING": "6",
            "MV2_USE_OLD_ALLREDUCE": "1",
            "MV2_USE_INDEXED_ALLREDUCE_TUNING": "0",
            "MV2_USE_SHARED_MEM": "0",
            "MV2_USE_SHMEM_ALLREDUCE": "0",
            "MV2_USE_OSU_COLLECTIVES": "0",
            "MV2_USE_ANL_COLLECTIVES": "1",
            "MV2_USE_BLOCKING": "1",
            "MV2_USE_MCAST": "1",
            "MV2_ENABLE_SHARP": "1",
            "MV2_ENABLE_ALLREDUCE_SKIP_SMALL_MESSAGE_TUNING_TABLE_SEARCH": "1",
            "MV2_ENABLE_ALLREDUCE_SKIP_LARGE_MESSAGE_TUNING_TABLE_SEARCH": "1",
            **{alias: "2" for alias in CVAR_ALIASES},
        }
        for name, value in conflicts.items():
            with self.subTest(name=name):
                result = self.launch(ALLREDUCE_PROFILE="twolevel-rd-shmem", **{name: value})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("conflicts", result.stderr)
                self.assertIn(name, result.stderr)
                self.assertNotIn("DRY_RUN hosts=", result.stdout)

    def test_matching_native_override_is_allowed(self):
        self.settings(self.launch(ALLREDUCE_PROFILE="flat-rd",
                                  MV2_INTER_ALLREDUCE_TUNING="1",
                                  MPIR_CVAR_ALLREDUCE_COLLECTIVE_ALGORITHM="-1"))

    def test_alltoall_is_unchanged_and_rejects_accidental_profile(self):
        for mode in ("ordinary", "xrc", "hollow"):
            with self.subTest(mode=mode):
                result = self.launch(mode, "alltoall")
                self.assertFalse(any("ALLREDUCE" in name for name in self.settings(result)))
                self.assertNotIn("ALLREDUCE_CONFIG", result.stdout)
                result = self.launch(mode, "alltoall", ALLREDUCE_PROFILE="flat-rd")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("only to allreduce", result.stderr)

    def test_invalid_profile_and_dry_run_are_rejected(self):
        for update in ({"ALLREDUCE_PROFILE": "typo"}, {"DRY_RUN": "invalid"}):
            with self.subTest(update=update):
                result = self.launch(**update)
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("DRY_RUN hosts=", result.stdout)

    def test_hostfile_is_cleaned_up(self):
        self.settings(self.launch(ALLREDUCE_PROFILE="flat-rd"))
        self.assertEqual(list(Path(self.tmp.name).glob("mv2-hollow-hosts.*")), [])

    def test_non_dry_launch_still_invokes_mpiexec(self):
        result = self.launch(DRY_RUN="0", ALLREDUCE_PROFILE="twolevel-rd-shmem")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("DRY_RUN hosts=", result.stdout)
        self.assertIn("-genv MV2_INTER_ALLREDUCE_TUNING 1", result.stdout)
        self.assertIn("mv2-rank allreduce -m 1024:32768", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
