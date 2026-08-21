# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest


E2 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E2))
import events  # noqa: E402


class EventTests(unittest.TestCase):
    def test_run_is_five_cache_neutral_workloads(self) -> None:
        self.assertEqual(events.PRESETS["run"], {
            "experiment": "copyup_matrix", "samples": 130, "workloads": 5,
        })
        self.assertNotIn("cache_mode", events.EVENT_FIELDS)
        self.assertNotIn("cache_mode", events.generate_events("smoke")[0])

    def test_legal_matrix_has_eighteen_cells(self) -> None:
        cells = events.legal_cells()
        self.assertEqual(len(cells), 18)
        self.assertIn((4, 1), cells)
        self.assertNotIn((4, 2), cells)
        self.assertIn((192, 8), cells)

    def test_smoke_is_deterministic_and_complete(self) -> None:
        first = events.generate_events("smoke")
        second = events.generate_events("smoke")
        self.assertEqual(first, second)
        self.assertEqual(len(first), 36)
        self.assertEqual(
            hashlib.sha256(events.canonical_jsonl(first)).hexdigest(),
            hashlib.sha256(events.canonical_jsonl(second)).hexdigest(),
        )
        events.validate_events(first, "smoke")

    def test_run_count(self) -> None:
        self.assertEqual(sum(events.expected_counts("run").values()), 11_700)

    def test_depth_presets_are_matched_and_complete(self) -> None:
        smoke = events.generate_events("depth-smoke")
        self.assertEqual(len(smoke), 24)
        self.assertEqual(sum(events.expected_counts("depth-run").values()), 7_800)
        self.assertEqual(events.DIRECTORY_DEPTHS, (0, 1, 2, 4, 8, 16))
        by_case: dict[str, list[dict]] = {}
        for event in smoke:
            by_case.setdefault(event["case_id"], []).append(event)
        self.assertEqual(len(by_case), 4)
        for variants in by_case.values():
            self.assertEqual(
                {event["directory_depth"] for event in variants},
                set(events.DIRECTORY_DEPTHS),
            )
            self.assertEqual(len({
                (event["file_size_before"], event["offset"], event["payload_seed"],
                 event["expected_before_sha256"], event["expected_after_sha256"])
                for event in variants
            }), 1)
            self.assertEqual(
                {event["relative_path"] for event in variants},
                {events.relative_path(depth) for depth in events.DIRECTORY_DEPTHS},
            )

    def test_hashes_match_images(self) -> None:
        event = events.generate_events("smoke")[0]
        before, after = events.event_images(event)
        self.assertEqual(hashlib.sha256(before).hexdigest(),
                         event["expected_before_sha256"])
        self.assertEqual(hashlib.sha256(after).hexdigest(),
                         event["expected_after_sha256"])
        self.assertEqual(len(before), event["file_size_before"])
        self.assertNotEqual(before, after)

    def test_cross_language_byte_stream_vector(self) -> None:
        self.assertEqual(
            events.byte_stream(17, 40).hex(),
            "6ffdb7e9a58db3f306b146b45727d8a40a3e4c2fdce3f033fe3733100192e88f"
            "3fae9d02e264122d",
        )

    def test_path_traversal_and_hash_are_rejected(self) -> None:
        original = events.generate_events("smoke")[0]
        for path in ("/edit.bin", "../edit.bin", "a/../edit.bin", "./edit.bin"):
            value = copy.deepcopy(original)
            value["relative_path"] = path
            with self.subTest(path=path), self.assertRaises(events.EventError):
                events.validate_event(value)
        bad_hash = copy.deepcopy(original)
        bad_hash["expected_after_sha256"] = "0" * 64
        with self.assertRaises(events.EventError):
            events.validate_event(bad_hash)

    def test_duplicate_and_impossible_cell_are_rejected(self) -> None:
        generated = events.generate_events("smoke")
        with self.assertRaises(events.EventError):
            events.validate_events([generated[0], generated[0]])
        impossible = copy.deepcopy(generated[0])
        impossible["dirty_blocks"] = 2
        impossible["write_bytes"] = 8192
        with self.assertRaises(events.EventError):
            events.validate_event(impossible)

    def test_read_requires_canonical_jsonl(self) -> None:
        generated = events.generate_events("smoke")
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "events.jsonl"
            path.write_bytes(events.canonical_jsonl(generated))
            self.assertEqual(events.read_events(path, "smoke"), generated)
            path.write_text(json.dumps(generated[0], indent=2) + "\n", encoding="ascii")
            with self.assertRaises(events.EventError):
                events.read_events(path)


if __name__ == "__main__":
    unittest.main()
