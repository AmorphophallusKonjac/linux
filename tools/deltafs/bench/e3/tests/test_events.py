# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import copy
import hashlib
import json
import pathlib
import sys
import tempfile
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import events  # noqa: E402


class EventTests(unittest.TestCase):
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
        self.assertEqual(sum(events.expected_counts("run", 3).values()), 2340)
        with self.assertRaises(events.EventError):
            events.generate_events("run", 6)

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
            "63fc2ec0a7af6412ee3c15b8bce5f6e0940d731d2b309d7cf6a3978a055d4f70"
            "d435fd17ace7be8e",
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
