#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Analyze three paired DeltaFS E3 copy-up benchmark result sets."""

from __future__ import annotations

import argparse
import collections
import hashlib
import json
import math
import pathlib
import random
import statistics
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Any, Callable, Iterable

import events


SCHEMA = events.SCHEMA
SEED = events.SEED
DELTAFS_ABI_VERSION = 2
INITIAL_GENERATION = 1
CHECKPOINT_GENERATION = 2
COPYUP_SOURCE = "checkpoint_frozen_upper"
BOOTSTRAP_REPLICATES = 10_000
FS_CONFIGS = ("ext4_noreflink", "xfs_noreflink", "xfs_reflink")
METRICS = ("copyup_bytes", "physical_io_bytes", "physical_io_bytes_corrected")
RAW_FIELDS = frozenset((
    "schema", "run", "sample", "sample_id", "sample_kind", "control_batch",
    "cache_mode", "fs_config", "event_id", "file_size_before", "size_bin",
    "offset", "logical_bytes_changed", "dirty_blocks", "copyup_bytes",
    "shared_bytes", "allocated_bytes_total", "copyup_amplification",
    "sectors_before", "sectors_after", "physical_io_bytes", "settle_timeout",
    "pre_sha256", "post_sha256", "upper_sha256", "lower_sha256",
    "fiemap_path", "fiemap_block_size", "status", "errno", "invalid_reason",
))
CONTROL_FIELDS = frozenset((
    "schema", "run", "sample", "sample_id", "sample_kind", "cache_mode",
    "fs_config", "control_batch", "replica", "sectors_before", "sectors_after",
    "physical_io_bytes", "settle_timeout", "status", "errno", "invalid_reason",
))
SUMMARY_FIELDS = frozenset((
    "schema", "preset", "completed", "passed", "counts", "control_counts",
    "planned_edits", "planned_controls", "dmesg_failures", "finished_at",
))
MANIFEST_FIELDS = frozenset((
    "schema", "preset", "run_index", "seed", "event_file", "event_file_sha256",
    "event_count",
    "git_commit", "kernel_release", "kernel_config_sha256", "fs_type", "fs_config",
    "fs_uuid", "backing_source", "backing_mount_options", "xfs_info",
    "device_stat", "canonical_device_stat", "overlay_mount_options", "started_at",
    "deltafs_abi_version", "initial_generation", "checkpoint_generation",
    "copyup_source",
    "legal_cells", "warm_count_per_cell", "cold_count_per_cell",
    "independent_runs", "noop_interval", "noop_repetitions", "settle_interval_ms",
    "settle_stable_comparisons", "settle_timeout_ms",
))


class AnalysisError(RuntimeError):
    """An E3 artifact cannot be safely analyzed."""


@dataclass(frozen=True)
class ResultSet:
    root: pathlib.Path
    manifest: dict[str, Any]
    events: list[dict[str, Any]]
    rows: list[dict[str, Any]]
    controls: list[dict[str, Any]]


@dataclass(frozen=True)
class GroupStats:
    fs_config: str
    cache_mode: str
    size_bin: str
    metric: str
    count: int
    p25: float
    p50: float
    p75: float
    p95: float
    ci95_low: float
    ci95_high: float


def is_plain_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, UnicodeError) as exc:
        raise AnalysisError(f"invalid or missing JSON artifact: {path}") from exc
    if not isinstance(value, dict):
        raise AnalysisError(f"JSON artifact is not an object: {path}")
    return value


def read_jsonl(path: pathlib.Path) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    try:
        stream = path.open(encoding="utf-8")
    except FileNotFoundError as exc:
        raise AnalysisError(f"missing JSONL artifact: {path}") from exc
    with stream:
        for line_number, line in enumerate(stream, start=1):
            if not line.strip():
                raise AnalysisError(f"blank JSONL line: {path}:{line_number}")
            try:
                value = json.loads(line)
            except json.JSONDecodeError as exc:
                raise AnalysisError(f"invalid JSONL: {path}:{line_number}: {exc}") from exc
            if not isinstance(value, dict):
                raise AnalysisError(f"JSONL line is not an object: {path}:{line_number}")
            result.append(value)
    return result


def validate_manifest(value: dict[str, Any]) -> None:
    if set(value) != MANIFEST_FIELDS:
        raise AnalysisError(
            f"manifest schema mismatch: missing={sorted(MANIFEST_FIELDS - set(value))} "
            f"extra={sorted(set(value) - MANIFEST_FIELDS)}"
        )
    if value["schema"] != SCHEMA or value["seed"] != SEED or \
            value["preset"] not in events.PRESETS or value["fs_config"] not in FS_CONFIGS:
        raise AnalysisError("manifest schema, seed, preset, or filesystem is invalid")
    if value["deltafs_abi_version"] != DELTAFS_ABI_VERSION or \
            value["initial_generation"] != INITIAL_GENERATION or \
            value["checkpoint_generation"] != CHECKPOINT_GENERATION or \
            value["copyup_source"] != COPYUP_SOURCE:
        raise AnalysisError("manifest does not describe the E3 DeltaFS v2 lifecycle")
    configuration = events.PRESETS[value["preset"]]
    if not is_plain_int(value["run_index"]) or \
            not 1 <= value["run_index"] <= configuration["runs"]:
        raise AnalysisError("manifest run_index is invalid")
    expected = {
        "event_count": sum(events.expected_counts(
            value["preset"], value["run_index"],
        ).values()),
        "legal_cells": [[size * 1024, dirty] for size, dirty in events.legal_cells()],
        "warm_count_per_cell": configuration["warm"],
        "cold_count_per_cell": configuration["cold"],
        "independent_runs": configuration["runs"],
        "noop_interval": 20,
        "noop_repetitions": 3,
        "settle_interval_ms": 100,
        "settle_stable_comparisons": 3,
        "settle_timeout_ms": 10000,
    }
    for key, expected_value in expected.items():
        if value[key] != expected_value:
            raise AnalysisError(f"manifest {key} differs from the E3 contract")
    if value["fs_config"].startswith("xfs_") != (value["fs_type"] == "xfs") or \
            (value["fs_config"] == "ext4_noreflink") != (value["fs_type"] == "ext4"):
        raise AnalysisError("manifest filesystem type/config mismatch")
    if value["event_file"] != "events.jsonl" or \
            not isinstance(value["event_file_sha256"], str) or \
            len(value["event_file_sha256"]) != 64:
        raise AnalysisError("manifest event identity is invalid")


def validate_raw_shape(row: dict[str, Any], fs_config: str, line_number: int) -> None:
    if set(row) != RAW_FIELDS:
        raise AnalysisError(f"raw schema mismatch at line {line_number}")
    integer_fields = (
        "schema", "run", "sample", "control_batch", "file_size_before", "offset",
        "logical_bytes_changed", "dirty_blocks", "copyup_bytes", "shared_bytes",
        "allocated_bytes_total", "sectors_before", "sectors_after",
        "physical_io_bytes", "fiemap_block_size", "errno",
    )
    if any(not is_plain_int(row[field]) for field in integer_fields):
        raise AnalysisError(f"raw numeric type mismatch at line {line_number}")
    if row["schema"] != SCHEMA or row["sample_kind"] != "edit" or \
            row["fs_config"] != fs_config or row["cache_mode"] not in ("warm", "cold") or \
            row["status"] not in ("ok", "invalid", "failed") or \
            not isinstance(row["settle_timeout"], bool) or \
            not isinstance(row["copyup_amplification"], (int, float)) or \
            isinstance(row["copyup_amplification"], bool):
        raise AnalysisError(f"raw categorical type mismatch at line {line_number}")
    if row["invalid_reason"] is not None and not isinstance(row["invalid_reason"], str):
        raise AnalysisError(f"raw invalid_reason type mismatch at line {line_number}")


def validate_control_shape(row: dict[str, Any], fs_config: str, line_number: int) -> None:
    if set(row) != CONTROL_FIELDS:
        raise AnalysisError(f"control schema mismatch at line {line_number}")
    integer_fields = (
        "schema", "run", "sample", "control_batch", "replica", "sectors_before",
        "sectors_after", "physical_io_bytes", "errno",
    )
    if any(not is_plain_int(row[field]) for field in integer_fields):
        raise AnalysisError(f"control numeric type mismatch at line {line_number}")
    if row["schema"] != SCHEMA or row["sample_kind"] != "control" or \
            row["fs_config"] != fs_config or row["cache_mode"] not in ("warm", "cold") or \
            row["status"] not in ("ok", "invalid", "failed") or \
            not isinstance(row["settle_timeout"], bool):
        raise AnalysisError(f"control categorical mismatch at line {line_number}")


def fiemap_summary(path: pathlib.Path, file_size: int) -> tuple[int, int, int, int]:
    value = read_json(path)
    if set(value) != {"schema", "file_size", "block_size", "status", "errno", "extents"} or \
            value["schema"] != SCHEMA or value["file_size"] != file_size or \
            value["status"] != "ok" or value["errno"] != 0 or \
            not is_plain_int(value["block_size"]) or value["block_size"] <= 0 or \
            not isinstance(value["extents"], list) or not value["extents"]:
        raise AnalysisError(f"invalid FIEMAP dump header: {path}")
    cursor = 0
    mapped = shared = unshared = 0
    allowed = 0x00000001 | 0x00001000 | 0x00002000
    for index, extent in enumerate(value["extents"]):
        if not isinstance(extent, dict) or set(extent) != {
                "logical", "physical", "length", "flags"} or any(
                    not is_plain_int(extent[field]) for field in extent
                ):
            raise AnalysisError(f"invalid FIEMAP extent: {path}")
        logical = extent["logical"]
        length = extent["length"]
        flags = extent["flags"]
        if logical < cursor or length <= 0 or logical + length > file_size or \
                flags & ~allowed or bool(flags & 1) != (index + 1 == len(value["extents"])):
            raise AnalysisError(f"unsupported or inconsistent FIEMAP extent: {path}")
        mapped += length
        if flags & 0x2000:
            shared += length
        else:
            unshared += length
        cursor = logical + length
    return mapped, shared, unshared, value["block_size"]


def validate_result_set(result: ResultSet) -> list[str]:
    errors_found: list[str] = []
    event_by_id = {value["event_id"]: value for value in result.events}
    expected_batch: dict[str, int] = {}
    grouped_events: dict[tuple[int, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for event_value in result.events:
        grouped_events[(event_value["run"], event_value["cache_mode"])].append(event_value)
    for group in grouped_events.values():
        for index, event_value in enumerate(group):
            expected_batch[event_value["event_id"]] = index // result.manifest["noop_interval"] + 1
    if len(result.rows) != len(result.events):
        errors_found.append(
            f"{result.manifest['fs_config']}: raw count {len(result.rows)} != "
            f"event count {len(result.events)}"
        )
    seen: set[str] = set()
    sample_ids: set[str] = set()
    fiemap_paths: set[str] = set()
    for line_number, row in enumerate(result.rows, start=1):
        try:
            validate_raw_shape(row, result.manifest["fs_config"], line_number)
        except AnalysisError as exc:
            errors_found.append(str(exc))
            continue
        event = event_by_id.get(row["event_id"])
        if event is None or row["event_id"] in seen or \
                row["sample_id"] in sample_ids or row["fiemap_path"] in fiemap_paths:
            errors_found.append(f"{result.manifest['fs_config']}: missing/duplicate event {row['event_id']}")
            continue
        seen.add(row["event_id"])
        sample_ids.add(row["sample_id"])
        fiemap_paths.add(row["fiemap_path"])
        expected = {
            "run": event["run"], "cache_mode": event["cache_mode"],
            "file_size_before": event["file_size_before"], "size_bin": event["size_bin"],
            "offset": event["offset"], "logical_bytes_changed": event["write_bytes"],
            "dirty_blocks": event["dirty_blocks"],
        }
        if any(row[key] != value for key, value in expected.items()):
            errors_found.append(f"{result.manifest['fs_config']}: event metadata mismatch {row['event_id']}")
        expected_order_id = result.events[line_number - 1]["event_id"] \
            if line_number <= len(result.events) else None
        if row["sample"] != line_number or row["event_id"] != expected_order_id or \
                row["control_batch"] != expected_batch.get(row["event_id"]):
            errors_found.append(f"{result.manifest['fs_config']}: sample order mismatch {row['event_id']}")
        if row["status"] == "ok":
            if row["errno"] or row["settle_timeout"] or row["invalid_reason"] is not None or \
                    row["copyup_bytes"] <= 0 or row["allocated_bytes_total"] <= 0 or \
                    row["sectors_after"] < row["sectors_before"] or \
                    (row["sectors_after"] - row["sectors_before"]) * 512 != row["physical_io_bytes"] or \
                    row["copyup_bytes"] + row["shared_bytes"] != row["allocated_bytes_total"] or \
                    not math.isclose(
                        row["copyup_amplification"],
                        row["copyup_bytes"] / row["logical_bytes_changed"],
                        rel_tol=1e-12,
                    ) or row["pre_sha256"] != event["expected_before_sha256"] or \
                    row["lower_sha256"] != event["expected_before_sha256"] or \
                    row["post_sha256"] != event["expected_after_sha256"] or \
                    row["upper_sha256"] != event["expected_after_sha256"]:
                errors_found.append(f"{result.manifest['fs_config']}: successful oracle mismatch {row['event_id']}")
            fiemap_path = result.root / row["fiemap_path"]
            try:
                fiemap_path.resolve(strict=True).relative_to(result.root)
                mapped, shared, unshared, block_size = fiemap_summary(
                    fiemap_path, row["file_size_before"],
                )
                if (mapped, shared, unshared, block_size) != (
                        row["allocated_bytes_total"], row["shared_bytes"],
                        row["copyup_bytes"], row["fiemap_block_size"]):
                    raise AnalysisError("FIEMAP summary differs from raw")
                if mapped != row["file_size_before"]:
                    raise AnalysisError("dense synthetic file contains a FIEMAP hole")
            except (AnalysisError, OSError, ValueError) as exc:
                errors_found.append(
                    f"{result.manifest['fs_config']}: FIEMAP {row['event_id']}: {exc}"
                )
        else:
            errors_found.append(
                f"{result.manifest['fs_config']}: non-ok edit {row['event_id']}: {row['status']}"
            )
    if seen != set(event_by_id):
        errors_found.append(f"{result.manifest['fs_config']}: raw event set is incomplete")

    expected_controls: set[tuple[int, str, int, int]] = set()
    grouped = collections.defaultdict(list)
    for event in result.events:
        grouped[(event["run"], event["cache_mode"])].append(event)
    for (run, cache), group in grouped.items():
        for batch in range(1, math.ceil(len(group) / result.manifest["noop_interval"]) + 1):
            for replica in range(1, result.manifest["noop_repetitions"] + 1):
                expected_controls.add((run, cache, batch, replica))
    actual_controls: set[tuple[int, str, int, int]] = set()
    for line_number, control in enumerate(result.controls, start=1):
        try:
            validate_control_shape(control, result.manifest["fs_config"], line_number)
        except AnalysisError as exc:
            errors_found.append(str(exc))
            continue
        key = (control["run"], control["cache_mode"],
               control["control_batch"], control["replica"])
        if key in actual_controls:
            errors_found.append(f"{result.manifest['fs_config']}: duplicate control {key}")
        actual_controls.add(key)
        if control["sample"] != line_number or control["status"] != "ok" or \
                control["errno"] or control["settle_timeout"] or \
                control["invalid_reason"] is not None or \
                control["sectors_after"] < control["sectors_before"] or \
                (control["sectors_after"] - control["sectors_before"]) * 512 != \
                control["physical_io_bytes"]:
            errors_found.append(f"{result.manifest['fs_config']}: invalid control {key}")
    if actual_controls != expected_controls:
        errors_found.append(f"{result.manifest['fs_config']}: control schedule is incomplete")
    summary = read_json(result.root / "summary.json")
    status_counts = collections.Counter(row.get("status") for row in result.rows)
    control_status_counts = collections.Counter(row.get("status") for row in result.controls)
    expected_summary_counts = {
        status: status_counts[status] for status in ("ok", "invalid", "failed")
    }
    expected_control_counts = {
        status: control_status_counts[status] for status in ("ok", "invalid", "failed")
    }
    if set(summary) != SUMMARY_FIELDS or summary.get("schema") != SCHEMA or \
            summary.get("preset") != result.manifest["preset"] or \
            not summary.get("completed") or not summary.get("passed") or \
            summary.get("dmesg_failures") or summary.get("counts") != expected_summary_counts or \
            summary.get("control_counts") != expected_control_counts or \
            summary.get("planned_edits") != result.manifest["event_count"] or \
            summary.get("planned_controls") != len(expected_controls):
        errors_found.append(f"{result.manifest['fs_config']}: runner summary did not pass")
    return errors_found


def discover(root: pathlib.Path) -> list[ResultSet]:
    root = root.resolve(strict=True)
    manifest_paths = sorted(
        path for path in root.rglob("manifest.json")
        if "analysis" not in path.relative_to(root).parts
    )
    result_sets: list[ResultSet] = []
    for path in manifest_paths:
        manifest = read_json(path)
        if set(manifest) != MANIFEST_FIELDS:
            continue
        validate_manifest(manifest)
        directory = path.parent
        event_path = directory / manifest["event_file"]
        if hashlib.sha256(event_path.read_bytes()).hexdigest() != manifest["event_file_sha256"]:
            raise AnalysisError(f"event file hash mismatch: {event_path}")
        try:
            event_values = events.read_events(
                event_path, manifest["preset"], manifest["run_index"],
            )
        except events.EventError as exc:
            raise AnalysisError(f"invalid event file {event_path}: {exc}") from exc
        result_sets.append(ResultSet(
            root=directory, manifest=manifest, events=event_values,
            rows=read_jsonl(directory / "raw.jsonl"),
            controls=read_jsonl(directory / "controls.jsonl"),
        ))
    presets = {result.manifest["preset"] for result in result_sets}
    if len(presets) != 1:
        raise AnalysisError("filesystem results do not share one preset")
    preset = next(iter(presets))
    expected_runs = range(1, events.PRESETS[preset]["runs"] + 1)
    by_identity: dict[tuple[str, int], ResultSet] = {}
    for result in result_sets:
        identity = (result.manifest["fs_config"], result.manifest["run_index"])
        if identity in by_identity:
            raise AnalysisError(f"duplicate filesystem/run shard: {identity}")
        by_identity[identity] = result
    expected_identities = {(config, run_index) for config in FS_CONFIGS
                           for run_index in expected_runs}
    if set(by_identity) != expected_identities:
        missing = sorted(expected_identities - set(by_identity))
        extra = sorted(set(by_identity) - expected_identities)
        raise AnalysisError(f"filesystem/run shards mismatch: missing={missing} extra={extra}")
    for run_index in expected_runs:
        hashes = {
            by_identity[(config, run_index)].manifest["event_file_sha256"]
            for config in FS_CONFIGS
        }
        if len(hashes) != 1:
            raise AnalysisError(f"run {run_index} event hashes differ across filesystems")
    return sorted(result_sets, key=lambda result: (
        result.manifest["run_index"],
        FS_CONFIGS.index(result.manifest["fs_config"]),
    ))


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(float(value) for value in values)
    if not ordered:
        raise AnalysisError("percentile of empty values")
    if not 0 <= fraction <= 1:
        raise AnalysisError("percentile fraction is out of range")
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def stable_seed(parts: Iterable[str]) -> int:
    digest = hashlib.sha256("\0".join(parts).encode("utf-8")).digest()
    return SEED ^ int.from_bytes(digest[:8], "little")


def cluster_bootstrap(rows: list[dict[str, Any]], value: Callable[[dict[str, Any]], float],
                      statistic: Callable[[list[float]], float], parts: Iterable[str],
                      replicates: int = BOOTSTRAP_REPLICATES) -> tuple[float, float]:
    by_run: dict[int, list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        by_run[row["run"]].append(row)
    run_ids = sorted(by_run)
    if not run_ids or replicates < 1:
        raise AnalysisError("bootstrap has no clusters or replicates")
    randomizer = random.Random(stable_seed(parts))
    samples: list[float] = []
    for _ in range(replicates):
        values: list[float] = []
        for _ in run_ids:
            source = by_run[randomizer.choice(run_ids)]
            values.extend(value(row) for row in source)
        samples.append(statistic(values))
    return percentile(samples, 0.025), percentile(samples, 0.975)


def weighted_percentile(counts: collections.Counter[float], fraction: float) -> float:
    total = sum(counts.values())
    if total <= 0 or not 0 <= fraction <= 1:
        raise AnalysisError("weighted percentile has no data or invalid fraction")
    position = (total - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    lower_value = upper_value = None
    seen = 0
    for value in sorted(counts):
        next_seen = seen + counts[value]
        if lower_value is None and lower < next_seen:
            lower_value = value
        if upper < next_seen:
            upper_value = value
            break
        seen = next_seen
    if lower_value is None or upper_value is None:
        raise AnalysisError("weighted percentile rank was not found")
    return lower_value + (upper_value - lower_value) * (position - lower)


def cluster_bootstrap_median(rows: list[dict[str, Any]], field: str,
                             parts: Iterable[str],
                             replicates: int = BOOTSTRAP_REPLICATES
                             ) -> tuple[float, float]:
    by_run: dict[int, collections.Counter[float]] = collections.defaultdict(collections.Counter)
    for row in rows:
        by_run[row["run"]][float(row[field])] += 1
    run_ids = sorted(by_run)
    if not run_ids or replicates < 1:
        raise AnalysisError("bootstrap has no clusters or replicates")
    randomizer = random.Random(stable_seed(parts))
    samples: list[float] = []
    for _ in range(replicates):
        selected = collections.Counter(randomizer.choice(run_ids) for _ in run_ids)
        combined: collections.Counter[float] = collections.Counter()
        for run_id, multiplier in selected.items():
            for value, count in by_run[run_id].items():
                combined[value] += count * multiplier
        samples.append(weighted_percentile(combined, 0.5))
    return percentile(samples, 0.025), percentile(samples, 0.975)


def control_baselines(result_sets: list[ResultSet]) -> dict[tuple[str, int, str, int], float]:
    grouped: dict[tuple[str, int, str, int], list[float]] = collections.defaultdict(list)
    for result in result_sets:
        for row in result.controls:
            if row.get("status") == "ok":
                key = (result.manifest["fs_config"], row["run"], row["cache_mode"],
                       row["control_batch"])
                grouped[key].append(float(row["physical_io_bytes"]))
    baselines = {}
    for key, values in grouped.items():
        if len(values) != 3:
            raise AnalysisError(f"control baseline is not a triplet: {key}")
        baselines[key] = statistics.median(values)
    return baselines


def enriched_rows(result_sets: list[ResultSet]) -> list[dict[str, Any]]:
    baselines = control_baselines(result_sets)
    result: list[dict[str, Any]] = []
    for result_set in result_sets:
        for source in result_set.rows:
            row = dict(source)
            key = (row["fs_config"], row["run"], row["cache_mode"], row["control_batch"])
            baseline = baselines.get(key)
            if baseline is None:
                raise AnalysisError(f"missing control baseline: {key}")
            row["noop_median_bytes"] = baseline
            row["physical_io_bytes_corrected"] = max(
                0.0, float(row["physical_io_bytes"]) - baseline,
            )
            result.append(row)
    return result


def calculate_stats(rows: list[dict[str, Any]],
                    replicates: int = BOOTSTRAP_REPLICATES) -> list[GroupStats]:
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        if row["status"] == "ok":
            for metric in METRICS:
                grouped[(row["fs_config"], row["cache_mode"], row["size_bin"], metric)].append(row)
    result = []
    for key in sorted(grouped, key=lambda item: (
            FS_CONFIGS.index(item[0]), ("warm", "cold").index(item[1]),
            list(events.SIZE_BINS.values()).index(item[2]), METRICS.index(item[3]))):
        group = grouped[key]
        metric = key[3]
        values = [float(row[metric]) for row in group]
        low, high = cluster_bootstrap_median(group, metric, (*key, "median"), replicates)
        result.append(GroupStats(
            *key, len(group), percentile(values, 0.25), percentile(values, 0.5),
            percentile(values, 0.75), percentile(values, 0.95), low, high,
        ))
    return result


def paired_rows(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    by_event: dict[str, dict[str, dict[str, Any]]] = collections.defaultdict(dict)
    errors_found: list[str] = []
    for row in rows:
        if row["fs_config"] in by_event[row["event_id"]]:
            errors_found.append(f"duplicate pair: {row['event_id']} {row['fs_config']}")
        by_event[row["event_id"]][row["fs_config"]] = row
    result = []
    for event_id in sorted(by_event):
        group = by_event[event_id]
        missing = sorted(set(FS_CONFIGS) - set(group))
        invalid = sorted(config for config, row in group.items() if row["status"] != "ok")
        if missing or invalid:
            errors_found.append(f"{event_id}\tmissing={missing}\tinvalid={invalid}")
            continue
        comparisons = (
            ("xfs_metadata", "ext4_noreflink", "xfs_noreflink"),
            ("reflink", "xfs_noreflink", "xfs_reflink"),
        )
        for comparison, source_config, target_config in comparisons:
            source = group[source_config]
            target = group[target_config]
            for metric in METRICS:
                source_value = float(source[metric])
                target_value = float(target[metric])
                if target_value:
                    ratio = source_value / target_value
                elif source_value:
                    ratio = math.inf
                else:
                    ratio = 1.0
                result.append({
                    "event_id": event_id, "run": source["run"],
                    "cache_mode": source["cache_mode"], "size_bin": source["size_bin"],
                    "comparison": comparison, "source_config": source_config,
                    "target_config": target_config, "metric": metric,
                    "source_bytes": source_value, "target_bytes": target_value,
                    "bytes_saved": source_value - target_value, "ratio": ratio,
                })
    return result, errors_found


def regression(rows: list[dict[str, Any]], replicates: int = BOOTSTRAP_REPLICATES
               ) -> list[dict[str, Any]]:
    def fit_sums(count: int, sum_x: float, sum_y: float,
                 sum_xx: float, sum_xy: float) -> tuple[float, float]:
        x_mean = sum_x / count
        y_mean = sum_y / count
        denominator = sum_xx - sum_x * sum_x / count
        if not denominator:
            raise AnalysisError("regression has no x variance")
        beta = (sum_xy - sum_x * sum_y / count) / denominator
        return y_mean - beta * x_mean, beta

    grouped: dict[tuple[str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        if row["status"] == "ok" and row["copyup_bytes"] > 0:
            grouped[(row["fs_config"], row["cache_mode"])].append(row)
    output = []
    for key, group in sorted(grouped.items()):
        pairs = [(math.log2(row["file_size_before"]), math.log2(row["copyup_bytes"]))
                 for row in group]
        total = (
            len(pairs), sum(x for x, _ in pairs), sum(y for _, y in pairs),
            sum(x * x for x, _ in pairs), sum(x * y for x, y in pairs),
        )
        alpha, beta = fit_sums(*total)
        by_run: dict[int, tuple[int, float, float, float, float]] = {}
        for run_id in sorted({row["run"] for row in group}):
            run_pairs = [
                (math.log2(row["file_size_before"]), math.log2(row["copyup_bytes"]))
                for row in group if row["run"] == run_id
            ]
            by_run[run_id] = (
                len(run_pairs), sum(x for x, _ in run_pairs),
                sum(y for _, y in run_pairs), sum(x * x for x, _ in run_pairs),
                sum(x * y for x, y in run_pairs),
            )
        run_ids = sorted(by_run)
        randomizer = random.Random(stable_seed((*key, "regression")))
        betas = []
        for _ in range(replicates):
            selected = collections.Counter(randomizer.choice(run_ids) for _ in run_ids)
            sums = [0, 0.0, 0.0, 0.0, 0.0]
            for run_id, multiplier in selected.items():
                for index, value in enumerate(by_run[run_id]):
                    sums[index] += value * multiplier
            betas.append(fit_sums(*sums)[1])
        low, high = percentile(betas, 0.025), percentile(betas, 0.975)
        output.append({"fs_config": key[0], "cache_mode": key[1], "n": len(group),
                       "alpha": alpha, "beta": beta, "beta_ci95_low": low,
                       "beta_ci95_high": high})
    return output


def number(value: float) -> str:
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    return f"{value:.6f}".rstrip("0").rstrip(".")


def write_summary(path: pathlib.Path, stats: list[GroupStats]) -> None:
    lines = ["fs_config\tcache_mode\tsize_bin\tmetric\tn\tp25\tp50\tp75\tp95\tci95_low\tci95_high\n"]
    for item in stats:
        lines.append("\t".join((
            item.fs_config, item.cache_mode, item.size_bin, item.metric, str(item.count),
            number(item.p25), number(item.p50), number(item.p75), number(item.p95),
            number(item.ci95_low), number(item.ci95_high),
        )) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_paired(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    fields = ("event_id", "run", "cache_mode", "size_bin", "comparison",
              "source_config", "target_config", "metric", "source_bytes",
              "target_bytes", "bytes_saved", "ratio")
    lines = ["\t".join(fields) + "\n"]
    for row in rows:
        lines.append("\t".join(
            str(row[field]) if not isinstance(row[field], float) else number(row[field])
            for field in fields
        ) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_paired_summary(path: pathlib.Path, paired: list[dict[str, Any]],
                         replicates: int = BOOTSTRAP_REPLICATES) -> None:
    grouped: dict[tuple[str, str, str, str], list[dict[str, Any]]] = collections.defaultdict(list)
    for row in paired:
        grouped[(row["comparison"], row["cache_mode"], row["size_bin"], row["metric"])].append(row)
    lines = ["comparison\tcache_mode\tsize_bin\tmetric\tn\tp50_bytes_saved\tci95_low\tci95_high\tp50_ratio\n"]
    for key, group in sorted(grouped.items()):
        low, high = cluster_bootstrap_median(
            group, "bytes_saved", (*key, "paired"), replicates,
        )
        lines.append("\t".join((
            *key, str(len(group)), number(statistics.median(row["bytes_saved"] for row in group)),
            number(low), number(high), number(statistics.median(row["ratio"] for row in group)),
        )) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_regression(path: pathlib.Path, values: list[dict[str, Any]]) -> None:
    fields = ("fs_config", "cache_mode", "n", "alpha", "beta",
              "beta_ci95_low", "beta_ci95_high")
    lines = ["\t".join(fields) + "\n"]
    for row in values:
        lines.append("\t".join(
            str(row[field]) if not isinstance(row[field], float) else number(row[field])
            for field in fields
        ) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_sensitivity(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    lines = ["fs_config\trun\tcache_mode\tcontrol_batch\tno_op_median_bytes\tn\traw_p50_bytes\tcorrected_p50_bytes\n"]
    grouped: dict[tuple[str, int, str, int, float], list[dict[str, Any]]] = collections.defaultdict(list)
    for row in rows:
        grouped[(row["fs_config"], row["run"], row["cache_mode"],
                 row["control_batch"], row["noop_median_bytes"])].append(row)
    for key, group in sorted(grouped.items()):
        lines.append("\t".join((
            key[0], str(key[1]), key[2], str(key[3]), number(key[4]), str(len(group)),
            number(statistics.median(row["physical_io_bytes"] for row in group)),
            number(statistics.median(row["physical_io_bytes_corrected"] for row in group)),
        )) + "\n")
    path.write_text("".join(lines), encoding="ascii")


class Canvas:
    def __init__(self, width: int, height: int) -> None:
        self.width = width
        self.height = height
        self.pixels = bytearray((255, 255, 255) * (width * height))

    def set(self, x: int, y: int, color: tuple[int, int, int]) -> None:
        if 0 <= x < self.width and 0 <= y < self.height:
            offset = (y * self.width + x) * 3
            self.pixels[offset:offset + 3] = bytes(color)

    def line(self, x0: int, y0: int, x1: int, y1: int,
             color: tuple[int, int, int], width: int = 1) -> None:
        dx, sx = abs(x1 - x0), 1 if x0 < x1 else -1
        dy, sy = -abs(y1 - y0), 1 if y0 < y1 else -1
        error = dx + dy
        while True:
            for px in range(x0 - width // 2, x0 + width // 2 + 1):
                for py in range(y0 - width // 2, y0 + width // 2 + 1):
                    self.set(px, py, color)
            if x0 == x1 and y0 == y1:
                break
            twice = 2 * error
            if twice >= dy:
                error += dy
                x0 += sx
            if twice <= dx:
                error += dx
                y0 += sy

    def circle(self, x: int, y: int, radius: int, color: tuple[int, int, int]) -> None:
        for py in range(y - radius, y + radius + 1):
            for px in range(x - radius, x + radius + 1):
                if (px - x) ** 2 + (py - y) ** 2 <= radius ** 2:
                    self.set(px, py, color)

    def text(self, x: int, y: int, value: str, color: tuple[int, int, int],
             scale: int = 1) -> None:
        for character in value.upper():
            glyph = FONT.get(character, FONT["?"])
            for row, bits in enumerate(glyph):
                for column in range(5):
                    if bits & (1 << (4 - column)):
                        for offset_y in range(scale):
                            for offset_x in range(scale):
                                self.set(x + column * scale + offset_x,
                                         y + row * scale + offset_y, color)
            x += 6 * scale


FONT = {
    " ": (0, 0, 0, 0, 0, 0, 0), "-": (0, 0, 0, 31, 0, 0, 0),
    "/": (1, 2, 4, 8, 16, 0, 0), "^": (4, 10, 17, 0, 0, 0, 0),
    "?": (14, 17, 1, 2, 4, 0, 4),
    "0": (14, 17, 19, 21, 25, 17, 14), "1": (4, 12, 4, 4, 4, 4, 14),
    "2": (14, 17, 1, 2, 4, 8, 31), "3": (30, 1, 1, 14, 1, 1, 30),
    "4": (2, 6, 10, 18, 31, 2, 2), "5": (31, 16, 16, 30, 1, 1, 30),
    "6": (14, 16, 16, 30, 17, 17, 14), "7": (31, 1, 2, 4, 8, 8, 8),
    "8": (14, 17, 17, 14, 17, 17, 14), "9": (14, 17, 17, 15, 1, 1, 14),
    "A": (14, 17, 17, 31, 17, 17, 17), "B": (30, 17, 17, 30, 17, 17, 30),
    "C": (14, 17, 16, 16, 16, 17, 14), "D": (30, 17, 17, 17, 17, 17, 30),
    "E": (31, 16, 16, 30, 16, 16, 31), "F": (31, 16, 16, 30, 16, 16, 16),
    "G": (14, 17, 16, 23, 17, 17, 15), "H": (17, 17, 17, 31, 17, 17, 17),
    "I": (14, 4, 4, 4, 4, 4, 14), "K": (17, 18, 20, 24, 20, 18, 17),
    "L": (16, 16, 16, 16, 16, 16, 31), "M": (17, 27, 21, 21, 17, 17, 17),
    "N": (17, 25, 21, 21, 19, 17, 17), "O": (14, 17, 17, 17, 17, 17, 14),
    "P": (30, 17, 17, 30, 16, 16, 16), "R": (30, 17, 17, 30, 20, 18, 17),
    "S": (15, 16, 16, 14, 1, 1, 30), "T": (31, 4, 4, 4, 4, 4, 4),
    "U": (17, 17, 17, 17, 17, 17, 14), "W": (17, 17, 17, 21, 21, 21, 10),
    "X": (17, 17, 10, 4, 10, 17, 17), "Y": (17, 17, 10, 4, 4, 4, 4),
}


def png_chunk(kind: bytes, data: bytes) -> bytes:
    payload = kind + data
    return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))


def write_plot(path: pathlib.Path, stats: list[GroupStats], metric: str) -> None:
    width, height = 1000, 600
    left, right, top, bottom = 90, 50, 55, 70
    canvas = Canvas(width, height)
    axis, grid = (40, 40, 40), (220, 224, 228)
    colors = {
        "ext4_noreflink": (20, 105, 170),
        "xfs_noreflink": (210, 130, 25),
        "xfs_reflink": (35, 145, 85),
    }
    values = [item for item in stats if item.cache_mode == "warm" and item.metric == metric]
    maximum = max((item.p50 for item in values), default=1.0)
    maximum = max(maximum, 1.0)
    log_max = max(1.0, math.log2(maximum))
    plot_width, plot_height = width - left - right, height - top - bottom
    for tick in range(6):
        y = top + round(plot_height * tick / 5)
        canvas.line(left, y, width - right, y, grid)
        exponent = log_max * (5 - tick) / 5
        canvas.text(12, y - 4, f"2^{exponent:.0f}", axis)
    canvas.line(left, top, left, height - bottom, axis, 2)
    canvas.line(left, height - bottom, width - right, height - bottom, axis, 2)
    title = "E3 COPYUP BY SIZE" if metric == "copyup_bytes" else "E3 PHYSICAL IO BY SIZE"
    canvas.text(left, 18, title, axis, 2)
    canvas.text(12, top - 18, "BYTES LOG2", axis)
    for index, label in enumerate(("4K", "8-16K", "16-32K", "32-64K", "64-128K", "128-256K")):
        x = left + round(index * plot_width / 5)
        canvas.line(x, height - bottom, x, height - bottom + 6, axis)
        canvas.text(x - len(label) * 3, height - bottom + 12, label, axis)
    for config in FS_CONFIGS:
        series = sorted(
            (item for item in values if item.fs_config == config),
            key=lambda item: list(events.SIZE_BINS.values()).index(item.size_bin),
        )
        previous = None
        for index, item in enumerate(series):
            x = left + round(index * plot_width / 5)
            y = top + round((1 - math.log2(max(item.p50, 1.0)) / log_max) * plot_height)
            if previous is not None:
                canvas.line(previous[0], previous[1], x, y, colors[config], 3)
            canvas.circle(x, y, 5, colors[config])
            previous = (x, y)
    for index, config in enumerate(FS_CONFIGS):
        y = 18 + index * 12
        canvas.line(width - 240, y, width - 200, y, colors[config], 4)
        canvas.text(width - 190, y - 4, config.replace("_", "-"), axis)
    raw = b"".join(
        b"\x00" + bytes(canvas.pixels[y * width * 3:(y + 1) * width * 3])
        for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(raw, 9))
    png += png_chunk(b"IEND", b"")
    path.write_bytes(png)


def analyze(root: pathlib.Path, replicates: int = BOOTSTRAP_REPLICATES
            ) -> tuple[list[GroupStats], list[str]]:
    result_sets = discover(root)
    analysis_dir = root.resolve() / "analysis"
    analysis_dir.mkdir(exist_ok=True)
    errors_found: list[str] = []
    for result in result_sets:
        errors_found.extend(validate_result_set(result))
    if errors_found:
        rows = []
    else:
        try:
            rows = enriched_rows(result_sets)
        except AnalysisError as exc:
            errors_found.append(str(exc))
            rows = []
    paired, pairing_errors = paired_rows(rows)
    errors_found.extend(pairing_errors)
    stats = calculate_stats(rows, replicates) if rows else []
    regressions = regression(rows, replicates) if rows else []
    write_summary(analysis_dir / "summary.tsv", stats)
    write_paired(analysis_dir / "paired-benefit.tsv", paired)
    write_paired_summary(analysis_dir / "paired-benefit-summary.tsv", paired, replicates)
    write_regression(analysis_dir / "regression.tsv", regressions)
    write_sensitivity(analysis_dir / "noop-sensitivity.tsv", rows)
    (analysis_dir / "pairing-errors.tsv").write_text(
        "error\n" + "".join(f"{error}\n" for error in errors_found), encoding="utf-8",
    )
    with (analysis_dir / "invalid.jsonl").open("w", encoding="utf-8") as stream:
        for result in result_sets:
            for kind, source in (("edit", result.rows), ("control", result.controls)):
                for row in source:
                    if row.get("status") != "ok":
                        json.dump({"artifact": kind, **row}, stream, sort_keys=True,
                                  separators=(",", ":"))
                        stream.write("\n")
    write_plot(analysis_dir / "copyup-by-size.png", stats, "copyup_bytes")
    write_plot(analysis_dir / "physical-io-by-size.png", stats, "physical_io_bytes")
    summary = {
        "schema": SCHEMA, "passed": not errors_found,
        "preset": result_sets[0].manifest["preset"], "result_sets": len(result_sets),
        "paired_events": len({row["event_id"] for row in paired}),
        "statistics_groups": len(stats), "errors": errors_found,
    }
    (analysis_dir / "summary.json").write_text(
        json.dumps(summary, sort_keys=True, indent=2) + "\n", encoding="utf-8",
    )
    return stats, errors_found


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze three paired DeltaFS E3 runs")
    parser.add_argument("results_root", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        stats, errors_found = analyze(arguments.results_root)
    except (AnalysisError, OSError) as exc:
        print(f"FAIL: E3 analysis: {exc}", file=sys.stderr)
        return 1
    if errors_found:
        for error in errors_found[:50]:
            print(f"FAIL: E3 analysis: {error}", file=sys.stderr)
        return 1
    print(f"PASS: E3 analysis completed; groups={len(stats)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
