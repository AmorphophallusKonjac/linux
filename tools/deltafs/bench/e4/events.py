#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Deterministic DeltaFS E4 temporal write-latency events."""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
from typing import Any, Iterable


SCHEMA = 1
EXPERIMENT = "temporal_write_latency"
SEED = 14857
BLOCK_SIZE = 4096
GENERATIONS = (1, 2, 4, 8, 16, 32)
HISTORY_DEPTHS = (1, 4, 16, 64, 128)
FAMILIES = ("single", "burst4", "multi16")
EVENT_FIELDS = frozenset((
    "schema", "experiment", "event_id", "pair_id", "sequence_id",
    "sweep", "workload_family", "generation", "history_depth",
    "run", "sample", "generation_count",
    "file_id", "file_size", "offset", "write_bytes", "write_index",
    "payload_seed", "initial_seed", "expected_before_sha256", "expected_after_sha256",
))
PRESETS = {
    "smoke": {"sweep": "temporal", "access_mode": "reopen", "samples": 2,
              "runs": 1, "families": FAMILIES, "generations": GENERATIONS},
    "run": {"sweep": "temporal", "access_mode": "reopen", "samples": 20,
            "runs": 5, "families": FAMILIES, "generations": GENERATIONS},
    "depth-smoke": {"sweep": "history", "access_mode": "reopen", "samples": 2,
                    "runs": 1, "families": FAMILIES, "generations": (1,)},
    "depth-run": {"sweep": "history", "access_mode": "reopen", "samples": 20,
                  "runs": 5, "families": FAMILIES, "generations": (1,)},
    "fd-smoke": {"sweep": "temporal", "access_mode": "held_fd", "samples": 2,
                 "runs": 1, "families": ("single", "burst4"),
                 "generations": GENERATIONS},
    "fd-run": {"sweep": "temporal", "access_mode": "held_fd", "samples": 20,
               "runs": 5, "families": ("single", "burst4"),
               "generations": GENERATIONS},
}


class EventError(ValueError):
    """An E4 event violates the fixed schema."""


class SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & ((1 << 64) - 1)

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & ((1 << 64) - 1)
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & ((1 << 64) - 1)
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & ((1 << 64) - 1)
        return (value ^ (value >> 31)) & ((1 << 64) - 1)

    def shuffle(self, values: list[Any]) -> None:
        for index in range(len(values) - 1, 0, -1):
            selected = self.next() % (index + 1)
            values[index], values[selected] = values[selected], values[index]


def byte_stream(seed: int, length: int) -> bytes:
    if not isinstance(seed, int) or isinstance(seed, bool) or seed < 0:
        raise EventError("payload seed must be a non-negative integer")
    prefix = b"deltafs-e4-v1\0" + struct.pack("<Q", seed & ((1 << 64) - 1))
    block = hashlib.sha256(prefix).hexdigest().encode("ascii")
    return (block * ((length + len(block) - 1) // len(block)))[:length]


def file_geometry(family: str, file_id: int) -> tuple[int, str]:
    if family in ("single", "burst4"):
        if file_id != 0:
            raise EventError("single/burst4 only contain file 0")
        return 256 * 1024, "files/edit.bin"
    if family == "multi16" and 0 <= file_id < 16:
        return 64 * 1024, f"files/dir-{file_id // 4 + 1:02d}/edit-{file_id:02d}.bin"
    raise EventError(f"invalid family/file: {family}/{file_id}")


def writes_per_generation(family: str) -> int:
    return {"single": 1, "burst4": 4, "multi16": 16}[family]


def _sha256(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sequence_events(preset: str, run: int, sample: int, family: str,
                     generation_count: int, history_depth: int,
                     randomizer: SplitMix64) -> list[dict[str, Any]]:
    configuration = PRESETS[preset]
    pair_id = f"e4-{configuration['sweep']}-r{run:02d}-s{sample:03d}-{family}-"
    pair_id += f"g{generation_count:03d}-d{history_depth:03d}"
    sequence_id = pair_id + "-temporal"
    files = range(1 if family != "multi16" else 16)
    contents: dict[int, bytearray] = {
        file_id: bytearray(byte_stream(
            SEED ^ (run << 32) ^ (sample << 16) ^ (file_id << 8),
            file_geometry(family, file_id)[0],
        ))
        for file_id in files
    }
    result: list[dict[str, Any]] = []
    for generation in range(1, generation_count + 1):
        selected = list(files)
        if family == "multi16":
            randomizer.shuffle(selected)
        for write_index, file_id in enumerate(selected):
            file_size, relative_path = file_geometry(family, file_id)
            writes = writes_per_generation(family)
            if family in ("single", "burst4"):
                selected_writes = range(writes)
            else:
                selected_writes = (0,)
            for burst_index in selected_writes:
                positions = file_size // BLOCK_SIZE
                offset = (randomizer.next() % positions) * BLOCK_SIZE
                payload_seed = randomizer.next()
                before = bytes(contents[file_id])
                payload = byte_stream(payload_seed, BLOCK_SIZE)
                after = bytearray(before)
                after[offset:offset + BLOCK_SIZE] = payload
                event_id = f"{pair_id}-g{generation:03d}-f{file_id:02d}-w{burst_index:02d}"
                result.append({
                    "schema": SCHEMA, "experiment": EXPERIMENT,
                    "event_id": event_id, "pair_id": pair_id,
                    "sequence_id": sequence_id, "sweep": configuration["sweep"],
                    "workload_family": family, "generation": generation,
                    "history_depth": history_depth, "file_id": file_id,
                    "run": run, "sample": sample, "generation_count": generation_count,
                    "file_size": file_size, "offset": offset,
                    "write_bytes": BLOCK_SIZE,
                    "write_index": write_index if family == "multi16" else burst_index,
                    "payload_seed": payload_seed,
                    "initial_seed": SEED ^ (run << 32) ^ (sample << 16) ^ (file_id << 8),
                    "expected_before_sha256": _sha256(before),
                    "expected_after_sha256": _sha256(bytes(after)),
                    "relative_path": relative_path,
                })
                contents[file_id] = after
    return result


def generate_events(preset: str) -> list[dict[str, Any]]:
    if preset not in PRESETS:
        raise EventError(f"unknown preset: {preset}")
    configuration = PRESETS[preset]
    result: list[dict[str, Any]] = []
    for run in range(1, configuration["runs"] + 1):
        for sample in range(1, configuration["samples"] + 1):
            for family_index, family in enumerate(configuration["families"]):
                randomizer_seed = SEED ^ (run << 32) ^ (sample << 16) ^ family_index
                if configuration["sweep"] == "temporal":
                    for generation_count in configuration["generations"]:
                        result.extend(_sequence_events(
                            preset, run, sample, family,
                            generation_count, 1, SplitMix64(randomizer_seed),
                        ))
                else:
                    for depth in HISTORY_DEPTHS:
                        result.extend(_sequence_events(
                            preset, run, sample, family, 1, depth,
                            SplitMix64(randomizer_seed),
                        ))
    validate_events(result, preset)
    return result


def validate_event(event: dict[str, Any]) -> None:
    if not isinstance(event, dict) or set(event) != EVENT_FIELDS | {"relative_path"}:
        raise EventError("event fields differ from schema")
    if event["schema"] != SCHEMA or event["experiment"] != EXPERIMENT:
        raise EventError("event schema or experiment is invalid")
    for field in ("event_id", "pair_id", "sequence_id", "sweep", "workload_family",
                  "relative_path", "expected_before_sha256", "expected_after_sha256"):
        if not isinstance(event[field], str) or not event[field].isascii() or not event[field]:
            raise EventError(f"invalid string field: {field}")
    numeric = ("generation", "history_depth", "run", "sample", "generation_count",
               "file_id", "file_size", "offset",
               "write_bytes", "write_index", "payload_seed", "initial_seed")
    if any(not isinstance(event[field], int) or isinstance(event[field], bool)
           for field in numeric):
        raise EventError("event numeric fields are invalid")
    if event["generation"] < 1 or event["history_depth"] not in HISTORY_DEPTHS:
        raise EventError("event generation/depth is invalid")
    if event["run"] < 1 or event["sample"] < 1 or \
            event["generation_count"] not in GENERATIONS or \
            event["generation"] > event["generation_count"]:
        raise EventError("event sequence coordinate is invalid")
    if event["write_bytes"] != BLOCK_SIZE or event["offset"] % BLOCK_SIZE:
        raise EventError("event write geometry is invalid")
    file_size, path = file_geometry(event["workload_family"], event["file_id"])
    if event["file_size"] != file_size or event["relative_path"] != path:
        raise EventError("event file geometry differs from family")
    if event["offset"] + event["write_bytes"] > file_size:
        raise EventError("event write exceeds file")
    for field in ("expected_before_sha256", "expected_after_sha256"):
        value = event[field]
        if len(value) != 64 or any(character not in "0123456789abcdef" for character in value):
            raise EventError(f"invalid hash field: {field}")


def validate_events(values: Iterable[dict[str, Any]], preset: str | None = None) -> None:
    events = list(values)
    ids: set[str] = set()
    for event in events:
        validate_event(event)
        if event["event_id"] in ids:
            raise EventError(f"duplicate event id: {event['event_id']}")
        ids.add(event["event_id"])
    if preset is None:
        return
    configuration = PRESETS[preset]
    expected_sequences = configuration["runs"] * configuration["samples"] * len(configuration["families"])
    expected_sequences *= len(HISTORY_DEPTHS) if configuration["sweep"] == "history" else \
        len(configuration["generations"])
    actual_sequences = len({event["sequence_id"] for event in events})
    if actual_sequences != expected_sequences:
        raise EventError(f"expected {expected_sequences} sequences, got {actual_sequences}")


def canonical_jsonl(values: Iterable[dict[str, Any]]) -> bytes:
    return b"".join(
        (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        for value in values
    )


def read_events(path: pathlib.Path, preset: str | None = None) -> list[dict[str, Any]]:
    values = []
    with path.open(encoding="ascii") as stream:
        for line in stream:
            if not line.strip():
                raise EventError("blank event line")
            value = json.loads(line)
            if not isinstance(value, dict):
                raise EventError("event line is not an object")
            values.append(value)
    validate_events(values, preset)
    if canonical_jsonl(values) != path.read_bytes():
        raise EventError("event file is not canonical JSONL")
    return values
