#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Versioned deterministic events for the DeltaFS E3 benchmark."""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
from typing import Any, Iterable


SCHEMA = 1
SEED = 14857
BLOCK_SIZE = 4096
MASK64 = (1 << 64) - 1
FILE_SIZES = (4, 12, 24, 48, 96, 192)
DIRTY_BLOCKS = (1, 2, 4, 8)
SIZE_BINS = {
    4: "4KiB",
    12: "8-16KiB",
    24: "16-32KiB",
    48: "32-64KiB",
    96: "64-128KiB",
    192: "128-256KiB",
}
PRESETS = {
    "smoke": {"warm": 1, "cold": 1, "runs": 1},
    "run": {"warm": 100, "cold": 30, "runs": 5},
}
EVENT_FIELDS = frozenset((
    "schema", "event_id", "run", "cache_mode", "relative_path",
    "file_size_before", "offset", "write_bytes", "payload_seed",
    "expected_before_sha256", "expected_after_sha256", "size_bin",
    "dirty_blocks",
))


class EventError(ValueError):
    """An event or event file violates E3 v1."""


class SplitMix64:
    def __init__(self, seed: int) -> None:
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
        return (value ^ (value >> 31)) & MASK64

    def shuffle(self, values: list[Any]) -> None:
        for index in range(len(values) - 1, 0, -1):
            selected = self.next() % (index + 1)
            values[index], values[selected] = values[selected], values[index]


def byte_stream(seed: int, length: int) -> bytes:
    if not isinstance(seed, int) or isinstance(seed, bool) or not 0 <= seed <= MASK64:
        raise EventError("byte-stream seed must be a uint64")
    if not isinstance(length, int) or isinstance(length, bool) or length < 0:
        raise EventError("byte-stream length must be non-negative")
    prefix = b"deltafs-e3-v1\0" + struct.pack("<Q", seed)
    chunks = []
    remaining = length
    counter = 0
    while remaining:
        digest = hashlib.sha256(prefix + struct.pack("<Q", counter)).digest()
        chunks.append(digest[:remaining])
        remaining -= min(remaining, len(digest))
        counter += 1
    return b"".join(chunks)


def preimage_seed(event_id: str) -> int:
    digest = hashlib.sha256(b"deltafs-e3-preimage\0" + event_id.encode("ascii")).digest()
    return struct.unpack("<Q", digest[:8])[0]


def event_images(event: dict[str, Any]) -> tuple[bytes, bytes]:
    before = byte_stream(preimage_seed(event["event_id"]), event["file_size_before"])
    after = bytearray(before)
    payload = byte_stream(event["payload_seed"], event["write_bytes"])
    start = event["offset"]
    after[start:start + len(payload)] = payload
    return before, bytes(after)


def legal_cells() -> list[tuple[int, int]]:
    return [
        (size_kib, dirty)
        for size_kib in FILE_SIZES
        for dirty in DIRTY_BLOCKS
        if dirty * BLOCK_SIZE <= size_kib * 1024
    ]


def _event(run: int, cache_mode: str, size_kib: int, dirty: int,
           ordinal: int, randomizer: SplitMix64) -> dict[str, Any]:
    file_size = size_kib * 1024
    write_bytes = dirty * BLOCK_SIZE
    positions = file_size // BLOCK_SIZE - dirty + 1
    offset = (randomizer.next() % positions) * BLOCK_SIZE
    payload_seed = randomizer.next()
    event_id = (
        f"e3-r{run:02d}-{cache_mode}-f{size_kib:03d}-"
        f"d{dirty:02d}-n{ordinal:03d}"
    )
    value = {
        "schema": SCHEMA,
        "event_id": event_id,
        "run": run,
        "cache_mode": cache_mode,
        "relative_path": "edit.bin",
        "file_size_before": file_size,
        "offset": offset,
        "write_bytes": write_bytes,
        "payload_seed": payload_seed,
        "expected_before_sha256": "",
        "expected_after_sha256": "",
        "size_bin": SIZE_BINS[size_kib],
        "dirty_blocks": dirty,
    }
    before, after = event_images(value)
    value["expected_before_sha256"] = hashlib.sha256(before).hexdigest()
    value["expected_after_sha256"] = hashlib.sha256(after).hexdigest()
    return value


def generate_events(preset: str, run_index: int | None = None) -> list[dict[str, Any]]:
    if preset not in PRESETS:
        raise EventError(f"unknown preset: {preset}")
    configuration = PRESETS[preset]
    if run_index is None:
        selected_runs = range(1, configuration["runs"] + 1)
    elif not is_plain_int(run_index) or not 1 <= run_index <= configuration["runs"]:
        raise EventError("run_index is outside the preset")
    else:
        selected_runs = (run_index,)
    result: list[dict[str, Any]] = []
    for run in selected_runs:
        for cache_index, cache_mode in enumerate(("warm", "cold"), start=1):
            randomizer = SplitMix64(SEED ^ (run << 32) ^ cache_index)
            group = [
                _event(run, cache_mode, size, dirty, ordinal, randomizer)
                for size, dirty in legal_cells()
                for ordinal in range(configuration[cache_mode])
            ]
            randomizer.shuffle(group)
            result.extend(group)
    validate_events(result, preset, run_index)
    return result


def is_plain_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_relative_path(value: Any) -> None:
    if not isinstance(value, str) or not value or "\0" in value:
        raise EventError("relative_path must be a non-empty string")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or value != path.as_posix() or \
            any(part in ("", ".", "..") for part in path.parts):
        raise EventError(f"relative_path is not normalized and relative: {value!r}")


def _valid_hash(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value
    )


def validate_event(event: dict[str, Any]) -> None:
    if not isinstance(event, dict) or set(event) != EVENT_FIELDS:
        actual = set(event) if isinstance(event, dict) else set()
        raise EventError(
            f"event schema fields differ: missing={sorted(EVENT_FIELDS - actual)} "
            f"extra={sorted(actual - EVENT_FIELDS)}"
        )
    if event["schema"] != SCHEMA or not isinstance(event["event_id"], str) or \
            not event["event_id"].isascii() or not event["event_id"]:
        raise EventError("invalid event schema or ID")
    if event["cache_mode"] not in ("warm", "cold"):
        raise EventError("invalid cache mode")
    _validate_relative_path(event["relative_path"])
    integer_fields = ("run", "file_size_before", "offset", "write_bytes",
                      "payload_seed", "dirty_blocks")
    if any(not is_plain_int(event[field]) for field in integer_fields):
        raise EventError("event numeric fields must be plain integers")
    if event["run"] < 1 or event["file_size_before"] <= 0 or \
            event["offset"] < 0 or event["write_bytes"] <= 0 or \
            not 0 <= event["payload_seed"] <= MASK64 or event["dirty_blocks"] <= 0:
        raise EventError("event numeric field is out of range")
    if event["file_size_before"] % 1024 or event["offset"] % BLOCK_SIZE or \
            event["write_bytes"] % BLOCK_SIZE or \
            event["write_bytes"] != event["dirty_blocks"] * BLOCK_SIZE or \
            event["offset"] > event["file_size_before"] or \
            event["write_bytes"] > event["file_size_before"] - event["offset"]:
        raise EventError("event write geometry is invalid")
    size_kib = event["file_size_before"] // 1024
    if size_kib not in SIZE_BINS or event["size_bin"] != SIZE_BINS[size_kib] or \
            (size_kib, event["dirty_blocks"]) not in legal_cells():
        raise EventError("event matrix cell or size bin is invalid")
    if not _valid_hash(event["expected_before_sha256"]) or \
            not _valid_hash(event["expected_after_sha256"]):
        raise EventError("event hashes are not lowercase SHA-256")
    before, after = event_images(event)
    if hashlib.sha256(before).hexdigest() != event["expected_before_sha256"] or \
            hashlib.sha256(after).hexdigest() != event["expected_after_sha256"]:
        raise EventError("event hash does not match deterministic content")


def expected_counts(preset: str, run_index: int | None = None
                    ) -> dict[tuple[int, str, int, int], int]:
    configuration = PRESETS[preset]
    selected_runs = range(1, configuration["runs"] + 1) \
        if run_index is None else (run_index,)
    return {
        (run, cache, size * 1024, dirty): configuration[cache]
        for run in selected_runs
        for cache in ("warm", "cold")
        for size, dirty in legal_cells()
    }


def validate_events(values: Iterable[dict[str, Any]], preset: str | None = None,
                    run_index: int | None = None) -> None:
    events = list(values)
    identities: set[str] = set()
    counts: dict[tuple[int, str, int, int], int] = {}
    for event in events:
        validate_event(event)
        if event["event_id"] in identities:
            raise EventError(f"duplicate event_id: {event['event_id']}")
        identities.add(event["event_id"])
        key = (event["run"], event["cache_mode"],
               event["file_size_before"], event["dirty_blocks"])
        counts[key] = counts.get(key, 0) + 1
    if preset is not None and counts != expected_counts(preset, run_index):
        raise EventError("event schedule differs from the fixed preset")


def canonical_jsonl(values: Iterable[dict[str, Any]]) -> bytes:
    return b"".join(
        (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        for value in values
    )


def read_events(path: pathlib.Path, preset: str | None = None,
                run_index: int | None = None) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    try:
        stream = path.open(encoding="ascii")
    except (FileNotFoundError, UnicodeError) as exc:
        raise EventError(f"cannot read event file: {path}") from exc
    with stream:
        for line_number, line in enumerate(stream, start=1):
            if not line.strip():
                raise EventError(f"blank event line: {line_number}")
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise EventError(f"invalid event JSON at line {line_number}: {exc}") from exc
            if not isinstance(value, dict):
                raise EventError(f"event line {line_number} is not an object")
            result.append(value)
    validate_events(result, preset, run_index)
    if path.read_bytes() != canonical_jsonl(result):
        raise EventError("event file is not in canonical JSONL form")
    return result
