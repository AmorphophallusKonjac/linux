# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import events  # noqa: E402
import run  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_cli_is_independent_of_e3_device_counter(self) -> None:
        parsed = run.parse_args(["smoke", "/backing", "/out"])
        self.assertEqual(parsed.preset, "smoke")
        self.assertFalse(hasattr(parsed, "device_stat"))

    def test_filesystem_configuration_only_probes_xfs(self) -> None:
        calls: list[list[str]] = []
        original = run.command_output
        run.command_output = lambda command: calls.append(command) or "reflink=0\n"
        try:
            self.assertEqual(
                run.filesystem_configuration({"fstype": "ext4", "target": "/ext4"}),
                ("ext4", ""),
            )
            self.assertEqual(
                run.filesystem_configuration({"fstype": "f2fs", "target": "/f2fs"}),
                ("f2fs", ""),
            )
            config, info = run.filesystem_configuration(
                {"fstype": "xfs", "target": "/xfs"},
            )
        finally:
            run.command_output = original
        self.assertEqual(config, "xfs_noreflink")
        self.assertEqual(info, "reflink=0\n")
        self.assertEqual(calls, [["xfs_info", "/xfs"]])

    def test_supported_filesystem_magics_include_f2fs(self) -> None:
        self.assertEqual(run.FILESYSTEM_MAGICS["f2fs"], 0xF2F52010)

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

    def test_helper_measures_reopen(self) -> None:
        helper = E3 / "temporal_write_bench"
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
                process = subprocess.run([
                    str(helper), "held_fd", str(root), str(target), "0", "17",
                    "8192", "4096", str(root / "held.json"),
                ], check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                self.assertNotEqual(process.returncode, 0)
            finally:
                os.sched_setaffinity(0, original_affinity)

    def test_lower_direct_replays_event_without_overlay(self) -> None:
        class SubprocessLogs:
            def subprocess(self, command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.run(
                    command, text=True, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    **kwargs,
                )

        event = events.generate_events("smoke")[0]
        original_affinity = os.sched_getaffinity(0)
        os.sched_setaffinity(0, {min(original_affinity)})
        with tempfile.TemporaryDirectory() as temporary:
            try:
                root = pathlib.Path(temporary)
                sample = root / "direct"
                rows = run.make_lower_direct_rows(
                    [event], sample, E3 / "temporal_write_bench",
                    SubprocessLogs(), "reopen", root / "setup.jsonl",  # type: ignore[arg-type]
                )
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["sample_kind"], "lower_direct")
                self.assertEqual(
                    (rows[0]["checkpoint_generation_before"],
                     rows[0]["checkpoint_generation_after"]),
                    (0, 0),
                )
                target = sample / "base" / event["relative_path"]
                self.assertEqual(run.sha256_file(target), event["expected_after_sha256"])
                self.assertFalse((sample / "merged" / event["relative_path"]).exists())
            finally:
                os.sched_setaffinity(0, original_affinity)

    def test_timed_helper_excludes_checkpoint_syncfs_and_hash(self) -> None:
        source = (E3 / "temporal_write_bench.c").read_text(encoding="ascii")
        self.assertNotIn("ioctl(", source)
        self.assertNotIn("syncfs(", source)
        self.assertIn("CLOCK_MONOTONIC_RAW", source)
        self.assertLess(source.index("fill_e3_bytes(payload_seed"),
                        source.index("getrusage(RUSAGE_SELF, &usage_before"))

    def test_new_dmesg_handles_prefix_and_rotation(self) -> None:
        self.assertEqual(run.new_dmesg("a\nb\n", "a\nb\nc\n"), "c\n")
        self.assertEqual(run.new_dmesg("a\nb\nc", "b\nc\nd"), "d")
        with self.assertRaises(run.E3Error):
            run.new_dmesg("a", "z")


if __name__ == "__main__":
    unittest.main()
