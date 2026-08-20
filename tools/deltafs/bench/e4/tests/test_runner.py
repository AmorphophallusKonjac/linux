# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


E4 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E4))
import events  # noqa: E402
import run  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_cli_is_independent_of_e3_device_counter(self) -> None:
        parsed = run.parse_args(["smoke", "/backing", "/out"])
        self.assertEqual(parsed.preset, "smoke")
        self.assertFalse(hasattr(parsed, "device_stat"))

    def test_checkpoint_renames_upper_and_uses_next_workdir(self) -> None:
        class RecordingLogs:
            def __init__(self) -> None:
                self.commands: list[list[str]] = []

            def subprocess(self, command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
                self.commands.append(command)
                return subprocess.CompletedProcess(command, 0, "", "")

        with tempfile.TemporaryDirectory() as temporary:
            sample = pathlib.Path(temporary)
            (sample / "active" / "upper").mkdir(parents=True)
            (sample / "active" / "work").mkdir()
            (sample / "merged").mkdir()
            logs = RecordingLogs()
            run.checkpoint(sample, 1, pathlib.Path("/helper"), logs)  # type: ignore[arg-type]
            self.assertTrue((sample / "layers" / "g001").is_dir())
            self.assertTrue((sample / "active" / "work-g002").is_dir())
            self.assertEqual(logs.commands[0][2], "1")

    def test_helper_measures_reopen_and_held_fd(self) -> None:
        helper = E4 / "temporal_write_bench"
        original_affinity = os.sched_getaffinity(0)
        os.sched_setaffinity(0, {min(original_affinity)})
        with tempfile.TemporaryDirectory() as temporary:
            try:
                root = pathlib.Path(temporary)
                target = root / "edit.bin"
                target.write_bytes(b"\0" * 8192)
                reopen_result = root / "reopen.json"
                process = subprocess.run([
                    str(helper), "reopen", str(root), str(target), "0", "17",
                    "8192", "4096", str(reopen_result),
                ], check=False)
                self.assertEqual(process.returncode, 0)
                value = json.loads(reopen_result.read_text(encoding="ascii"))
                self.assertEqual(value["status"], "ok")
                self.assertGreater(value["edit_e2e_ns"], 0)
                self.assertEqual(target.read_bytes()[:4096], events.byte_stream(17, 4096))
                held_result = root / "held.json"
                fd = os.open(target, os.O_RDWR | os.O_CLOEXEC)
                try:
                    process = subprocess.run([
                        str(helper), "held_fd", str(root), str(target), "4096", "18",
                        "8192", "4096", str(fd), str(held_result),
                    ], pass_fds=(fd,), check=False)
                finally:
                    os.close(fd)
                self.assertEqual(process.returncode, 0)
                held = json.loads(held_result.read_text(encoding="ascii"))
                self.assertEqual(held["open_ns"], 0)
                self.assertEqual(held["close_ns"], 0)
            finally:
                os.sched_setaffinity(0, original_affinity)

    def test_timed_helper_excludes_checkpoint_syncfs_and_hash(self) -> None:
        source = (E4 / "temporal_write_bench.c").read_text(encoding="ascii")
        self.assertNotIn("ioctl(", source)
        self.assertNotIn("syncfs(", source)
        self.assertIn("CLOCK_MONOTONIC_RAW", source)
        self.assertLess(source.index("fill_e4_bytes(payload_seed"),
                        source.index("getrusage(RUSAGE_SELF, &usage_before"))

    def test_new_dmesg_handles_prefix_and_rotation(self) -> None:
        self.assertEqual(run.new_dmesg("a\nb\n", "a\nb\nc\n"), "c\n")
        self.assertEqual(run.new_dmesg("a\nb\nc", "b\nc\nd"), "d")
        with self.assertRaises(run.E4Error):
            run.new_dmesg("a", "z")


if __name__ == "__main__":
    unittest.main()
