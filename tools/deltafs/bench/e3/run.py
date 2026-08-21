#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""DeltaFS E3 temporal write-latency benchmark runner."""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
from typing import Any

import events


SCHEMA = events.SCHEMA
DELTAFS_ABI_VERSION = 2
MOUNT_FEATURES = (
    "index=off", "nfs_export=off", "metacopy=off", "xino=off",
    "uuid=off", "redirect_dir=nofollow",
)
FILESYSTEM_MAGICS = {
    "ext4": 0xEF53,
    "xfs": 0x58465342,
    "f2fs": 0xF2F52010,
}
DMESG_FAILURE = re.compile(
    r"(?:\bBUG:|\bWARNING:|KASAN|KFENCE|UBSAN|lockdep|"
    r"RCU (?:stall|warning)|rcu_preempt detected stalls)", re.IGNORECASE,
)


class E3Error(RuntimeError):
    """An E3 environment or execution contract failure."""


class Logs:
    def __init__(self, out_dir: pathlib.Path) -> None:
        self.stdout = (out_dir / "stdout.log").open("a", encoding="utf-8")
        self.stderr = (out_dir / "stderr.log").open("a", encoding="utf-8")

    def close(self) -> None:
        self.stdout.close()
        self.stderr.close()

    def info(self, message: str) -> None:
        print(message)
        print(message, file=self.stdout, flush=True)

    def error(self, message: str) -> None:
        print(message, file=sys.stderr)
        print(message, file=self.stderr, flush=True)

    def subprocess(self, command: list[str], *, cwd: pathlib.Path | None = None,
                   check: bool = True, pass_fds: tuple[int, ...] = ()) -> subprocess.CompletedProcess[str]:
        process = subprocess.run(
            command, cwd=cwd, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False, pass_fds=pass_fds,
        )
        if process.stdout:
            self.stdout.write(process.stdout)
            self.stdout.flush()
        if process.stderr:
            self.stderr.write(process.stderr)
            self.stderr.flush()
        if check and process.returncode:
            detail = process.stderr.strip() or process.stdout.strip()
            raise E3Error(
                f"command failed ({process.returncode}): {' '.join(command)}"
                + (f": {detail}" if detail else "")
            )
        return process


def atomic_write(path: pathlib.Path, data: bytes) -> None:
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        view = memoryview(data)
        while view:
            count = os.write(fd, view)
            if count <= 0:
                raise OSError("short atomic write")
            view = view[count:]
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(temporary, path)


def write_json(path: pathlib.Path, value: Any) -> None:
    atomic_write(path, (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("ascii"))


def append_jsonl(path: pathlib.Path, value: dict[str, Any]) -> None:
    with path.open("a", encoding="ascii") as stream:
        stream.write(json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def timed_setup(path: pathlib.Path, sequence_id: str, variant: str,
                kind: str, generation: int, function: Any) -> Any:
    before = time.monotonic_ns()
    status = "ok"
    reason = None
    try:
        return function()
    except BaseException as exc:
        status = "failed"
        reason = str(exc).replace("\n", " ")[:1000]
        raise
    finally:
        append_jsonl(path, {
            "schema": SCHEMA, "sequence_id": sequence_id, "variant": variant,
            "setup_kind": kind, "generation": generation,
            "latency_ns": time.monotonic_ns() - before, "status": status,
            "invalid_reason": reason,
        })


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def command_output(command: list[str]) -> str:
    process = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=False)
    if process.returncode:
        raise E3Error(f"command failed: {' '.join(command)}: {process.stderr.strip()}")
    return process.stdout


def find_mount(path: pathlib.Path) -> dict[str, str]:
    output = command_output(["findmnt", "-J", "-T", str(path), "-o",
                             "SOURCE,TARGET,FSTYPE,OPTIONS,FS-OPTIONS,UUID"])
    try:
        entry = json.loads(output)["filesystems"][0]
    except (KeyError, IndexError, TypeError, json.JSONDecodeError) as exc:
        raise E3Error("findmnt returned invalid JSON") from exc
    return {key: str(entry.get(key, "") or "") for key in (
        "source", "target", "fstype", "options", "fs-options", "uuid")}


def filesystem_magic(path: pathlib.Path) -> int:
    class StatFs(ctypes.Structure):
        _fields_ = [("f_type", ctypes.c_long), ("f_bsize", ctypes.c_long),
                    ("f_blocks", ctypes.c_ulonglong), ("f_bfree", ctypes.c_ulonglong),
                    ("f_bavail", ctypes.c_ulonglong), ("f_files", ctypes.c_ulonglong),
                    ("f_ffree", ctypes.c_ulonglong), ("f_fsid", ctypes.c_int * 2),
                    ("f_namelen", ctypes.c_long), ("f_frsize", ctypes.c_long),
                    ("f_flags", ctypes.c_long), ("f_spare", ctypes.c_long * 4)]
    value = StatFs()
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.statfs
    function.argtypes = [ctypes.c_char_p, ctypes.POINTER(StatFs)]
    function.restype = ctypes.c_int
    if function(os.fsencode(path), ctypes.byref(value)):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), path)
    return int(value.f_type)


def overlay_mounts() -> list[str]:
    mounts = []
    with open("/proc/self/mountinfo", encoding="utf-8") as stream:
        for line in stream:
            if " - overlay " in line:
                mounts.append(line.split()[4])
    return mounts


def canonical_new_path(path: pathlib.Path) -> pathlib.Path:
    if path.exists():
        return path.resolve(strict=True)
    return path.parent.resolve(strict=True) / path.name


def is_relative_to(path: pathlib.Path, parent: pathlib.Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def validate_environment(backing_arg: pathlib.Path, out_arg: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict[str, str], int]:
    if os.geteuid() != 0:
        raise E3Error("E3 must run as root")
    if not hasattr(__import__("time"), "CLOCK_MONOTONIC_RAW"):
        raise E3Error("CLOCK_MONOTONIC_RAW is unavailable")
    if backing_arg.is_symlink():
        raise E3Error("BACKING_DIR must not be a symlink")
    backing = backing_arg.resolve(strict=True)
    if not backing.is_dir() or any(backing.iterdir()):
        raise E3Error("BACKING_DIR must be an existing empty directory")
    out = canonical_new_path(out_arg)
    if is_relative_to(out, backing):
        raise E3Error("OUT_DIR must not be inside BACKING_DIR")
    if overlay_mounts():
        raise E3Error("refusing to run while an OverlayFS mount exists")
    mount = find_mount(backing)
    if mount["fstype"] not in FILESYSTEM_MAGICS or filesystem_magic(backing) != FILESYSTEM_MAGICS[mount["fstype"]]:
        raise E3Error("BACKING_DIR must be on ext4, XFS, or F2FS")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise E3Error("OUT_DIR must not exist or must be empty")
    out.mkdir(mode=0o700, exist_ok=True)
    if out.stat().st_dev == backing.stat().st_dev:
        raise E3Error("OUT_DIR must be on a different device")
    affinity = sorted(os.sched_getaffinity(0))
    if not affinity:
        raise E3Error("no available CPU")
    os.sched_setaffinity(0, {affinity[0]})
    return backing, out, mount, affinity[0]


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def syncfs(path: pathlib.Path) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    function = libc.syncfs
    function.argtypes = [ctypes.c_int]
    function.restype = ctypes.c_int
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        if function(fd):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error))
    finally:
        os.close(fd)


def kernel_config_sha256() -> str:
    candidates = (pathlib.Path("/proc/config.gz"),
                  pathlib.Path(f"/boot/config-{platform.release()}"),
                  pathlib.Path(__file__).resolve().parents[4] / ".config")
    for candidate in candidates:
        if candidate.is_file():
            return sha256_file(candidate)
    return ""


def repo_commit() -> str:
    root = pathlib.Path(__file__).resolve().parents[4]
    process = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                             text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, check=False)
    return process.stdout.strip() if process.returncode == 0 else "unknown"


def filesystem_configuration(mount: dict[str, str]) -> tuple[str, str]:
    if mount["fstype"] in ("ext4", "f2fs"):
        return mount["fstype"], ""
    xfs_info = command_output(["xfs_info", mount["target"]])
    matches = re.findall(r"(?:^|\s)reflink=([01])(?:\s|$)", xfs_info)
    if len(matches) != 1:
        raise E3Error("xfs_info did not report exactly one reflink value")
    fs_config = "xfs_reflink" if matches[0] == "1" else "xfs_noreflink"
    return fs_config, xfs_info


def build_manifest(preset: str, event_path: pathlib.Path, values: list[dict[str, Any]],
                   mount: dict[str, str], cpu: int) -> dict[str, Any]:
    configuration = events.PRESETS[preset]
    event_data = events.canonical_jsonl(values)
    clocksource = pathlib.Path("/sys/devices/system/clocksource/clocksource0/current_clocksource")
    fs_config, xfs_info = filesystem_configuration(mount)
    return {
        "schema": SCHEMA, "experiment": events.EXPERIMENT, "preset": preset,
        "seed": events.SEED, "event_file": event_path.name,
        "event_file_sha256": hashlib.sha256(event_data).hexdigest(),
        "event_count": len(values), "deltafs_abi_version": DELTAFS_ABI_VERSION,
        "git_commit": repo_commit(), "kernel_release": platform.release(),
        "kernel_config_sha256": kernel_config_sha256(), "fs_type": mount["fstype"],
        "fs_config": fs_config, "xfs_info": xfs_info,
        "fs_uuid": mount["uuid"], "backing_source": mount["source"],
        "backing_mount_options": mount["options"],
        "overlay_mount_options": list(MOUNT_FEATURES), "access_mode": configuration["access_mode"],
        "direct_baseline": "lower_direct",
        "sweep": configuration["sweep"], "workload_families": list(configuration["families"]),
        "generation_counts": list(configuration["generations"]),
        "history_depths": list(events.HISTORY_DEPTHS), "sequences_per_cell": configuration["samples"],
        "independent_runs": configuration["runs"], "cpu": cpu,
        "clocksource": clocksource.read_text(encoding="ascii").strip() if clocksource.is_file() else "unknown",
        "started_at": utc_now(),
        "expected_sequence_count": configuration["runs"] * configuration["samples"] *
        len(configuration["families"]) * (len(events.HISTORY_DEPTHS)
                                            if configuration["sweep"] == "history"
                                            else len(configuration["generations"])),
    }


def create_tree(sample: pathlib.Path, sequence_events: list[dict[str, Any]], family: str) -> None:
    (sample / "base").mkdir(parents=True)
    (sample / "active" / "upper").mkdir(parents=True)
    (sample / "active" / "work").mkdir(parents=True)
    (sample / "merged").mkdir()
    first: dict[int, dict[str, Any]] = {}
    for event in sequence_events:
        first.setdefault(event["file_id"], event)
    for event in first.values():
        target = sample / "base" / event["relative_path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(events.byte_stream(event["initial_seed"], event["file_size"]))
        target.chmod(0o644)
    del family
    for path in sample.rglob("*"):
        if path.stat().st_dev != sample.stat().st_dev:
            raise E3Error(f"sample crosses backing superblock: {path}")


def mount_sample(sample: pathlib.Path, logs: Logs) -> None:
    options = ["lowerdir=base", "upperdir=active/upper", "workdir=active/work", *MOUNT_FEATURES]
    logs.subprocess(["mount", "-t", "overlay", "overlay", "-o", ",".join(options), "merged"], cwd=sample)
    mount = find_mount(sample / "merged")
    if mount["fstype"] != "overlay":
        raise E3Error("sample mount is not OverlayFS")
    conflicts = {"index=on", "nfs_export=on", "metacopy=on", "xino=on", "xino=auto",
                 "uuid=on", "uuid=auto", "redirect_dir=on", "redirect_dir=follow", "redirect_dir=off"}
    found = sorted(set(mount["options"].split(",")) & conflicts)
    if found:
        raise E3Error(f"mounted OverlayFS feature mismatch: {found}")


def checkpoint(sample: pathlib.Path, generation: int, helper: pathlib.Path, logs: Logs) -> None:
    frozen = sample / "layers" / f"g{generation:03d}"
    frozen.parent.mkdir(exist_ok=True)
    os.rename(sample / "active" / "upper", frozen)
    (sample / "active" / "upper").mkdir()
    next_work = sample / "active" / f"work-g{generation + 1:03d}"
    next_work.mkdir()
    logs.subprocess([str(helper), str(sample / "merged"), str(generation),
                     str(sample / "active" / "upper"), str(next_work)])


def unmount(sample: pathlib.Path, logs: Logs) -> None:
    logs.subprocess(["umount", "--", str(sample / "merged")])


def read_helper(path: pathlib.Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="ascii"))
    if set(value) != {"schema", "open_ns", "pwrite_only_ns", "fsync_ns", "close_ns",
                      "edit_e2e_ns", "cpu_before", "cpu_after", "major_faults_delta",
                      "errno", "status", "invalid_reason"} or value["schema"] != 1:
        raise E3Error("helper result schema mismatch")
    return value


def verify_image(event: dict[str, Any], path: pathlib.Path, expected: str) -> None:
    if path.stat().st_size != event["file_size"] or sha256_file(path) != expected:
        raise E3Error(f"image oracle failed: {path}")


def sequence_key(values: list[dict[str, Any]]) -> str:
    keys = {event["sequence_id"] for event in values}
    if len(keys) != 1:
        raise E3Error("sequence event set is not singular")
    return next(iter(keys))


def run_one(sample: pathlib.Path, event: dict[str, Any], sample_kind: str,
            helper: pathlib.Path, logs: Logs, access_mode: str,
            checkpoint_before: int, checkpoint_after: int,
            root_name: str = "merged") -> dict[str, Any]:
    root = sample / root_name
    target = root / event["relative_path"]
    result_path = sample / "helper-result.json"
    command = [str(helper), access_mode, str(root), str(target),
               str(event["offset"]), str(event["payload_seed"]),
               str(event["file_size"]), str(event["write_bytes"])]
    command.append(str(result_path))
    verify_image(event, target, event["expected_before_sha256"])
    process = logs.subprocess(command, check=False)
    result = read_helper(result_path)
    verify_image(event, target, event["expected_after_sha256"])
    row = {**event, "sample_kind": sample_kind, "access_mode": access_mode,
           "checkpoint_generation_before": checkpoint_before,
           "checkpoint_generation_after": checkpoint_after,
           "pre_sha256": event["expected_before_sha256"],
           "post_sha256": event["expected_after_sha256"],
           **result}
    if process.returncode or result["status"] != "ok":
        raise E3Error(f"temporal helper returned {result['status']}")
    return row


def make_lower_direct_rows(sequence: list[dict[str, Any]], sample: pathlib.Path,
                           helper: pathlib.Path, logs: Logs,
                           access_mode: str,
                           setup_path: pathlib.Path | None = None) -> list[dict[str, Any]]:
    """Measure equivalent writes directly through the backing filesystem.

    The base tree is independent of the OverlayFS variants. Events are replayed
    in canonical order so each pre/post hash oracle remains identical, while
    checkpoint generation fields use 0/0 to denote that no DeltaFS transition
    took place for this control.
    """
    family = sequence[0]["workload_family"]
    sequence_id = sequence[0]["sequence_id"]
    if setup_path is None:
        setup_path = sample.parent / f"{sample.name}-setup.jsonl"
    create_tree(sample, sequence, family)
    timed_setup(setup_path, sequence_id, "lower_direct", "syncfs", 0,
                lambda: syncfs(sample / "base"))
    rows = []
    for event in sequence:
        rows.append(run_one(
            sample, event, "lower_direct", helper, logs, access_mode,
            0, 0, root_name="base",
        ))
    return rows


def materialize_resident(sample: pathlib.Path, generation_events: list[dict[str, Any]]) -> None:
    unique = {event["relative_path"]: event for event in generation_events}
    for event in unique.values():
        target = sample / "merged" / event["relative_path"]
        with target.open("r+b", buffering=0) as stream:
            block = stream.read(events.BLOCK_SIZE)
            if len(block) != events.BLOCK_SIZE:
                raise E3Error("resident prewrite could not read one block")
            stream.seek(0)
            if stream.write(block) != len(block):
                raise E3Error("resident prewrite was short")
            os.fsync(stream.fileno())
        verify_image(event, target, event["expected_before_sha256"])
    syncfs(sample / "merged")


def make_sequence_rows(sequence: list[dict[str, Any]], sample_kind: str,
                       sample: pathlib.Path, helper: pathlib.Path,
                       checkpoint_helper: pathlib.Path, logs: Logs,
                       access_mode: str, resident: bool = False,
                       setup_path: pathlib.Path | None = None) -> list[dict[str, Any]]:
    family = sequence[0]["workload_family"]
    sequence_id = sequence[0]["sequence_id"]
    if setup_path is None:
        setup_path = sample.parent / f"{sample.name}-setup.jsonl"
    create_tree(sample, sequence, family)
    timed_setup(setup_path, sequence_id, sample_kind, "syncfs", 0,
                lambda: syncfs(sample / "base"))
    timed_setup(setup_path, sequence_id, sample_kind, "mount", 1,
                lambda: mount_sample(sample, logs))
    try:
        rows = []
        max_generation = max(event["generation"] for event in sequence)
        by_generation: dict[int, list[dict[str, Any]]] = {}
        for event in sequence:
            by_generation.setdefault(event["generation"], []).append(event)
        generation = 1
        history_depth = sequence[0]["history_depth"]
        for _ in range(1, history_depth):
            timed_setup(
                setup_path, sequence_id, sample_kind, "checkpoint", generation,
                lambda generation=generation: checkpoint(
                    sample, generation, checkpoint_helper, logs,
                ),
            )
            generation += 1
        for measured_generation in range(1, max_generation + 1):
            generation_events = by_generation[measured_generation]
            checkpoint_before = generation
            timed_setup(
                setup_path, sequence_id, sample_kind, "checkpoint", generation,
                lambda generation=generation: checkpoint(
                    sample, generation, checkpoint_helper, logs,
                )
            )
            generation += 1
            checkpoint_after = generation
            if resident:
                timed_setup(
                    setup_path, sequence_id, sample_kind, "syncfs", generation,
                    lambda: materialize_resident(sample, generation_events),
                )
            for event in generation_events:
                kind = sample_kind
                if sample_kind == "first_touch" and event["workload_family"] == "burst4" and event["write_index"] > 0:
                    kind = "steady_after_copyup"
                rows.append(run_one(sample, event, kind, helper, logs,
                                    access_mode, checkpoint_before, checkpoint_after))
        return rows
    finally:
        if os.path.ismount(sample / "merged"):
            timed_setup(setup_path, sequence_id, sample_kind, "unmount", generation,
                        lambda: unmount(sample, logs))


def read_dmesg(logs: Logs) -> str:
    process = subprocess.run(["dmesg"], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=False)
    if process.returncode:
        raise E3Error(f"cannot read dmesg: {process.stderr.strip()}")
    return process.stdout


def preflight_v2(backing: pathlib.Path, sequence: list[dict[str, Any]],
                 checkpoint_helper: pathlib.Path, logs: Logs) -> None:
    sample = backing / ".e3-work" / "preflight-v2"
    create_tree(sample, sequence, sequence[0]["workload_family"])
    mount_sample(sample, logs)
    try:
        checkpoint(sample, 1, checkpoint_helper, logs)
    finally:
        if os.path.ismount(sample / "merged"):
            unmount(sample, logs)
    shutil.rmtree(sample)


def new_dmesg(before: str, after: str) -> str:
    if after.startswith(before):
        return after[len(before):]
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    for overlap in range(min(len(before_lines), len(after_lines)), 0, -1):
        if before_lines[-overlap:] == after_lines[:overlap]:
            return "\n".join(after_lines[overlap:])
    raise E3Error("dmesg ring changed without overlap")


def run_benchmark(preset: str, backing: pathlib.Path, out: pathlib.Path,
                  mount: dict[str, str], cpu: int) -> int:
    logs = Logs(out)
    helper = pathlib.Path(__file__).with_name("temporal_write_bench")
    checkpoint_helper = pathlib.Path(__file__).with_name("checkpoint_v2")
    values = events.generate_events(preset)
    event_path = out / "events.jsonl"
    event_path.write_bytes(events.canonical_jsonl(values))
    write_json(out / "manifest.json", build_manifest(preset, event_path, values, mount, cpu))
    write_json(out / "summary.json", {"schema": SCHEMA, "preset": preset, "completed": False})
    if not helper.is_file() or not checkpoint_helper.is_file():
        logs.close()
        raise E3Error("E3 binaries are not built; run make -C tools/deltafs e3-bench")
    raw_path = out / "raw.jsonl"
    setup_path = out / "setup.jsonl"
    setup_path.touch()
    dmesg_before = read_dmesg(logs)
    atomic_write(out / "dmesg-before.log", dmesg_before.encode())
    grouped: dict[str, list[dict[str, Any]]] = {}
    for event in values:
        grouped.setdefault(event["sequence_id"], []).append(event)
    rows: list[dict[str, Any]] = []
    failures: list[str] = []
    configuration = events.PRESETS[preset]
    work_root = backing / ".e3-work"
    work_root.mkdir(mode=0o700)
    try:
        preflight_v2(backing, next(iter(grouped.values())), checkpoint_helper, logs)
        for index, (sequence_id, sequence) in enumerate(grouped.items(), 1):
            logs.info(f"E3: sequence {index}/{len(grouped)} {sequence_id}")
            sample_root = work_root / sequence_id
            try:
                variants = [("lower_direct", None), ("first_touch", False),
                            ("upper_resident", True)]
                rotation = (index - 1) % len(variants)
                variants = variants[rotation:] + variants[:rotation]
                for variant, resident in variants:
                    variant_root = sample_root.with_name(sample_root.name + "-" + variant)
                    if variant == "lower_direct":
                        sample_rows = make_lower_direct_rows(
                            sequence, variant_root, helper, logs,
                            configuration["access_mode"], setup_path,
                        )
                    else:
                        sample_rows = make_sequence_rows(
                            sequence, variant, variant_root, helper, checkpoint_helper,
                            logs, configuration["access_mode"], bool(resident),
                            setup_path,
                        )
                    rows.extend(sample_rows)
                    for row in sample_rows:
                        append_jsonl(raw_path, row)
                    shutil.rmtree(variant_root)
            except BaseException as exc:
                reason = str(exc).replace("\n", " ")[:1000]
                failures.append(f"{sequence_id}: {reason}")
                logs.error(f"FAIL: {sequence_id}: {reason}")
                append_jsonl(raw_path, {"schema": SCHEMA, "sequence_id": sequence_id,
                                       "status": "failed", "invalid_reason": reason})
                if os.path.ismount(sample_root / "merged"):
                    unmount(sample_root, logs)
        dmesg_after = read_dmesg(logs)
        atomic_write(out / "dmesg-after.log", dmesg_after.encode())
        diagnostics = new_dmesg(dmesg_before, dmesg_after)
        dmesg_failures = [line for line in diagnostics.splitlines() if DMESG_FAILURE.search(line)]
        counts = {"ok": sum(1 for row in rows if row.get("status") == "ok"),
                  "failed": len(failures)}
        summary = {"schema": SCHEMA, "preset": preset, "completed": not failures,
                   "passed": not failures and not dmesg_failures,
                   "sequence_count": len(grouped), "timed_rows": len(rows),
                   "counts": counts, "dmesg_failures": dmesg_failures,
                   "errors": failures, "finished_at": utc_now()}
        write_json(out / "summary.json", summary)
        if summary["passed"]:
            logs.info(f"PASS: E3 {preset}; sequences={len(grouped)} rows={len(rows)}")
            return 0
        return 1
    finally:
        logs.close()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Measure DeltaFS E3 temporal write latency")
    parser.add_argument("preset", choices=tuple(events.PRESETS))
    parser.add_argument("backing_dir", type=pathlib.Path)
    parser.add_argument("out_dir", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        backing, out, mount, cpu = validate_environment(arguments.backing_dir, arguments.out_dir)
        return run_benchmark(arguments.preset, backing, out, mount, cpu)
    except (E3Error, OSError, events.EventError) as exc:
        print(f"FAIL: E3 runner: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
