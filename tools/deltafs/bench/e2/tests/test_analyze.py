# SPDX-License-Identifier: GPL-2.0

import copy
import errno
import json
import pathlib
import sys
import tempfile
import unittest


E2_DIR = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E2_DIR))
import analyze  # noqa: E402


def smoke_manifest() -> dict:
    return {
        "schema": 1,
        "preset": "smoke",
        "seed": 14857,
        "git_commit": "0" * 40,
        "kernel_release": "test",
        "kernel_config_sha256": "1" * 64,
        "fs_type": "xfs",
        "fs_uuid": "uuid",
        "backing_source": "/dev/test",
        "backing_mount_options": "rw",
        "deltafs_mount_options": [
            "index=off", "nfs_export=off", "metacopy=off", "xino=off",
            "uuid=off", "redirect_dir=nofollow",
        ],
        "cpu": 3,
        "clocksource": "kvm-clock",
        "started_at": "2026-08-13T00:00:00Z",
        "depth_matrix": {"checkpoint": [1, 127], "restore": [1, 128]},
        "warmup_count": 2,
        "measured_count": 5,
        "independent_runs": 1,
    }


def row(sample: int, operation: str, depth: int, warmup: bool) -> dict:
    source = depth if operation == "checkpoint" else 128
    target = depth + 1 if operation == "checkpoint" else depth
    return {
        "schema": 1,
        "run": 1,
        "sample": sample,
        "warmup": warmup,
        "operation": operation,
        "source_depth": source,
        "target_depth": target,
        "request_depth": target,
        "rollback_distance": 0 if operation == "checkpoint" else 128 - depth,
        "expected_generation": 1,
        "generation_after": 2,
        "cpu_before": 3,
        "cpu_after": 3,
        "major_faults": 0,
        "ioctl_ret": 0,
        "errno": 0,
        "ioctl_latency_ns": 100_000 + sample,
        "status": "ok",
        "invalid_reason": None,
    }


def smoke_rows() -> list[dict]:
    rows = [{
        "schema": 1,
        "run": 1,
        "sample": 1,
        "warmup": False,
        "operation": "checkpoint",
        "source_depth": 128,
        "target_depth": 129,
        "request_depth": 129,
        "rollback_distance": 0,
        "expected_generation": 1,
        "generation_after": 1,
        "cpu_before": -1,
        "cpu_after": -1,
        "major_faults": 0,
        "ioctl_ret": -1,
        "errno": errno.E2BIG,
        "ioctl_latency_ns": 0,
        "status": "expected_reject",
        "invalid_reason": None,
    }]
    sample = 2
    for operation, depths in (("checkpoint", (1, 127)), ("restore", (1, 128))):
        for depth in depths:
            for _ in range(2):
                rows.append(row(sample, operation, depth, True))
                sample += 1
            for _ in range(5):
                rows.append(row(sample, operation, depth, False))
                sample += 1
    return rows


def write_run(root: pathlib.Path, rows: list[dict]) -> None:
    (root / "manifest.json").write_text(
        json.dumps(smoke_manifest()), encoding="utf-8"
    )
    with (root / "raw.jsonl").open("w", encoding="utf-8") as stream:
        for item in rows:
            stream.write(json.dumps(item) + "\n")
    (root / "summary.json").write_text(
        json.dumps({
            "schema": 1,
            "completed": True,
            "passed": True,
            "counts": {
                "ok": sum(item["status"] == "ok" for item in rows),
                "expected_reject": sum(
                    item["status"] == "expected_reject" for item in rows
                ),
                "invalid": sum(item["status"] == "invalid" for item in rows),
                "failed": sum(item["status"] == "failed" for item in rows),
            },
            "dmesg_failures": [],
        }),
        encoding="utf-8",
    )


class AnalyzerTests(unittest.TestCase):
    def test_complete_smoke_generates_all_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            write_run(root, smoke_rows())
            stats, errors = analyze.analyze(root)
            self.assertEqual(errors, [])
            self.assertEqual(len(stats), 4)
            self.assertTrue(all(item.count == 5 for item in stats))
            analysis_dir = root / "analysis"
            for name in (
                "summary.tsv", "latency-vs-depth.tsv", "errno-counts.tsv",
                "invalid.jsonl", "latency-vs-depth.png",
            ):
                self.assertTrue((analysis_dir / name).is_file(), name)
            self.assertEqual(
                (analysis_dir / "latency-vs-depth.png").read_bytes()[:8],
                b"\x89PNG\r\n\x1a\n",
            )
            invalid = (analysis_dir / "invalid.jsonl").read_text(encoding="utf-8")
            self.assertIn('"status":"expected_reject"', invalid)

    def test_missing_sample_is_rejected_but_diagnostics_are_written(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            rows = smoke_rows()
            rows.pop()
            write_run(root, rows)
            _, errors = analyze.analyze(root)
            self.assertTrue(any("sample matrix mismatch" in error for error in errors))
            self.assertTrue((root / "analysis/errno-counts.tsv").is_file())

    def test_invalid_ok_semantics_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            rows = smoke_rows()
            rows[2] = copy.deepcopy(rows[2])
            rows[2]["generation_after"] = 1
            write_run(root, rows)
            _, errors = analyze.analyze(root)
            self.assertTrue(any("successful-sample contract" in error for error in errors))

    def test_unknown_raw_field_is_rejected(self):
        item = smoke_rows()[0]
        item["extra"] = 1
        with self.assertRaises(analyze.AnalysisError):
            analyze.validate_raw_row(item, 1)


if __name__ == "__main__":
    unittest.main()
