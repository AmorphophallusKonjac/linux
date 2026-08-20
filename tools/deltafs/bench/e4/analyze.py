#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Analyze paired DeltaFS E4 temporal write-latency artifacts."""

from __future__ import annotations

import hashlib
import json
import math
import pathlib
import statistics
import struct
import sys
import zlib
from collections import defaultdict
from typing import Any, Iterable

import events


SCHEMA = events.SCHEMA
FS_CONFIGS = ("ext4_noreflink", "xfs_noreflink", "xfs_reflink")
BOOTSTRAP_REPLICATES = 1000


class AnalysisError(RuntimeError):
    """E4 artifacts are incomplete or inconsistent."""


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        raise AnalysisError("percentile of empty values")
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def stats(values: list[float]) -> dict[str, float | int]:
    return {
        "n": len(values), "mean": statistics.fmean(values),
        "p50": percentile(values, 0.50), "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def bootstrap_ci(rows: list[dict[str, Any]], field: str, replicates: int = BOOTSTRAP_REPLICATES) -> tuple[float, float]:
    sequences: dict[str, list[float]] = defaultdict(list)
    for row in rows:
        sequences[str(row["sequence_id"])].append(float(row[field]))
    if not sequences:
        raise AnalysisError("cannot bootstrap empty rows")
    seed = events.SEED
    values: list[float] = []
    keys = list(sequences)
    for _ in range(replicates):
        sample: list[float] = []
        for _index in keys:
            seed = (seed * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
            chosen = sequences[keys[seed % len(keys)]]
            seed = (seed * 6364136223846793005 + 1442695040888963407) & ((1 << 64) - 1)
            sample.append(chosen[seed % len(chosen)])
        values.append(statistics.fmean(sample))
    return percentile(values, 0.025), percentile(values, 0.975)


def write_tsv(path: pathlib.Path, header: list[str], rows: Iterable[dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as stream:
        stream.write("\t".join(header) + "\n")
        for row in rows:
            stream.write("\t".join(str(row.get(key, "")) for key in header) + "\n")


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xffffffff)


def plot_png(path: pathlib.Path, rows: list[dict[str, Any]], x_field: str,
             y_field: str = "mean") -> None:
    width, height = 720, 360
    pixels = bytearray([255, 255, 255, 255] * width * height)
    left, top, right, bottom = 70, 30, width - 25, height - 50

    def pixel(x: int, y: int, color: tuple[int, int, int]) -> None:
        if 0 <= x < width and 0 <= y < height:
            offset = (y * width + x) * 4
            pixels[offset:offset + 3] = bytes(color)

    def line(x0: int, y0: int, x1: int, y1: int, color: tuple[int, int, int]) -> None:
        dx, sx = abs(x1 - x0), 1 if x0 < x1 else -1
        dy, sy = -abs(y1 - y0), 1 if y0 < y1 else -1
        error = dx + dy
        while True:
            pixel(x0, y0, color)
            if x0 == x1 and y0 == y1:
                break
            twice = 2 * error
            if twice >= dy:
                error += dy
                x0 += sx
            if twice <= dx:
                error += dx
                y0 += sy

    for x in range(left, right + 1):
        pixel(x, bottom, (100, 100, 100))
    for y in range(top, bottom + 1):
        pixel(left, y, (100, 100, 100))
    if rows:
        x_values = sorted({float(row[x_field]) for row in rows})
        y_values = [float(row[y_field]) for row in rows]
        x_low, x_high = min(x_values), max(x_values)
        y_low, y_high = 0.0, max(y_values) * 1.10 or 1.0
        colors = ((27, 94, 153), (202, 87, 36), (47, 133, 90),
                  (139, 91, 164), (70, 70, 70))
        grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
        for row in rows:
            grouped[(str(row.get("fs_config")), str(row.get("workload_family")),
                     str(row.get("sample_kind")))].append(row)
        for index, group in enumerate(grouped.values()):
            color = colors[index % len(colors)]
            points = []
            for row in sorted(group, key=lambda item: float(item[x_field])):
                x_value, y_value = float(row[x_field]), float(row[y_field])
                x = left + round((x_value - x_low) / (x_high - x_low or 1) * (right - left))
                y = bottom - round((y_value - y_low) / (y_high - y_low) * (bottom - top))
                points.append((x, y))
                for dx in range(-2, 3):
                    for dy in range(-2, 3):
                        pixel(x + dx, y + dy, color)
            for first, second in zip(points, points[1:]):
                line(*first, *second, color)
    scanlines = [b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4])
                 for y in range(height)]
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    data += png_chunk(b"IDAT", zlib.compress(b"".join(scanlines), 9)) + png_chunk(b"IEND", b"")
    path.write_bytes(data)


def discover(root: pathlib.Path) -> list[dict[str, Any]]:
    result = []
    for directory in sorted(root.iterdir()):
        if not directory.is_dir() or directory.name == "analysis":
            continue
        manifest_path = directory / "manifest.json"
        event_path = directory / "events.jsonl"
        raw_path = directory / "raw.jsonl"
        if not manifest_path.is_file() or not event_path.is_file() or not raw_path.is_file():
            raise AnalysisError(f"incomplete result directory: {directory}")
        manifest = json.loads(manifest_path.read_text(encoding="ascii"))
        values = events.read_events(event_path, manifest.get("preset"))
        event_data = events.canonical_jsonl(values)
        if hashlib.sha256(event_data).hexdigest() != manifest.get("event_file_sha256"):
            raise AnalysisError(f"event hash mismatch: {directory}")
        raw = [json.loads(line) for line in raw_path.read_text(encoding="ascii").splitlines() if line]
        result.append({"directory": directory, "manifest": manifest, "events": values, "raw": raw})
    if len(result) != 3:
        raise AnalysisError("E4 analysis requires exactly three filesystem result directories")
    return result


def validate(result_sets: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    event_hashes = {item["manifest"].get("event_file_sha256") for item in result_sets}
    if len(event_hashes) != 1:
        errors.append("filesystem event schedules do not share one hash")
    seen_configs: set[str] = set()
    for item in result_sets:
        manifest = item["manifest"]
        config = manifest.get("fs_config")
        seen_configs.add(config)
        if config not in FS_CONFIGS or manifest.get("schema") != SCHEMA or \
                manifest.get("experiment") != events.EXPERIMENT or \
                manifest.get("deltafs_abi_version") != 2:
            errors.append(f"invalid manifest: {item['directory']}")
        summary_path = item["directory"] / "summary.json"
        try:
            runner_summary = json.loads(summary_path.read_text(encoding="ascii"))
        except (FileNotFoundError, json.JSONDecodeError):
            errors.append(f"missing runner summary: {item['directory']}")
            runner_summary = {}
        if not runner_summary.get("passed"):
            errors.append(f"runner did not pass: {item['directory']}")
        by_event: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for row in item["raw"]:
            if row.get("status") != "ok":
                errors.append(f"non-ok row in {item['directory']}: {row.get('invalid_reason')}")
                continue
            for field in ("event_id", "pair_id", "sequence_id", "sample_kind", "edit_e2e_ns"):
                if field not in row:
                    errors.append(f"missing {field} in {item['directory']}")
                    break
            if "event_id" in row:
                by_event[str(row["event_id"])].append(row)
            if row.get("edit_e2e_ns", 0) < 0:
                errors.append(f"negative latency in {item['directory']}")
            if row.get("pre_sha256") != row.get("expected_before_sha256") or \
                    row.get("post_sha256") != row.get("expected_after_sha256"):
                errors.append(f"hash oracle mismatch in {item['directory']}")
            before = row.get("checkpoint_generation_before")
            after = row.get("checkpoint_generation_after")
            if not isinstance(before, int) or not isinstance(after, int) or \
                    after < before or after - before > 1:
                errors.append(f"invalid generation transition in {item['directory']}")
        expected_ids = {event["event_id"] for event in item["events"]}
        event_by_id = {event["event_id"]: event for event in item["events"]}
        if set(by_event) != expected_ids:
            errors.append(f"raw/event ID set differs in {item['directory']}")
        baseline_kind = "upper_resident" if manifest.get("access_mode") == "reopen" else "current_fd"
        for event_id in expected_ids:
            kinds = [row.get("sample_kind") for row in by_event.get(event_id, [])]
            event = event_by_id[event_id]
            numerator = "steady_after_copyup" if event["workload_family"] == "burst4" and \
                event["write_index"] > 0 else "first_touch"
            if sorted(kinds) != sorted((baseline_kind, numerator)):
                errors.append(f"incomplete pair {event_id} in {item['directory']}")
    if seen_configs != set(FS_CONFIGS):
        errors.append("filesystem result set is incomplete")
    return errors


def all_rows(result_sets: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows = []
    for item in result_sets:
        config = item["manifest"]["fs_config"]
        for row in item["raw"]:
            if row.get("status") == "ok":
                rows.append({**row, "fs_config": config})
    return rows


def group_summary(rows: list[dict[str, Any]], field: str, groups: tuple[str, ...]) -> list[dict[str, Any]]:
    grouped: dict[tuple[Any, ...], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        grouped[tuple(row.get(key) for key in groups)].append(row)
    output = []
    for key, group in sorted(grouped.items(), key=lambda item: tuple(str(x) for x in item[0])):
        values = [float(row[field]) for row in group]
        low, high = bootstrap_ci(group, field, 200)
        item = dict(zip(groups, key))
        item.update(stats(values), ci95_low=low, ci95_high=high)
        output.append(item)
    return output


def paired_amplification(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    indexed = {(row["fs_config"], row["event_id"], row["sample_kind"]): row for row in rows}
    result = []
    for row in rows:
        if row["sample_kind"] not in ("first_touch", "steady_after_copyup"):
            continue
        baseline_kind = "upper_resident" if row["access_mode"] == "reopen" else "current_fd"
        baseline = indexed.get((row["fs_config"], row["event_id"], baseline_kind))
        if baseline is None:
            continue
        result.append({
            "fs_config": row["fs_config"], "workload_family": row["workload_family"],
            "generation": row["generation"], "history_depth": row["history_depth"],
            "event_id": row["event_id"], "sequence_id": row["sequence_id"],
            "sample_kind": row["sample_kind"],
            "latency_ratio": float(row["edit_e2e_ns"]) / float(baseline["edit_e2e_ns"]),
            "overhead_ns": float(row["edit_e2e_ns"]) - float(baseline["edit_e2e_ns"]),
        })
    return result


def analyze(root: pathlib.Path) -> list[str]:
    result_sets = discover(root)
    errors = validate(result_sets)
    rows = all_rows(result_sets) if not errors else []
    analysis = root / "analysis"
    analysis.mkdir(exist_ok=True)
    if rows:
        by_generation = group_summary(rows, "edit_e2e_ns", ("fs_config", "workload_family", "sample_kind", "generation"))
        by_depth = group_summary(rows, "edit_e2e_ns", ("fs_config", "workload_family", "sample_kind", "history_depth"))
        amplification = paired_amplification(rows)
        amp_summary = group_summary(amplification, "latency_ratio", ("fs_config", "workload_family", "sample_kind", "generation")) if amplification else []
        burst = [row for row in rows if row["workload_family"] == "burst4"]
        burst_summary = group_summary(burst, "edit_e2e_ns", ("fs_config", "sample_kind", "write_index")) if burst else []
        write_tsv(analysis / "latency-by-generation.tsv",
                  ["fs_config", "workload_family", "sample_kind", "generation", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], by_generation)
        write_tsv(analysis / "summary.tsv",
                  ["fs_config", "workload_family", "sample_kind", "generation", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], by_generation)
        write_tsv(analysis / "latency-by-history-depth.tsv",
                  ["fs_config", "workload_family", "sample_kind", "history_depth", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], by_depth)
        write_tsv(analysis / "amplification.tsv",
                  ["fs_config", "workload_family", "sample_kind", "generation", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], amp_summary)
        write_tsv(analysis / "burst-amortization.tsv",
                  ["fs_config", "sample_kind", "write_index", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], burst_summary)
        if any(item["manifest"].get("access_mode") == "held_fd" for item in result_sets):
            write_tsv(analysis / "held-fd.tsv", ["fs_config", "workload_family", "sample_kind", "generation", "n", "mean", "p50", "p95", "p99", "stddev", "ci95_low", "ci95_high"], by_generation)
        plot_png(analysis / "latency-vs-generation.png", by_generation, "generation")
        plot_png(analysis / "latency-vs-history-depth.png", by_depth, "history_depth")
        plot_png(analysis / "latency-amplification.png", amp_summary, "generation")
    else:
        errors.append("no valid raw rows")
    (analysis / "invalid.jsonl").write_text("\n".join(
        json.dumps(error, sort_keys=True) for error in errors
    ) + ("\n" if errors else ""), encoding="ascii")
    write_tsv(analysis / "pairing-errors.tsv", ["error"], ({"error": error} for error in errors))
    summary = {"schema": SCHEMA, "experiment": events.EXPERIMENT,
               "passed": not errors, "result_sets": len(result_sets),
               "timed_rows": len(rows), "errors": errors}
    write_json = (analysis / "summary.json").write_text
    write_json(json.dumps(summary, sort_keys=True, indent=2) + "\n", encoding="ascii")
    return errors


def main(argv: list[str] | None = None) -> int:
    arguments = sys.argv[1:] if argv is None else argv
    if len(arguments) != 1:
        print("Usage: analyze.py RESULTS_ROOT", file=sys.stderr)
        return 2
    try:
        errors = analyze(pathlib.Path(arguments[0]))
    except (AnalysisError, OSError, json.JSONDecodeError, events.EventError) as exc:
        print(f"FAIL: E4 analysis: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors[:50]:
            print(f"FAIL: E4 analysis: {error}", file=sys.stderr)
        return 1
    print("PASS: E4 analysis completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
