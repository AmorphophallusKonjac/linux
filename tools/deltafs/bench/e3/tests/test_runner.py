# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import pathlib
import tempfile
import sys
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import events  # noqa: E402
import run  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_smoke_batches_cover_each_event(self) -> None:
        values = events.generate_events("smoke")
        batches = run.planned_batches(values)
        flattened = [event for _, _, _, group in batches for event in group]
        self.assertEqual(flattened, values)
        self.assertEqual(len(batches), 2)
        self.assertEqual([(item[0], item[1], item[2]) for item in batches],
                         [(1, "warm", 1), (1, "cold", 1)])
        self.assertTrue(all(len(item[3]) == 18 for item in batches))

    def test_run_batch_boundaries_and_controls(self) -> None:
        values = [
            {"run": run_number, "cache_mode": cache_mode}
            for run_number in range(1, 6)
            for cache_mode, count in (("warm", 1800), ("cold", 540))
            for _ in range(count)
        ]
        batches = run.planned_batches(values)
        self.assertEqual(sum(len(group) for _, _, _, group in batches), 11_700)
        self.assertTrue(all(1 <= len(group) <= run.NOOP_INTERVAL
                            for _, _, _, group in batches))
        # Per run: ceil(1800/20) warm + ceil(540/20) cold = 117 batches.
        self.assertEqual(len(batches), 585)
        self.assertEqual(len(batches) * run.NOOP_REPETITIONS, 1755)

    def test_one_full_run_shard_has_fixed_counts(self) -> None:
        counts = events.expected_counts("run", 4)
        self.assertEqual(sum(counts.values()), 2340)
        warm = sum(count for (run_index, cache, _, _), count in counts.items()
                   if run_index == 4 and cache == "warm")
        cold = sum(count for (run_index, cache, _, _), count in counts.items()
                   if run_index == 4 and cache == "cold")
        self.assertEqual((warm, cold), (1800, 540))
        batches = (warm + 19) // 20 + (cold + 19) // 20
        self.assertEqual(batches * run.NOOP_REPETITIONS, 351)

    def test_new_dmesg_accepts_prefix_and_rotation_overlap(self) -> None:
        self.assertEqual(run.new_dmesg("a\nb\n", "a\nb\nc\n"), "c\n")
        self.assertEqual(run.new_dmesg("a\nb\nc", "b\nc\nd"), "d")
        with self.assertRaises(run.E3Error):
            run.new_dmesg("a\nb", "c\nd")

    def test_summarize_requires_complete_controls(self) -> None:
        values = events.generate_events("smoke")
        manifest = {
            "preset": "smoke", "event_count": len(values), "independent_runs": 1,
            "warm_count_per_cell": 1, "cold_count_per_cell": 1,
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

    def test_sample_preimage_remains_writable_for_copyup(self) -> None:
        event = events.generate_events("smoke")[0]
        with tempfile.TemporaryDirectory() as temporary:
            sample = pathlib.Path(temporary) / "sample"
            original_syncfs = run.syncfs
            run.syncfs = lambda _path: None
            try:
                run.create_sample(sample, event)
            finally:
                run.syncfs = original_syncfs
            target = sample / "lower" / "edit.bin"
            self.assertEqual(target.stat().st_mode & 0o777, 0o644)
            self.assertEqual(run.sha256_file(target), event["expected_before_sha256"])


if __name__ == "__main__":
    unittest.main()
