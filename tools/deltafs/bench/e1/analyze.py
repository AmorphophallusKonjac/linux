#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Analyze versioned DeltaFS E1 switch ioctl latency results."""

from __future__ import annotations

import argparse
import collections
import errno
import json
import math
import pathlib
import random
import statistics
import struct
import sys
import zlib
from dataclasses import dataclass
from typing import Any, Iterable


SCHEMA = 2
DELTAFS_ABI_VERSION = 2
SEED = 14857
MAX_LOWERS = 128
BOOTSTRAP_REPLICATES = 10_000
VALID_STATUSES = frozenset(("ok", "expected_reject", "invalid", "failed"))
RAW_FIELDS = frozenset((
    "schema", "run", "sample", "warmup", "operation", "source_depth",
    "target_depth", "request_depth", "rollback_distance",
    "keep_bottom", "prefix_depth", "request_fd_count",
    "expected_generation", "generation_after", "cpu_before", "cpu_after",
    "major_faults", "ioctl_ret", "errno", "ioctl_latency_ns", "status",
    "invalid_reason",
))


class AnalysisError(RuntimeError):
    """An E1 artifact violates the analysis contract."""


@dataclass(frozen=True)
class GroupStats:
    filesystem: str
    operation: str
    request_depth: int
    count: int
    mean: float
    median: float
    p25: float
    p75: float
    p95: float
    p99: float
    stddev: float
    ci95_low: float
    ci95_high: float


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AnalysisError(f"missing artifact: {path}") from exc
    except json.JSONDecodeError as exc:
        raise AnalysisError(f"invalid JSON artifact: {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise AnalysisError(f"expected a JSON object: {path}")
    return value


def read_jsonl(path: pathlib.Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    try:
        stream = path.open(encoding="utf-8")
    except FileNotFoundError as exc:
        raise AnalysisError(f"missing artifact: {path}") from exc
    with stream:
        for line_number, line in enumerate(stream, start=1):
            if not line.strip():
                raise AnalysisError(f"blank raw line at {line_number}")
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise AnalysisError(
                    f"invalid raw JSON at line {line_number}: {exc}"
                ) from exc
            if not isinstance(row, dict):
                raise AnalysisError(f"raw line {line_number} is not an object")
            validate_raw_row(row, line_number)
            rows.append(row)
    return rows


def is_plain_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def validate_raw_row(row: dict[str, Any], line_number: int) -> None:
    if set(row) != RAW_FIELDS:
        missing = sorted(RAW_FIELDS - set(row))
        extra = sorted(set(row) - RAW_FIELDS)
        raise AnalysisError(
            f"raw line {line_number} schema mismatch: missing={missing} extra={extra}"
        )
    if row["schema"] != SCHEMA or row["operation"] not in ("checkpoint", "restore"):
        raise AnalysisError(f"raw line {line_number} has invalid schema/operation")
    if row["status"] not in VALID_STATUSES or not isinstance(row["warmup"], bool):
        raise AnalysisError(f"raw line {line_number} has invalid status/warmup")
    integer_fields = (
        "run", "sample", "source_depth", "target_depth", "request_depth",
        "rollback_distance", "keep_bottom", "prefix_depth", "request_fd_count",
        "expected_generation", "cpu_before", "cpu_after",
        "major_faults", "ioctl_ret", "errno", "ioctl_latency_ns",
    )
    if any(not is_plain_int(row[field]) for field in integer_fields):
        raise AnalysisError(f"raw line {line_number} has a non-integer numeric field")
    if row["generation_after"] is not None and not is_plain_int(row["generation_after"]):
        raise AnalysisError(f"raw line {line_number} has invalid generation_after")
    if row["invalid_reason"] is not None and not isinstance(row["invalid_reason"], str):
        raise AnalysisError(f"raw line {line_number} has invalid invalid_reason")
    if row["run"] < 1 or row["sample"] < 1 or row["source_depth"] < 1 or \
            row["request_depth"] < 1 or row["ioctl_latency_ns"] < 0:
        raise AnalysisError(f"raw line {line_number} has an out-of-range value")


def validate_manifest(manifest: dict[str, Any]) -> None:
    required = {
        "schema", "deltafs_abi_version", "preset", "seed", "git_commit",
        "kernel_release",
        "kernel_config_sha256", "fs_type", "fs_uuid",
        "backing_source", "backing_mount_options", "deltafs_mount_options",
        "cpu", "clocksource", "started_at", "depth_matrix", "warmup_count",
        "measured_count", "independent_runs",
    }
    if set(manifest) != required:
        raise AnalysisError(
            f"manifest schema mismatch: missing={sorted(required - set(manifest))} "
            f"extra={sorted(set(manifest) - required)}"
        )
    if not is_plain_int(manifest["schema"]) or \
            not is_plain_int(manifest["deltafs_abi_version"]) or \
            not is_plain_int(manifest["seed"]) or \
            manifest["schema"] != SCHEMA or \
            manifest["deltafs_abi_version"] != DELTAFS_ABI_VERSION or \
            manifest["seed"] != SEED:
        raise AnalysisError("manifest schema, ABI, or seed does not match E1 v2")
    if manifest["preset"] not in ("smoke", "run"):
        raise AnalysisError("manifest preset is not smoke or run")
    if manifest["fs_type"] not in ("ext4", "xfs"):
        raise AnalysisError("manifest filesystem is not ext4 or XFS")
    if not isinstance(manifest["depth_matrix"], dict) or \
            set(manifest["depth_matrix"]) != {"checkpoint", "restore"}:
        raise AnalysisError("manifest depth_matrix is invalid")
    if not is_plain_int(manifest["cpu"]) or manifest["cpu"] < 0 or any(
            not is_plain_int(manifest[field]) or manifest[field] < 1
            for field in ("warmup_count", "measured_count", "independent_runs")
    ):
        raise AnalysisError("manifest CPU or sample counts are invalid")
    if any(
            not isinstance(manifest["depth_matrix"][operation], list) or
            any(not is_plain_int(depth) or depth < 1 or depth > MAX_LOWERS
                for depth in manifest["depth_matrix"][operation])
            for operation in ("checkpoint", "restore")
    ):
        raise AnalysisError("manifest depths are invalid")
    expected = {
        "smoke": ((1, 127), (1, 128), 2, 5, 1),
        "run": ((1, 2, 4, 8, 16, 32, 64, 127),
                (1, 2, 4, 8, 16, 32, 64, 128), 20, 200, 5),
    }[manifest["preset"]]
    actual = (
        tuple(manifest["depth_matrix"]["checkpoint"]),
        tuple(manifest["depth_matrix"]["restore"]),
        manifest["warmup_count"], manifest["measured_count"],
        manifest["independent_runs"],
    )
    if actual != expected:
        raise AnalysisError(f"manifest preset matrix differs from E1 v2: {actual}")


def validate_row_semantics(row: dict[str, Any], cpu: int) -> None:
    operation = row["operation"]
    if row["expected_generation"] != 1:
        raise AnalysisError("raw row expected_generation is not 1")
    if operation == "checkpoint":
        if row["target_depth"] != row["source_depth"] + 1 or \
                row["request_depth"] != row["target_depth"] or \
                row["rollback_distance"] != 0 or row["keep_bottom"] != 0 or \
                row["prefix_depth"] != 0 or row["request_fd_count"] != 2:
            raise AnalysisError("checkpoint raw depth semantics are invalid")
    else:
        if row["source_depth"] != MAX_LOWERS or \
                row["target_depth"] != row["request_depth"] or \
                row["rollback_distance"] != MAX_LOWERS - row["target_depth"] or \
                row["keep_bottom"] != row["target_depth"] or \
                row["prefix_depth"] != 0 or row["request_fd_count"] != 2:
            raise AnalysisError("restore raw depth semantics are invalid")
    if row["status"] == "ok":
        if row["ioctl_ret"] != 0 or row["errno"] != 0 or \
                row["generation_after"] != 2 or row["invalid_reason"] is not None or \
                row["cpu_before"] != cpu or row["cpu_after"] != cpu or \
                row["major_faults"] != 0:
            raise AnalysisError("status=ok row violates the successful-sample contract")


def planned_counts(manifest: dict[str, Any]) -> collections.Counter[tuple[Any, ...]]:
    planned: collections.Counter[tuple[Any, ...]] = collections.Counter()
    runs = manifest["independent_runs"]
    warmup = manifest["warmup_count"]
    measured = manifest["measured_count"]
    for run_number in range(1, runs + 1):
        planned[(run_number, "checkpoint", MAX_LOWERS, MAX_LOWERS + 1,
                 MAX_LOWERS + 1, False, "expected_reject")] += 1
        for operation in ("checkpoint", "restore"):
            for depth in manifest["depth_matrix"][operation]:
                source = depth if operation == "checkpoint" else MAX_LOWERS
                target = depth + 1 if operation == "checkpoint" else depth
                request = target
                planned[(run_number, operation, source, target,
                         request, True, "ok")] += warmup
                planned[(run_number, operation, source, target,
                         request, False, "ok")] += measured
    return planned


def actual_counts(rows: Iterable[dict[str, Any]]) -> collections.Counter[tuple[Any, ...]]:
    return collections.Counter(
        (
            row["run"], row["operation"], row["source_depth"],
            row["target_depth"], row["request_depth"], row["warmup"],
            row["status"],
        )
        for row in rows
    )


def plan_errors(manifest: dict[str, Any], rows: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    identities = [(row["run"], row["sample"]) for row in rows]
    duplicates = [identity for identity, count in collections.Counter(identities).items()
                  if count != 1]
    if duplicates:
        errors.append(f"duplicate run/sample identities: {duplicates[:10]}")
    cells = sum(len(manifest["depth_matrix"][operation])
                for operation in ("checkpoint", "restore"))
    samples_per_run = 1 + cells * (
        manifest["warmup_count"] + manifest["measured_count"]
    )
    for run_number in range(1, manifest["independent_runs"] + 1):
        actual_samples = sorted(row["sample"] for row in rows
                                if row["run"] == run_number)
        if actual_samples != list(range(1, samples_per_run + 1)):
            errors.append(f"run {run_number} sample numbers are not contiguous 1..{samples_per_run}")
    for row in rows:
        try:
            validate_row_semantics(row, manifest["cpu"])
        except AnalysisError as exc:
            errors.append(f"run={row['run']} sample={row['sample']}: {exc}")
    planned = planned_counts(manifest)
    actual = actual_counts(rows)
    if actual != planned:
        missing = list((planned - actual).items())[:20]
        extra = list((actual - planned).items())[:20]
        errors.append(f"sample matrix mismatch: missing={missing} extra={extra}")
    negatives = [row for row in rows if row["request_depth"] == MAX_LOWERS + 1]
    if len(negatives) != manifest["independent_runs"] or any(
            row["operation"] != "checkpoint" or row["status"] != "expected_reject"
            or row["errno"] != errno.E2BIG or row["generation_after"] != 1
            or row["ioctl_ret"] != -1 or row["ioctl_latency_ns"] != 0
            or row["cpu_before"] != -1 or row["cpu_after"] != -1
            or row["major_faults"] != 0 or row["invalid_reason"] is not None
            for row in negatives
    ):
        errors.append("checkpoint@128 E2BIG preflight rows are invalid")
    summary = read_json_optional(pathlib.Path(manifest.get("_out_dir", ".")) / "summary.json")
    expected_status_counts = collections.Counter(row["status"] for row in rows)
    if summary is None:
        errors.append("runner summary.json is missing")
    elif summary.get("schema") != SCHEMA or not summary.get("completed") or \
            not summary.get("passed") or summary.get("dmesg_failures") or \
            summary.get("counts") != {
                status: expected_status_counts[status]
                for status in ("ok", "expected_reject", "invalid", "failed")
            }:
        errors.append("runner summary is incomplete or inconsistent with raw.jsonl")
    return errors


def read_json_optional(path: pathlib.Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    return read_json(path)


def percentile(sorted_values: list[float], probability: float) -> float:
    if not sorted_values:
        raise AnalysisError("cannot calculate a percentile of no samples")
    position = (len(sorted_values) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    weight = position - lower
    return sorted_values[lower] * (1.0 - weight) + sorted_values[upper] * weight


def cluster_bootstrap_mean(run_values: dict[int, list[float]],
                           seed: int) -> tuple[float, float]:
    run_ids = sorted(run_values)
    if not run_ids:
        raise AnalysisError("cannot bootstrap an empty group")
    randomizer = random.Random(seed)
    estimates: list[float] = []
    for _ in range(BOOTSTRAP_REPLICATES):
        total = 0.0
        count = 0
        for _cluster in run_ids:
            selected_run = randomizer.choice(run_ids)
            values = run_values[selected_run]
            total += sum(randomizer.choices(values, k=len(values)))
            count += len(values)
        estimates.append(total / count)
    estimates.sort()
    return percentile(estimates, 0.025), percentile(estimates, 0.975)


def calculate_stats(manifest: dict[str, Any],
                    rows: list[dict[str, Any]]) -> list[GroupStats]:
    groups: dict[tuple[str, int], dict[int, list[float]]] = {}
    for row in rows:
        if row["status"] != "ok" or row["warmup"]:
            continue
        key = (row["operation"], row["request_depth"])
        groups.setdefault(key, {}).setdefault(row["run"], []).append(
            float(row["ioctl_latency_ns"])
        )
    results = []
    for (operation, depth), run_values in sorted(groups.items()):
        values = sorted(value for cluster in run_values.values() for value in cluster)
        ci_low, ci_high = cluster_bootstrap_mean(run_values, SEED)
        results.append(GroupStats(
            filesystem=manifest["fs_type"],
            operation=operation,
            request_depth=depth,
            count=len(values),
            mean=statistics.fmean(values),
            median=percentile(values, 0.5),
            p25=percentile(values, 0.25),
            p75=percentile(values, 0.75),
            p95=percentile(values, 0.95),
            p99=percentile(values, 0.99),
            stddev=statistics.stdev(values) if len(values) > 1 else 0.0,
            ci95_low=ci_low,
            ci95_high=ci_high,
        ))
    return results


def number(value: float) -> str:
    return f"{value:.3f}"


def write_summary(path: pathlib.Path, stats: list[GroupStats]) -> None:
    header = (
        "filesystem\toperation\trequest_depth\tn\tmean_ns\tmedian_ns\t"
        "p25_ns\tp75_ns\tp95_ns\tp99_ns\tstddev_ns\tmean_ci95_low_ns\t"
        "mean_ci95_high_ns\n"
    )
    lines = [header]
    for item in stats:
        lines.append("\t".join((
            item.filesystem, item.operation, str(item.request_depth), str(item.count),
            number(item.mean), number(item.median), number(item.p25), number(item.p75),
            number(item.p95), number(item.p99), number(item.stddev),
            number(item.ci95_low), number(item.ci95_high),
        )) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_latency_data(path: pathlib.Path, stats: list[GroupStats]) -> None:
    lines = ["filesystem\toperation\trequest_depth\tn\tmean_us\tci95_low_us\tci95_high_us\n"]
    for item in stats:
        lines.append("\t".join((
            item.filesystem, item.operation, str(item.request_depth), str(item.count),
            number(item.mean / 1000.0), number(item.ci95_low / 1000.0),
            number(item.ci95_high / 1000.0),
        )) + "\n")
    path.write_text("".join(lines), encoding="ascii")


def write_errno_counts(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    counts = collections.Counter((row["status"], row["errno"]) for row in rows)
    lines = ["status\terrno\tcount\n"]
    for (status, error), count in sorted(counts.items()):
        lines.append(f"{status}\t{error}\t{count}\n")
    path.write_text("".join(lines), encoding="ascii")


def write_invalid(path: pathlib.Path, rows: list[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        for row in rows:
            if row["status"] != "ok":
                json.dump(row, stream, sort_keys=True, separators=(",", ":"))
                stream.write("\n")


class Canvas:
    def __init__(self, width: int, height: int,
                 background: tuple[int, int, int] = (255, 255, 255)) -> None:
        self.width = width
        self.height = height
        self.pixels = bytearray(background * (width * height))

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

    def fill_rect(self, x0: int, y0: int, x1: int, y1: int,
                  color: tuple[int, int, int]) -> None:
        for y in range(max(0, y0), min(self.height, y1 + 1)):
            for x in range(max(0, x0), min(self.width, x1 + 1)):
                self.set(x, y, color)

    def dashed_line(self, x0: int, y0: int, x1: int, y1: int,
                    color: tuple[int, int, int], width: int,
                    dash: int, gap: int) -> None:
        length = math.hypot(x1 - x0, y1 - y0)
        if length == 0:
            self.circle(x0, y0, max(1, width // 2), color)
            return
        cursor = 0.0
        while cursor < length:
            end = min(cursor + dash, length)
            start_ratio, end_ratio = cursor / length, end / length
            self.line(
                round(x0 + (x1 - x0) * start_ratio),
                round(y0 + (y1 - y0) * start_ratio),
                round(x0 + (x1 - x0) * end_ratio),
                round(y0 + (y1 - y0) * end_ratio),
                color, width,
            )
            cursor += dash + gap

    def marker(self, x: int, y: int, radius: int, color: tuple[int, int, int],
               shape: str, width: int = 2) -> None:
        if shape == "circle":
            outer, inner = radius ** 2, max(0, radius - width) ** 2
            for py in range(y - radius, y + radius + 1):
                for px in range(x - radius, x + radius + 1):
                    distance = (px - x) ** 2 + (py - y) ** 2
                    if inner <= distance <= outer:
                        self.set(px, py, color)
        elif shape == "square":
            self.line(x - radius, y - radius, x + radius, y - radius, color, width)
            self.line(x + radius, y - radius, x + radius, y + radius, color, width)
            self.line(x + radius, y + radius, x - radius, y + radius, color, width)
            self.line(x - radius, y + radius, x - radius, y - radius, color, width)
        else:
            self.line(x, y - radius, x + radius, y, color, width)
            self.line(x + radius, y, x, y + radius, color, width)
            self.line(x, y + radius, x - radius, y, color, width)
            self.line(x - radius, y, x, y - radius, color, width)

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
    ".": (0, 0, 0, 0, 0, 6, 6), "/": (1, 2, 4, 8, 16, 0, 0),
    "^": (4, 10, 17, 0, 0, 0, 0), "?": (14, 17, 1, 2, 4, 0, 4),
    "0": (14, 17, 19, 21, 25, 17, 14), "1": (4, 12, 4, 4, 4, 4, 14),
    "2": (14, 17, 1, 2, 4, 8, 31), "3": (30, 1, 1, 14, 1, 1, 30),
    "4": (2, 6, 10, 18, 31, 2, 2), "5": (31, 16, 16, 30, 1, 1, 30),
    "6": (14, 16, 16, 30, 17, 17, 14), "7": (31, 1, 2, 4, 8, 8, 8),
    "8": (14, 17, 17, 14, 17, 17, 14), "9": (14, 17, 17, 15, 1, 1, 14),
    "A": (14, 17, 17, 31, 17, 17, 17), "B": (30, 17, 17, 30, 17, 17, 30),
    "C": (14, 17, 16, 16, 16, 17, 14), "D": (30, 17, 17, 17, 17, 17, 30),
    "E": (31, 16, 16, 30, 16, 16, 31), "F": (31, 16, 16, 30, 16, 16, 16),
    "G": (14, 17, 16, 23, 17, 17, 15), "H": (17, 17, 17, 31, 17, 17, 17),
    "I": (14, 4, 4, 4, 4, 4, 14), "J": (7, 2, 2, 2, 2, 18, 12),
    "K": (17, 18, 20, 24, 20, 18, 17), "L": (16, 16, 16, 16, 16, 16, 31),
    "M": (17, 27, 21, 21, 17, 17, 17), "N": (17, 25, 21, 21, 19, 17, 17),
    "O": (14, 17, 17, 17, 17, 17, 14), "P": (30, 17, 17, 30, 16, 16, 16),
    "Q": (14, 17, 17, 17, 21, 18, 13), "R": (30, 17, 17, 30, 20, 18, 17),
    "S": (15, 16, 16, 14, 1, 1, 30), "T": (31, 4, 4, 4, 4, 4, 4),
    "U": (17, 17, 17, 17, 17, 17, 14), "V": (17, 17, 17, 17, 17, 10, 4),
    "W": (17, 17, 17, 21, 21, 21, 10), "X": (17, 17, 10, 4, 10, 17, 17),
    "Y": (17, 17, 10, 4, 4, 4, 4), "Z": (31, 1, 2, 4, 8, 16, 31),
}


def png_chunk(kind: bytes, data: bytes) -> bytes:
    payload = kind + data
    return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))


PLOT_TITLE = "SWITCH IOCTL LATENCY"
PLOT_STYLES: dict[str, dict[str, Any]] = {
    "checkpoint": {
        "color": (0, 114, 178), "label": "CHECKPOINT", "marker": "circle",
        "radius": 5, "width": 3, "dash": None,
    },
    "restore": {
        "color": (213, 94, 0), "label": "RESTORE", "marker": "square",
        "radius": 7, "width": 4, "dash": (13, 7),
    },
}


def text_width(value: str, scale: int = 1) -> int:
    return max(0, len(value) * 6 * scale - scale)


def nice_number(value: float) -> float:
    exponent = math.floor(math.log10(value))
    fraction = value / (10 ** exponent)
    if fraction < 1.5:
        nice = 1.0
    elif fraction < 3.0:
        nice = 2.0
    elif fraction < 7.0:
        nice = 5.0
    else:
        nice = 10.0
    return nice * (10 ** exponent)


def linear_plot_bounds(values: Iterable[float]) -> tuple[float, float, list[float]]:
    finite = [value for value in values if math.isfinite(value)]
    if not finite:
        return 0.0, 1.0, [0.0, 0.5, 1.0]
    low, high = min(finite), max(finite)
    span = max(high - low, 1.0)
    padded_low = max(0.0, low - span * 0.08)
    padded_high = high + span * 0.08
    step = nice_number(max((padded_high - padded_low) / 5.0, 1.0))
    axis_low = math.floor(padded_low / step) * step
    axis_high = math.ceil(padded_high / step) * step
    if axis_low == axis_high:
        axis_high = axis_low + step
    count = int(round((axis_high - axis_low) / step))
    ticks = [axis_low + index * step for index in range(count + 1)]
    return axis_low, axis_high, ticks


def format_latency_tick(value: float, step: float) -> str:
    if step >= 1:
        return str(round(value))
    return f"{value:.1f}"


def styled_line(canvas: Canvas, start: tuple[int, int], end: tuple[int, int],
                style: dict[str, Any]) -> None:
    if style["dash"] is None:
        canvas.line(*start, *end, style["color"], style["width"])
    else:
        canvas.dashed_line(*start, *end, style["color"], style["width"], *style["dash"])


def write_png(path: pathlib.Path, stats: list[GroupStats]) -> None:
    width, height = 1200, 720
    left, right, top, bottom = 110, 55, 105, 110
    canvas = Canvas(width, height, (248, 250, 252))
    axis, muted = (31, 41, 55), (91, 105, 120)
    grid, frame = (221, 227, 234), (183, 193, 204)
    plot_background = (255, 255, 255)
    plot_width, plot_height = width - left - right, height - top - bottom
    values = [item.mean / 1000.0 for item in stats]
    y_low, y_high, ticks = linear_plot_bounds(values)

    def y_position(value: float) -> int:
        return top + round((1.0 - (value - y_low) / (y_high - y_low)) * plot_height)

    def x_position(depth: int) -> int:
        ratio = math.log2(depth) / math.log2(MAX_LOWERS)
        return left + round(ratio * plot_width)

    canvas.fill_rect(left, top, width - right, height - bottom, plot_background)
    for tick in ticks:
        y = y_position(tick)
        canvas.line(left, y, width - right, y, grid)
        label = format_latency_tick(tick, ticks[1] - ticks[0])
        canvas.text(left - text_width(label) - 14, y - 4, label, muted)
    canvas.line(left, top, width - right, top, frame)
    canvas.line(width - right, top, width - right, height - bottom, frame)
    canvas.line(left, top, left, height - bottom, axis, 2)
    canvas.line(left, height - bottom, width - right, height - bottom, axis, 2)
    canvas.text(left, 18, PLOT_TITLE, axis, 2)
    canvas.text(left, 51, "MEAN LATENCY", muted)
    canvas.text(left, top - 24, "IOCTL LATENCY - MICROSECONDS", muted)
    for depth in (1, 2, 4, 8, 16, 32, 64, 128):
        x = x_position(depth)
        canvas.line(x, height - bottom, x, height - bottom + 6, axis)
        label = str(depth)
        canvas.text(x - text_width(label) // 2, height - bottom + 14, label, axis)
    x_caption = "REQUEST DEPTH - LOG2 SCALE"
    canvas.text(left + (plot_width - text_width(x_caption)) // 2,
                height - bottom + 42, x_caption, muted)

    series_by_operation: dict[str, list[GroupStats]] = {}
    for operation in PLOT_STYLES:
        series_by_operation[operation] = sorted(
            (item for item in stats if item.operation == operation),
            key=lambda item: item.request_depth,
        )
    for operation in ("checkpoint", "restore"):
        style = PLOT_STYLES[operation]
        points = [
            (x_position(item.request_depth), y_position(item.mean / 1000.0))
            for item in series_by_operation[operation]
        ]
        for start, end in zip(points, points[1:]):
            styled_line(canvas, start, end, style)
        for x, y in points:
            canvas.marker(x, y, style["radius"], style["color"], style["marker"])

    legend_x = width - 300
    for index, operation in enumerate(("checkpoint", "restore")):
        style = PLOT_STYLES[operation]
        y = 22 + index * 22
        styled_line(canvas, (legend_x, y), (legend_x + 48, y), style)
        canvas.marker(legend_x + 24, y, style["radius"], style["color"], style["marker"])
        canvas.text(legend_x + 62, y - 4, style["label"], axis)
    canvas.text(left, height - 24, "POINTS SHOW MEAN LATENCY", muted)
    raw = b"".join(b"\x00" + bytes(canvas.pixels[y * width * 3:(y + 1) * width * 3])
                   for y in range(height))
    png = b"\x89PNG\r\n\x1a\n"
    png += png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += png_chunk(b"IDAT", zlib.compress(raw, 9))
    png += png_chunk(b"IEND", b"")
    path.write_bytes(png)


def analyze(out_dir: pathlib.Path) -> tuple[list[GroupStats], list[str]]:
    out_dir = out_dir.resolve(strict=True)
    manifest = read_json(out_dir / "manifest.json")
    validate_manifest(manifest)
    manifest["_out_dir"] = str(out_dir)
    rows = read_jsonl(out_dir / "raw.jsonl")
    errors = plan_errors(manifest, rows)
    stats = calculate_stats(manifest, rows)
    analysis_dir = out_dir / "analysis"
    analysis_dir.mkdir(exist_ok=True)
    write_summary(analysis_dir / "summary.tsv", stats)
    write_latency_data(analysis_dir / "latency-vs-depth.tsv", stats)
    write_errno_counts(analysis_dir / "errno-counts.tsv", rows)
    write_invalid(analysis_dir / "invalid.jsonl", rows)
    write_png(analysis_dir / "latency-vs-depth.png", stats)
    return stats, errors


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze a DeltaFS E1 result directory")
    parser.add_argument("out_dir", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        stats, errors = analyze(arguments.out_dir)
    except (AnalysisError, OSError) as exc:
        print(f"FAIL: E1 analysis: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"FAIL: E1 analysis: {error}", file=sys.stderr)
        return 1
    print(f"PASS: E1 analysis completed; groups={len(stats)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
