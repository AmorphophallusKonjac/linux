# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import pathlib
import hashlib
import json
import sys
import tempfile
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import analyze  # noqa: E402
import events  # noqa: E402


def create_result(root: pathlib.Path, config: str, values: list[dict]) -> None:
    directory = root / config
    directory.mkdir()
    event_data = events.canonical_jsonl(values)
    (directory / "events.jsonl").write_bytes(event_data)
    manifest = {
        "schema": events.SCHEMA, "experiment": events.EXPERIMENT,
        "preset": "smoke", "deltafs_abi_version": 2, "fs_config": config,
        "fs_type": config if config in ("ext4", "f2fs") else "xfs",
        "access_mode": "reopen", "direct_baseline": "lower_direct",
        "sweep": "temporal",
        "event_file_sha256": hashlib.sha256(event_data).hexdigest(),
    }
    (directory / "manifest.json").write_text(
        json.dumps(manifest, sort_keys=True) + "\n", encoding="ascii",
    )
    (directory / "summary.json").write_text(
        json.dumps({"passed": True}) + "\n", encoding="ascii",
    )
    with (directory / "raw.jsonl").open("w", encoding="ascii") as stream:
        for event in values:
            numerator = "steady_after_copyup" if \
                event["workload_family"] == "burst4" and event["write_index"] > 0 \
                else "first_touch"
            for kind, latency in (("lower_direct", 50), (numerator, 200),
                                  ("upper_resident", 100)):
                row = {
                    **event, "sample_kind": kind, "access_mode": "reopen",
                    "checkpoint_generation_before": 0 if kind == "lower_direct" else event["generation"],
                    "checkpoint_generation_after": 0 if kind == "lower_direct" else event["generation"] + 1,
                    "pre_sha256": event["expected_before_sha256"],
                    "post_sha256": event["expected_after_sha256"],
                    "open_ns": 10, "pwrite_only_ns": 20, "fsync_ns": latency - 40,
                    "close_ns": 10, "edit_e2e_ns": latency, "cpu_before": 1,
                    "cpu_after": 1, "major_faults_delta": 0, "errno": 0,
                    "status": "ok", "invalid_reason": None,
                }
                stream.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")


class AnalyzeTests(unittest.TestCase):
    def test_percentile_and_stats(self) -> None:
        self.assertEqual(analyze.percentile([1, 2, 3, 4], 0.5), 2.5)
        value = analyze.stats([1, 2, 3])
        self.assertEqual(value["n"], 3)
        self.assertEqual(value["mean"], 2)

    def test_paired_amplification_is_per_event(self) -> None:
        rows = []
        for event_id, first, baseline in (("a", 20, 10), ("b", 9, 3)):
            common = {
                "fs_config": "xfs_reflink", "event_id": event_id,
                "sequence_id": "sequence", "workload_family": "single",
                "generation": 1, "history_depth": 1, "access_mode": "reopen",
            }
            rows.append({**common, "sample_kind": "first_touch", "edit_e2e_ns": first})
            rows.append({**common, "sample_kind": "upper_resident", "edit_e2e_ns": baseline})
        paired = analyze.paired_amplification(rows)
        self.assertEqual([row["latency_ratio"] for row in paired], [2.0, 3.0])
        self.assertEqual([row["overhead_ns"] for row in paired], [10.0, 6.0])

    def test_direct_overhead_is_paired_per_event(self) -> None:
        rows = []
        common = {
            "fs_config": "ext4", "event_id": "a", "sequence_id": "sequence",
            "workload_family": "single", "generation": 1, "history_depth": 1,
        }
        for kind, latency in (("lower_direct", 10), ("upper_resident", 13),
                              ("first_touch", 25)):
            rows.append({**common, "sample_kind": kind, "edit_e2e_ns": latency})
        paired = analyze.paired_direct_overhead(rows)
        self.assertEqual(
            {(row["sample_kind"], row["latency_ratio"], row["overhead_ns"])
             for row in paired},
            {("upper_resident", 1.3, 3.0), ("first_touch", 2.5, 15.0)},
        )

    def test_end_to_end_smoke_artifacts(self) -> None:
        values = events.generate_events("smoke")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            for config in analyze.FS_CONFIGS:
                create_result(root, config, values)
            errors = analyze.analyze(root)
            self.assertEqual(errors, [])
            analysis_dir = root / "analysis"
            expected = {
                "summary.tsv", "latency-by-generation.tsv",
                "latency-by-history-depth.tsv", "amplification.tsv",
                "deltafs-overhead.tsv", "latency-vs-lower.png",
                "burst-amortization.tsv", "invalid.jsonl", "pairing-errors.tsv",
                "latency-vs-generation.png", "latency-vs-history-depth.png",
                "latency-amplification.png", "summary.json",
            }
            self.assertEqual({path.name for path in analysis_dir.iterdir()}, expected)
            self.assertTrue(json.loads(
                (analysis_dir / "summary.json").read_text(encoding="ascii")
            )["passed"])
            self.assertGreater((analysis_dir / "latency-amplification.png").stat().st_size,
                               1000)


if __name__ == "__main__":
    unittest.main()
