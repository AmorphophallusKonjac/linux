# SPDX-License-Identifier: GPL-2.0
from __future__ import annotations

import hashlib
import pathlib
import sys
import unittest


E3 = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(E3))
import events  # noqa: E402


class EventTests(unittest.TestCase):
    def test_fixed_presets_and_sequence_counts(self) -> None:
        expected = {
            "smoke": (36, 2646), "run": (1800, 132300),
            "depth-smoke": (30, 210), "depth-run": (1500, 10500),
        }
        self.assertEqual(set(events.PRESETS), set(expected))
        for preset, (sequences, rows) in expected.items():
            configuration = events.PRESETS[preset]
            sequence_count = configuration["runs"] * configuration["samples"] * len(configuration["families"])
            if configuration["sweep"] == "history":
                sequence_count *= len(events.HISTORY_DEPTHS)
                event_count = configuration["runs"] * configuration["samples"] * \
                    len(events.HISTORY_DEPTHS) * sum(
                        events.writes_per_generation(family)
                        for family in configuration["families"]
                    )
            else:
                sequence_count *= len(configuration["generations"])
                event_count = configuration["runs"] * configuration["samples"] * \
                    sum(configuration["generations"]) * sum(
                        events.writes_per_generation(family)
                        for family in configuration["families"]
                    )
            self.assertEqual((sequence_count, event_count), (sequences, rows), preset)

    def test_smoke_is_canonical_and_hash_chained(self) -> None:
        values = events.generate_events("smoke")
        self.assertEqual(len(values), 2646)
        self.assertEqual(len({value["sequence_id"] for value in values}), 36)
        data = events.canonical_jsonl(values)
        self.assertEqual(data, events.canonical_jsonl(values))
        self.assertEqual(len(hashlib.sha256(data).hexdigest()), 64)
        burst = [value for value in values
                 if value["workload_family"] == "burst4" and
                 value["sequence_id"] == next(item["sequence_id"] for item in values
                                               if item["workload_family"] == "burst4")]
        self.assertTrue(burst)
        for before, after in zip(burst, burst[1:]):
            if before["file_id"] == after["file_id"]:
                self.assertEqual(before["expected_after_sha256"],
                                 after["expected_before_sha256"])

    def test_depth_boundary_never_declares_more_than_128_lowers(self) -> None:
        values = events.generate_events("depth-smoke")
        self.assertEqual({value["history_depth"] for value in values},
                         set(events.HISTORY_DEPTHS))
        self.assertEqual(max(value["history_depth"] for value in values), 128)
        self.assertTrue(all(value["generation_count"] == 1 for value in values))


if __name__ == "__main__":
    unittest.main()
