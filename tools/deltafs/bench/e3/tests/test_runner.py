# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import pathlib
import subprocess
import tempfile
import sys
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import events  # noqa: E402
import run  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_run_cli_has_no_run_index(self) -> None:
        parsed = run.parse_args(["run", "/backing", "/dev/stat", "/out"])
        self.assertFalse(hasattr(parsed, "run_index"))

    def test_xfs_info_uses_mount_target(self) -> None:
        calls: list[list[str]] = []
        original = run.command_output
        run.command_output = lambda command: calls.append(command) or "reflink=1\n"
        try:
            config, info = run.xfs_configuration({"fstype": "xfs", "target": "/mnt"})
        finally:
            run.command_output = original
        self.assertEqual(config, "xfs_reflink")
        self.assertEqual(calls, [["xfs_info", "/mnt"]])

    def test_counter_window_syncs_before_first_read(self) -> None:
        source = (E3 / "copyup_bench.c").read_text(encoding="ascii")
        edit = source[source.index("static int run_edit"):source.index("static int write_result")]
        self.assertLess(edit.index('"syncfs_before"'), edit.index('"settle_before"'))
        self.assertLess(edit.index("syncfs_path(options->merged)"),
                        edit.index("bench_wait_for_stable(options->device_stat"))
        control = source[source.index("static int run_control"):source.index("static int run_edit")]
        self.assertLess(control.index('"syncfs_before"'), control.index('"settle_before"'))

    def test_helper_has_no_cache_mode_or_eviction(self) -> None:
        source = (E3 / "copyup_bench.c").read_text(encoding="ascii")
        self.assertNotIn("cache_mode", source)
        self.assertNotIn("POSIX_FADV_DONTNEED", source)

    def test_smoke_batches_cover_each_event(self) -> None:
        values = events.generate_events("smoke")
        batches = run.planned_batches(values)
        flattened = [event for _, _, group in batches for event in group]
        self.assertEqual(flattened, values)
        self.assertEqual(len(batches), 2)
        self.assertEqual([(item[0], item[1]) for item in batches], [(1, 1), (1, 2)])
        self.assertEqual([len(item[2]) for item in batches], [20, 16])

    def test_run_batch_boundaries_and_controls(self) -> None:
        values = [
            {"workload": run_number}
            for run_number in range(1, 6)
            for _ in range(2340)
        ]
        batches = run.planned_batches(values)
        self.assertEqual(sum(len(group) for _, _, group in batches), 11_700)
        self.assertTrue(all(1 <= len(group) <= run.NOOP_INTERVAL
                            for _, _, group in batches))
        # Per workload: ceil(2340/20) = 117 batches.
        self.assertEqual(len(batches), 585)
        self.assertEqual(len(batches) * run.NOOP_REPETITIONS, 1755)

    def test_one_full_workload_has_fixed_counts(self) -> None:
        counts = events.expected_counts("run")
        self.assertEqual(sum(counts.values()), 11_700)
        workload = sum(count for (workload_index, _, _), count in counts.items()
                       if workload_index == 4)
        self.assertEqual(workload, 2340)
        batches = (workload + 19) // 20
        self.assertEqual(batches * run.NOOP_REPETITIONS, 351)

    def test_new_dmesg_accepts_prefix_and_rotation_overlap(self) -> None:
        self.assertEqual(run.new_dmesg("a\nb\n", "a\nb\nc\n"), "c\n")
        self.assertEqual(run.new_dmesg("a\nb\nc", "b\nc\nd"), "d")
        with self.assertRaises(run.E3Error):
            run.new_dmesg("a\nb", "c\nd")

    def test_summarize_requires_complete_controls(self) -> None:
        values = events.generate_events("smoke")
        manifest = {
            "preset": "smoke", "event_count": len(values), "independent_workloads": 1,
            "samples_per_cell": 2,
        }
        rows = [{"status": "ok"} for _ in values]
        control_count = len(run.planned_batches(values)) * run.NOOP_REPETITIONS
        controls = [{"status": "ok"} for _ in range(control_count)]
        summary = run.summarize(manifest, rows, controls, [], True)
        self.assertTrue(summary["passed"])
        controls.pop()
        self.assertFalse(run.summarize(manifest, rows, controls, [], True)["passed"])

    def test_raw_base_keeps_relative_fiemap_path(self) -> None:
        event = events.generate_events("smoke")[0]
        root = pathlib.Path("/tmp/e3-output")
        row = run.raw_base(event, 1, "sample", "xfs_reflink", 1,
                           root / "fiemap" / "sample.json", root)
        self.assertEqual(row["fiemap_path"], "fiemap/sample.json")
        self.assertEqual(row["logical_bytes_changed"], event["write_bytes"])

    def test_sample_preimage_starts_in_generation_one_upper(self) -> None:
        event = events.generate_events("smoke")[0]
        with tempfile.TemporaryDirectory() as temporary:
            sample = pathlib.Path(temporary) / "sample"
            original_syncfs = run.syncfs
            run.syncfs = lambda _path: None
            try:
                run.create_sample(sample, event)
            finally:
                run.syncfs = original_syncfs
            target = sample / "generation-1" / "upper" / "edit.bin"
            self.assertEqual(target.stat().st_mode & 0o777, 0o644)
            self.assertEqual(run.sha256_file(target), event["expected_before_sha256"])
            self.assertFalse((sample / "generation-2" / "upper" / "edit.bin").exists())

    def test_checkpoint_freezes_generation_one_and_uses_v2_helper(self) -> None:
        class RecordingLogs:
            def __init__(self) -> None:
                self.commands: list[list[str]] = []

            def subprocess(self, command: list[str], **_kwargs: object
                           ) -> subprocess.CompletedProcess[str]:
                self.commands.append(command)
                return subprocess.CompletedProcess(command, 0, "", "")

        event = events.generate_events("smoke")[0]
        with tempfile.TemporaryDirectory() as temporary:
            sample = pathlib.Path(temporary) / "sample"
            original_syncfs = run.syncfs
            run.syncfs = lambda _path: None
            try:
                run.create_sample(sample, event)
            finally:
                run.syncfs = original_syncfs
            logs = RecordingLogs()
            helper = pathlib.Path("/tmp/checkpoint_v2")
            run.checkpoint_sample(sample, helper, logs)  # type: ignore[arg-type]
            frozen = sample / "layers" / "g1" / "edit.bin"
            self.assertEqual(run.sha256_file(frozen), event["expected_before_sha256"])
            self.assertFalse((sample / "generation-1" / "upper").exists())
            self.assertEqual(logs.commands, [[
                str(helper), str(sample / "merged"), "1",
                str(sample / "generation-2" / "upper"),
                str(sample / "generation-2" / "work"),
            ]])


if __name__ == "__main__":
    unittest.main()
