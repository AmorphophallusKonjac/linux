#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Versioned deterministic events for the DeltaFS E2 benchmark."""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
from typing import Any, Iterable


SCHEMA = 3
SEED = 14857
BLOCK_SIZE = 4096
MASK64 = (1 << 64) - 1
FILE_SIZES = (4, 12, 24, 48, 96, 192)
DIRTY_BLOCKS = (1, 2, 4, 8)
DIRECTORY_DEPTHS = (0, 1, 2, 4, 8, 16)
DEPTH_FILE_SIZES = (4, 192)
SIZE_BINS = {
    4: "4KiB",
    12: "8-16KiB",
    24: "16-32KiB",
    48: "32-64KiB",
    96: "64-128KiB",
    192: "128-256KiB",
}
PRESETS = {
    "smoke": {"experiment": "copyup_matrix", "samples": 2, "workloads": 1},
    "run": {"experiment": "copyup_matrix", "samples": 130, "workloads": 5},
    "depth-smoke": {"experiment": "path_depth", "samples": 2, "workloads": 1},
    "depth-run": {"experiment": "path_depth", "samples": 130, "workloads": 5},
}
EVENT_FIELDS = frozenset((
    "schema", "experiment", "event_id", "case_id", "workload",
    "relative_path", "directory_depth",
    "file_size_before", "offset", "write_bytes", "payload_seed",
    "expected_before_sha256", "expected_after_sha256", "size_bin",
    "dirty_blocks",
))


class EventError(ValueError):
    """An event or event file violates E2 schema 3."""


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
    prefix = b"deltafs-e2-v3\0" + struct.pack("<Q", seed)
    chunks = []
    remaining = length
    counter = 0
    while remaining:
        digest = hashlib.sha256(prefix + struct.pack("<Q", counter)).digest()
        chunks.append(digest[:remaining])
        remaining -= min(remaining, len(digest))
        counter += 1
    return b"".join(chunks)


def preimage_seed(case_id: str) -> int:
    digest = hashlib.sha256(b"deltafs-e2-preimage\0" + case_id.encode("ascii")).digest()
    return struct.unpack("<Q", digest[:8])[0]


def event_images(event: dict[str, Any]) -> tuple[bytes, bytes]:
    before = byte_stream(preimage_seed(event["case_id"]), event["file_size_before"])
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


def relative_path(directory_depth: int) -> str:
    if directory_depth not in DIRECTORY_DEPTHS:
        raise EventError(f"unsupported directory depth: {directory_depth}")
    parents = [f"dir-{index:02d}" for index in range(1, directory_depth + 1)]
    return "/".join((*parents, "edit.bin"))


def experiment_cells(preset: str) -> list[tuple[int, int, int]]:
    configuration = PRESETS[preset]
    if configuration["experiment"] == "copyup_matrix":
        return [(size, dirty, 0) for size, dirty in legal_cells()]
    return [(size, 1, depth) for size in DEPTH_FILE_SIZES for depth in DIRECTORY_DEPTHS]


def _matrix_event(workload: int, size_kib: int, dirty: int,
                  ordinal: int, randomizer: SplitMix64) -> dict[str, Any]:
    file_size = size_kib * 1024
    write_bytes = dirty * BLOCK_SIZE
    positions = file_size // BLOCK_SIZE - dirty + 1
    offset = (randomizer.next() % positions) * BLOCK_SIZE
    payload_seed = randomizer.next()
    event_id = (
        f"e2-w{workload:02d}-f{size_kib:03d}-"
        f"d{dirty:02d}-n{ordinal:03d}"
    )
    value = {
        "schema": SCHEMA,
        "experiment": "copyup_matrix",
        "event_id": event_id,
        "case_id": event_id,
        "workload": workload,
        "relative_path": "edit.bin",
        "directory_depth": 0,
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


def _depth_case(workload: int, size_kib: int, ordinal: int,
                randomizer: SplitMix64) -> list[dict[str, Any]]:
    file_size = size_kib * 1024
    positions = file_size // BLOCK_SIZE
    offset = (randomizer.next() % positions) * BLOCK_SIZE
    payload_seed = randomizer.next()
    case_id = f"e2-depth-w{workload:02d}-f{size_kib:03d}-n{ordinal:03d}"
    variants = []
    hashes: tuple[str, str] | None = None
    for depth in DIRECTORY_DEPTHS:
        value = {
            "schema": SCHEMA,
            "experiment": "path_depth",
            "event_id": f"{case_id}-z{depth:02d}",
            "case_id": case_id,
            "workload": workload,
            "relative_path": relative_path(depth),
            "directory_depth": depth,
            "file_size_before": file_size,
            "offset": offset,
            "write_bytes": BLOCK_SIZE,
            "payload_seed": payload_seed,
            "expected_before_sha256": "",
            "expected_after_sha256": "",
            "size_bin": SIZE_BINS[size_kib],
            "dirty_blocks": 1,
        }
        if hashes is None:
            before, after = event_images(value)
            hashes = (hashlib.sha256(before).hexdigest(), hashlib.sha256(after).hexdigest())
        value["expected_before_sha256"], value["expected_after_sha256"] = hashes
        variants.append(value)
    randomizer.shuffle(variants)
    return variants


def generate_events(preset: str) -> list[dict[str, Any]]:
    if preset not in PRESETS:
        raise EventError(f"unknown preset: {preset}")
    configuration = PRESETS[preset]
    selected_workloads = range(1, configuration["workloads"] + 1)
    result: list[dict[str, Any]] = []
    for workload in selected_workloads:
        randomizer = SplitMix64(SEED ^ (workload << 32))
        if configuration["experiment"] == "copyup_matrix":
            group = [
                _matrix_event(workload, size, dirty, ordinal, randomizer)
                for size, dirty in legal_cells()
                for ordinal in range(configuration["samples"])
            ]
            randomizer.shuffle(group)
        else:
            cases = [
                _depth_case(workload, size, ordinal, randomizer)
                for size in DEPTH_FILE_SIZES
                for ordinal in range(configuration["samples"])
            ]
            randomizer.shuffle(cases)
            group = [event for case in cases for event in case]
        result.extend(group)
    validate_events(result, preset)
    return result


def is_plain_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_relative_path(value: Any) -> pathlib.PurePosixPath:
    if not isinstance(value, str) or not value or "\0" in value:
        raise EventError("relative_path must be a non-empty string")
    path = pathlib.PurePosixPath(value)
    if path.is_absolute() or value != path.as_posix() or \
            any(part in ("", ".", "..") for part in path.parts):
        raise EventError(f"relative_path is not normalized and relative: {value!r}")
    return path


def _valid_hash(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(
        character in "0123456789abcdef" for character in value
    )


def validate_event(event: dict[str, Any], *, verify_images: bool = True) -> None:
    if not isinstance(event, dict) or set(event) != EVENT_FIELDS:
        actual = set(event) if isinstance(event, dict) else set()
        raise EventError(
            f"event schema fields differ: missing={sorted(EVENT_FIELDS - actual)} "
            f"extra={sorted(actual - EVENT_FIELDS)}"
        )
    if event["schema"] != SCHEMA or event["experiment"] not in \
            ("copyup_matrix", "path_depth") or \
            any(not isinstance(event[field], str) or not event[field].isascii() or
                not event[field] for field in ("event_id", "case_id")):
        raise EventError("invalid event schema or ID")
    path = _validate_relative_path(event["relative_path"])
    integer_fields = ("workload", "file_size_before", "offset", "write_bytes",
                      "payload_seed", "dirty_blocks", "directory_depth")
    if any(not is_plain_int(event[field]) for field in integer_fields):
        raise EventError("event numeric fields must be plain integers")
    if event["workload"] < 1 or event["file_size_before"] <= 0 or \
            event["offset"] < 0 or event["write_bytes"] <= 0 or \
            not 0 <= event["payload_seed"] <= MASK64 or event["dirty_blocks"] <= 0 or \
            event["directory_depth"] < 0:
        raise EventError("event numeric field is out of range")
    if len(path.parts) - 1 != event["directory_depth"]:
        raise EventError("relative_path does not match directory_depth")
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
    if event["experiment"] == "copyup_matrix":
        if event["directory_depth"] != 0 or event["relative_path"] != "edit.bin" or \
                event["case_id"] != event["event_id"]:
            raise EventError("copyup_matrix event has path-depth metadata")
    elif size_kib not in DEPTH_FILE_SIZES or event["dirty_blocks"] != 1 or \
            event["write_bytes"] != BLOCK_SIZE or \
            event["directory_depth"] not in DIRECTORY_DEPTHS or \
            event["relative_path"] != relative_path(event["directory_depth"]):
        raise EventError("path_depth event differs from the fixed matrix")
    if not _valid_hash(event["expected_before_sha256"]) or \
            not _valid_hash(event["expected_after_sha256"]):
        raise EventError("event hashes are not lowercase SHA-256")
    if verify_images:
        before, after = event_images(event)
        if hashlib.sha256(before).hexdigest() != event["expected_before_sha256"] or \
                hashlib.sha256(after).hexdigest() != event["expected_after_sha256"]:
            raise EventError("event hash does not match deterministic content")


def expected_counts(preset: str) -> dict[tuple[int, int, int, int], int]:
    configuration = PRESETS[preset]
    return {
        (workload, size * 1024, dirty, depth): configuration["samples"]
        for workload in range(1, configuration["workloads"] + 1)
        for size, dirty, depth in experiment_cells(preset)
    }


def validate_events(values: Iterable[dict[str, Any]], preset: str | None = None) -> None:
    events = list(values)
    identities: set[str] = set()
    counts: dict[tuple[int, int, int, int], int] = {}
    cases: dict[str, list[dict[str, Any]]] = {}
    verified_cases: set[str] = set()
    for event in events:
        validate_event(event, verify_images=event.get("case_id") not in verified_cases)
        verified_cases.add(event["case_id"])
        if event["event_id"] in identities:
            raise EventError(f"duplicate event_id: {event['event_id']}")
        identities.add(event["event_id"])
        key = (event["workload"], event["file_size_before"], event["dirty_blocks"],
               event["directory_depth"])
        counts[key] = counts.get(key, 0) + 1
        cases.setdefault(event["case_id"], []).append(event)
    if preset is not None and counts != expected_counts(preset):
        raise EventError("event schedule differs from the fixed preset")
    if preset is not None:
        experiment = PRESETS[preset]["experiment"]
        if any(event["experiment"] != experiment for event in events):
            raise EventError("event experiment differs from preset")
        if experiment == "path_depth":
            for case_id, variants in cases.items():
                if len(variants) != len(DIRECTORY_DEPTHS) or \
                        {event["directory_depth"] for event in variants} != \
                        set(DIRECTORY_DEPTHS):
                    raise EventError(f"depth case is incomplete: {case_id}")
                shared = {
                    (event["workload"], event["file_size_before"], event["offset"],
                     event["write_bytes"], event["payload_seed"],
                     event["expected_before_sha256"], event["expected_after_sha256"])
                    for event in variants
                }
                if len(shared) != 1:
                    raise EventError(f"depth case variants are not matched: {case_id}")


def canonical_jsonl(values: Iterable[dict[str, Any]]) -> bytes:
    return b"".join(
        (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
        for value in values
    )


def read_events(path: pathlib.Path, preset: str | None = None) -> list[dict[str, Any]]:
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
    validate_events(result, preset)
    if path.read_bytes() != canonical_jsonl(result):
        raise EventError("event file is not in canonical JSONL form")
    return result
