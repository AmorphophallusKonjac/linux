# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import statistics
import string
import sys
import tempfile
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import analyze  # noqa: E402
import events  # noqa: E402
import run  # noqa: E402


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="ascii")


def write_jsonl(path: pathlib.Path, values: list[dict]) -> None:
    path.write_text("".join(
        json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n"
        for value in values
    ), encoding="ascii")


def manifest(config: str, event_hash: str, event_count: int) -> dict:
    fs_type = "ext4" if config.startswith("ext4") else "xfs"
    return {
        "schema": events.SCHEMA, "preset": "smoke", "seed": events.SEED,
        "event_file": "events.jsonl", "event_file_sha256": event_hash,
        "event_count": event_count, "git_commit": "test", "kernel_release": "test",
        "kernel_config_sha256": "", "fs_type": fs_type, "fs_config": config,
        "fs_uuid": "test", "backing_source": f"/dev/{config}",
        "backing_mount_options": "rw", "xfs_info": "" if fs_type == "ext4" else
        f"reflink={1 if config == 'xfs_reflink' else 0}",
        "device_stat": "/sys/block/test/stat",
        "canonical_device_stat": "/sys/devices/test/stat",
        "overlay_mount_options": list(run.MOUNT_FEATURES), "started_at": "test",
        "deltafs_abi_version": run.DELTAFS_ABI_VERSION,
        "initial_generation": run.INITIAL_GENERATION,
        "checkpoint_generation": run.CHECKPOINT_GENERATION,
        "copyup_source": run.COPYUP_SOURCE,
        "legal_cells": [[size * 1024, dirty] for size, dirty in events.legal_cells()],
        "samples_per_cell": 2,
        "independent_workloads": 1, "noop_interval": 20, "noop_repetitions": 3,
        "settle_interval_ms": 100, "settle_stable_comparisons": 3,
        "settle_timeout_ms": 10000,
    }


def create_result(root: pathlib.Path, config: str, event_values: list[dict]) -> None:
    directory = root / config
    fiemap_dir = directory / "fiemap"
    fiemap_dir.mkdir(parents=True)
    event_data = events.canonical_jsonl(event_values)
    (directory / "events.jsonl").write_bytes(event_data)
    write_json(directory / "manifest.json", manifest(
        config, hashlib.sha256(event_data).hexdigest(), len(event_values),
    ))
    rows = []
    for number, event in enumerate(event_values, start=1):
        fiemap_name = f"sample-{number:03d}.json"
        write_json(fiemap_dir / fiemap_name, {
            "schema": events.SCHEMA, "file_size": event["file_size_before"], "block_size": 4096,
            "status": "ok", "errno": 0,
            "extents": [{"logical": 0, "physical": number * 4096,
                         "length": event["file_size_before"], "flags": 1}],
        })
        rows.append({
            "schema": events.SCHEMA, "workload": event["workload"], "sample": number,
            "sample_id": f"sample-{number:03d}", "sample_kind": "edit",
            "control_batch": (number - 1) // run.NOOP_INTERVAL + 1,
            "fs_config": config, "event_id": event["event_id"],
            "file_size_before": event["file_size_before"], "size_bin": event["size_bin"],
            "offset": event["offset"], "logical_bytes_changed": event["write_bytes"],
            "dirty_blocks": event["dirty_blocks"],
            "copyup_bytes": event["file_size_before"], "shared_bytes": 0,
            "allocated_bytes_total": event["file_size_before"],
            "copyup_amplification": event["file_size_before"] / event["write_bytes"],
            "sectors_before": 1000, "sectors_after": 1008, "physical_io_bytes": 4096,
            "settle_timeout": False, "pre_sha256": event["expected_before_sha256"],
            "post_sha256": event["expected_after_sha256"],
            "upper_sha256": event["expected_after_sha256"],
            "lower_sha256": event["expected_before_sha256"],
            "fiemap_path": f"fiemap/{fiemap_name}", "fiemap_block_size": 4096,
            "status": "ok", "errno": 0, "invalid_reason": None,
        })
    write_jsonl(directory / "raw.jsonl", rows)
    controls = []
    sample = 0
    for batch in range(1, 3):
        for replica in range(1, 4):
            sample += 1
            controls.append({
                "schema": events.SCHEMA, "workload": 1, "sample": sample,
                "sample_id": f"control-{batch}-{replica}",
                "sample_kind": "control",
                "fs_config": config, "control_batch": batch, "replica": replica,
                "sectors_before": 2000, "sectors_after": 2002,
                "physical_io_bytes": 1024, "settle_timeout": False,
                "status": "ok", "errno": 0, "invalid_reason": None,
            })
    write_jsonl(directory / "controls.jsonl", controls)
    write_json(directory / "summary.json", {
        "schema": events.SCHEMA, "preset": "smoke", "completed": True, "passed": True,
        "counts": {"ok": len(rows), "invalid": 0, "failed": 0},
        "control_counts": {"ok": len(controls), "invalid": 0, "failed": 0},
        "planned_edits": len(rows), "planned_controls": len(controls),
        "dmesg_failures": [], "finished_at": "test",
    })


class AnalyzeTests(unittest.TestCase):
    def test_plot_font_and_log_scale_cover_amplification_charts(self) -> None:
        self.assertFalse(set(string.ascii_uppercase) - set(analyze.FONT))
        low, high, ticks = analyze.log2_plot_bounds([1, 85])
        self.assertLess(low, 0)
        self.assertGreater(high, 7)
        self.assertEqual(ticks, list(range(0, 8)))
        self.assertEqual(analyze.format_amplification_tick(0), "1X")
        self.assertEqual(analyze.format_amplification_tick(3), "8X")
        self.assertEqual(analyze.format_amplification_tick(-1), "1/2X")
        self.assertEqual(set(analyze.FILE_SIZE_STYLES), set(analyze.FILE_SIZE_ORDER))
        self.assertEqual(len({
            style["label"] for style in analyze.FILE_SIZE_STYLES.values()
        }), len(analyze.FILE_SIZE_ORDER))
        self.assertTrue(all("AMPLIFICATION" in title and "WRITE SIZE" in title
                            for title in analyze.PLOT_TITLES.values()))
        self.assertTrue(all("WARM" not in title and "CACHE" not in title
                            for title in analyze.PLOT_TITLES.values()))

    def test_amplification_stats_group_exact_cell(self) -> None:
        rows = [
            {
                "status": "ok", "fs_config": "xfs_reflink",
                "file_size_before": 12288, "logical_bytes_changed": 4096,
                "copyup_bytes": 4096, "physical_io_bytes": physical,
            }
            for physical in (8192, 12288)
        ]
        stats = analyze.calculate_amplification_stats(rows)
        self.assertEqual(len(stats), 2)
        copyup = next(item for item in stats
                      if item.metric == "copyup_amplification")
        physical = next(item for item in stats
                        if item.metric == "physical_write_amplification")
        self.assertEqual(copyup.count, 2)
        self.assertEqual(copyup.p50, 1.0)
        self.assertEqual(physical.count, 2)
        self.assertEqual(physical.p50, 2.5)

    def test_percentile_and_bootstrap(self) -> None:
        self.assertEqual(analyze.percentile([1, 2, 3, 4], 0.5), 2.5)
        rows = [{"workload": 1, "value": value} for value in (1, 2, 3)]
        first = analyze.cluster_bootstrap(
            rows, lambda row: row["value"], statistics.median, ("test",), 50,
        )
        second = analyze.cluster_bootstrap(
            rows, lambda row: row["value"], statistics.median, ("test",), 50,
        )
        self.assertEqual(first, second)

    def test_paired_join_reports_missing_and_uses_event_delta(self) -> None:
        rows = []
        for config, value in (("ext4_noreflink", 10), ("xfs_noreflink", 8),
                              ("xfs_reflink", 3)):
            rows.append({
                "event_id": "e", "workload": 1, "size_bin": "4KiB",
                "fs_config": config, "status": "ok", "copyup_bytes": value,
                "physical_io_bytes": value, "physical_io_bytes_corrected": value,
            })
        paired, errors_found = analyze.paired_rows(rows)
        self.assertFalse(errors_found)
        metadata = next(row for row in paired if row["comparison"] == "xfs_metadata"
                        and row["metric"] == "copyup_bytes")
        reflink = next(row for row in paired if row["comparison"] == "reflink"
                       and row["metric"] == "copyup_bytes")
        self.assertEqual(metadata["bytes_saved"], 2)
        self.assertEqual(reflink["bytes_saved"], 5)
        _, errors_found = analyze.paired_rows(rows[:-1])
        self.assertTrue(errors_found)

    def test_end_to_end_smoke_artifacts(self) -> None:
        event_values = events.generate_events("smoke")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for config in analyze.FS_CONFIGS:
                create_result(root, config, event_values)
            stats, errors_found = analyze.analyze(root, replicates=20)
            self.assertFalse(errors_found)
            self.assertEqual(len(stats), 54)
            analysis_dir = root / "analysis"
            expected = {
                "summary.tsv", "paired-benefit.tsv", "paired-benefit-summary.tsv",
                "regression.tsv", "noop-sensitivity.tsv", "pairing-errors.tsv",
                "amplification-summary.tsv", "invalid.jsonl",
                "copyup-amplification-by-write-size.png",
                "physical-write-amplification-by-write-size.png", "summary.json",
            }
            self.assertEqual({path.name for path in analysis_dir.iterdir()}, expected)
            self.assertTrue(json.loads(
                (analysis_dir / "summary.json").read_text(encoding="utf-8")
            )["passed"])
            self.assertGreater(
                (analysis_dir / "copyup-amplification-by-write-size.png").stat().st_size,
                1000,
            )
            header = (analysis_dir / "amplification-summary.tsv").read_text(
                encoding="ascii",
            ).splitlines()[0]
            self.assertNotIn("cache_mode", header)

    def test_event_hash_mismatch_is_rejected(self) -> None:
        event_values = events.generate_events("smoke")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for config in analyze.FS_CONFIGS:
                create_result(root, config, event_values)
            value = read = json.loads(
                (root / "xfs_reflink" / "manifest.json").read_text(encoding="ascii")
            )
            value = copy.deepcopy(read)
            value["event_file_sha256"] = "0" * 64
            write_json(root / "xfs_reflink" / "manifest.json", value)
            with self.assertRaises(analyze.AnalysisError):
                analyze.discover(root)

    def test_non_v2_manifest_is_rejected(self) -> None:
        event_values = events.generate_events("smoke")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for config in analyze.FS_CONFIGS:
                create_result(root, config, event_values)
            path = root / "ext4_noreflink" / "manifest.json"
            value = json.loads(path.read_text(encoding="ascii"))
            value["deltafs_abi_version"] = 1
            write_json(path, value)
            with self.assertRaisesRegex(analyze.AnalysisError, "v2 lifecycle"):
                analyze.discover(root)


if __name__ == "__main__":
    unittest.main()
