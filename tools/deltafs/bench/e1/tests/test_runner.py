# SPDX-License-Identifier: GPL-2.0

import collections
import errno
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


E1_DIR = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E1_DIR))
import run  # noqa: E402


class PresetTests(unittest.TestCase):
    def test_smoke_attempt_counts_and_depths(self):
        attempts = run.planned_attempts("smoke", 1)
        counts = collections.Counter(
            "negative" if item.negative else
            "warmup" if item.warmup else "measured"
            for item in attempts
        )
        self.assertEqual(counts, {"negative": 1, "warmup": 8, "measured": 20})
        negative = attempts[0]
        self.assertEqual(
            (negative.operation, negative.source_depth, negative.request_depth),
            ("checkpoint", 128, 129),
        )
        measured = collections.Counter(
            (item.operation, item.source_depth, item.request_depth)
            for item in attempts if not item.warmup and not item.negative
        )
        self.assertEqual(measured[("checkpoint", 1, 2)], 5)
        self.assertEqual(measured[("checkpoint", 127, 128)], 5)
        self.assertEqual(measured[("restore", 128, 1)], 5)
        self.assertEqual(measured[("restore", 128, 128)], 5)

    def test_run_attempt_counts(self):
        attempts = run.planned_attempts("run", 3)
        counts = collections.Counter(
            "negative" if item.negative else
            "warmup" if item.warmup else "measured"
            for item in attempts
        )
        self.assertEqual(
            counts,
            {"negative": 1, "warmup": 16 * 20, "measured": 16 * 200},
        )

    def test_lower_order_is_top_to_base(self):
        root = pathlib.Path("/backing/sample")
        self.assertEqual(
            run.source_lower_paths(root, 4),
            [
                root / "layers/l003",
                root / "layers/l002",
                root / "layers/l001",
                root / "base",
            ],
        )

    def test_generation_probe_request_layout(self):
        request = run.invalid_generation_request(2)
        self.assertEqual(len(request), run.REQUEST_SIZE)
        self.assertEqual(request[:8], b"H\x02\x00\x00\x02\x00\x00\x00")
        self.assertEqual(request[24:32], b"\x01\x00\x00\x00\x02\x00\x00\x00")
        self.assertEqual(request[32:36], b"\xff\xff\xff\xff")
        self.assertEqual(request[-32:], b"\x00" * 32)

    def test_mount_option_unescape(self):
        self.assertEqual(
            run.unescape_mount_option(r"/data/path\040with\011space"),
            "/data/path with\tspace",
        )
        self.assertEqual(
            run.split_mount_options(r"lowerdir+=/a\054b,upperdir=/c"),
            [r"lowerdir+=/a\054b", "upperdir=/c"],
        )


class SampleTests(unittest.TestCase):
    def test_switch_once_rejects_129_before_open_or_ioctl(self):
        binary = E1_DIR / "switch_once"
        self.assertTrue(binary.is_file(), "make e1-bench before running tests")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            spec = {
                "schema": 2,
                "operation": "checkpoint",
                "cpu": min(run.os.sched_getaffinity(0)),
                "source_depth": 128,
                "expected_generation": 1,
                "keep_bottom": 0,
                "merged": "/does/not/exist/merged",
                "upper": "/does/not/exist/upper",
                "work": "/does/not/exist/work",
                "lower_prefix": [],
            }
            spec_path = root / "spec.json"
            result_path = root / "result.json"
            spec_path.write_text(json.dumps(spec), encoding="utf-8")
            process = subprocess.run(
                [str(binary), str(spec_path), str(result_path)], check=False
            )
            self.assertEqual(process.returncode, 0)
            result = json.loads(result_path.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "expected_reject")
            self.assertEqual(result["errno"], errno.E2BIG)
            self.assertFalse(result["ioctl_attempted"])

    def test_negative_spec_does_not_rename_active_upper(self):
        attempt = run.Attempt("checkpoint", 128, 129, 129, 0, False, True)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "sample"
            run.create_sample(root, 1, attempt)
            active_upper = root / "active/upper"
            spec_path = run.prepare_spec(root, attempt, 0)
            spec = json.loads(spec_path.read_text(encoding="utf-8"))
            self.assertTrue(active_upper.is_dir())
            self.assertFalse((root / "layers/l128").exists())
            self.assertEqual(spec["source_depth"], 128)
            self.assertEqual(spec["keep_bottom"], 0)
            self.assertEqual(spec["lower_prefix"], [])

    def test_checkpoint_spec_freezes_upper_without_sending_lowers(self):
        attempt = run.Attempt("checkpoint", 2, 3, 3, 0, False)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "sample"
            run.create_sample(root, 1, attempt)
            spec = json.loads(
                run.prepare_spec(root, attempt, 0).read_text(encoding="utf-8")
            )
            self.assertFalse((root / "active/upper").exists())
            self.assertTrue((root / "layers/l002").is_dir())
            self.assertEqual(spec["source_depth"], 2)
            self.assertEqual(spec["keep_bottom"], 0)
            self.assertEqual(spec["lower_prefix"], [])

    def test_restore_spec_keeps_target_bottom_without_prefix_fds(self):
        attempt = run.Attempt("restore", 128, 8, 8, 120, False)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "sample"
            run.create_sample(root, 1, attempt)
            spec = json.loads(
                run.prepare_spec(root, attempt, 0).read_text(encoding="utf-8")
            )
            self.assertEqual(spec["source_depth"], 128)
            self.assertEqual(spec["keep_bottom"], 8)
            self.assertEqual(spec["lower_prefix"], [])

    def test_switch_once_accepts_v2_restore_spec_before_opening_paths(self):
        binary = E1_DIR / "switch_once"
        self.assertTrue(binary.is_file(), "make e1-bench before running tests")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            spec = {
                "schema": 2,
                "operation": "restore",
                "cpu": min(run.os.sched_getaffinity(0)),
                "source_depth": 128,
                "expected_generation": 1,
                "keep_bottom": 8,
                "merged": "/does/not/exist/merged",
                "upper": "/does/not/exist/upper",
                "work": "/does/not/exist/work",
                "lower_prefix": [],
            }
            spec_path = root / "spec.json"
            result_path = root / "result.json"
            spec_path.write_text(json.dumps(spec), encoding="utf-8")
            process = subprocess.run(
                [str(binary), str(spec_path), str(result_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("open switch paths", process.stderr)
            self.assertNotIn("read spec", process.stderr)

    def test_dmesg_suffix_and_overlap(self):
        self.assertEqual(run.new_dmesg("one\ntwo\n", "one\ntwo\nthree\n"), "three\n")
        self.assertEqual(run.new_dmesg("one\ntwo", "two\nthree"), "three")
        with self.assertRaises(run.E1Error):
            run.new_dmesg("one", "two")

    def test_negative_oracle_does_not_probe_generation(self):
        attempt = run.Attempt("checkpoint", 128, 129, 129, 0, False, True)
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "sample"
            run.create_sample(root, 1, attempt)
            before = run.lower_snapshot(root)
            result = {
                "status": "expected_reject",
                "errno": errno.E2BIG,
                "ioctl_attempted": False,
            }
            with mock.patch.object(run, "verify_merged"), \
                    mock.patch.object(run, "probe_generation") as probe:
                self.assertEqual(
                    run.verify_negative(root, 1, attempt, before, result), 1
                )
                probe.assert_not_called()


if __name__ == "__main__":
    unittest.main()
