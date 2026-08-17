#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""DeltaFS E3 fixed-preset copy-up benchmark runner."""

from __future__ import annotations

import argparse
import collections
import ctypes
import datetime as dt
import hashlib
import itertools
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
from typing import Any

import events


SCHEMA = events.SCHEMA
DELTAFS_ABI_VERSION = 2
INITIAL_GENERATION = 1
CHECKPOINT_GENERATION = 2
COPYUP_SOURCE = "checkpoint_frozen_upper"
MOUNT_FEATURES = (
    "index=off", "nfs_export=off", "metacopy=off", "xino=off",
    "uuid=off", "redirect_dir=nofollow",
)
FILESYSTEM_MAGICS = {"ext4": 0xEF53, "xfs": 0x58465342}
NOOP_INTERVAL = 20
NOOP_REPETITIONS = 3
STAT_FIELDS = frozenset((
    "schema", "kind", "status", "errno", "invalid_reason", "settle_timeout",
    "sectors_before", "sectors_after", "physical_io_bytes", "pre_sha256",
    "post_sha256", "upper_sha256", "lower_sha256", "copyup_bytes",
    "shared_bytes", "allocated_bytes_total", "fiemap_block_size",
    "fiemap_extent_count",
))
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
                   check: bool = True) -> subprocess.CompletedProcess[str]:
        process = subprocess.run(
            command, cwd=cwd, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=False,
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


def write_all_fd(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        count = os.write(fd, view)
        if count == 0:
            raise OSError("zero-length write")
        view = view[count:]


def atomic_write_bytes(path: pathlib.Path, data: bytes) -> None:
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        write_all_fd(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)
    try:
        os.replace(temporary, path)
        directory_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_fd)
        finally:
            os.close(directory_fd)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def atomic_write_json(path: pathlib.Path, value: Any) -> None:
    data = (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("ascii")
    atomic_write_bytes(path, data)


def append_jsonl(path: pathlib.Path, value: dict[str, Any]) -> None:
    data = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode("ascii")
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        write_all_fd(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")


def command_output(command: list[str]) -> str:
    process = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=False)
    if process.returncode:
        raise E3Error(
            f"command failed ({process.returncode}): {' '.join(command)}: "
            f"{process.stderr.strip()}"
        )
    return process.stdout


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


def find_mount(path: pathlib.Path) -> dict[str, str]:
    output = command_output([
        "findmnt", "-J", "-T", str(path), "-o",
        "SOURCE,TARGET,FSTYPE,OPTIONS,FS-OPTIONS,UUID",
    ])
    try:
        entry = json.loads(output)["filesystems"][0]
    except (KeyError, IndexError, TypeError, json.JSONDecodeError) as exc:
        raise E3Error("findmnt returned invalid JSON") from exc
    return {
        "source": str(entry.get("source", "")),
        "target": str(entry.get("target", "")),
        "fstype": str(entry.get("fstype", "")),
        "options": str(entry.get("options", "")),
        "fs_options": str(entry.get("fs-options", "")),
        "uuid": str(entry.get("uuid") or ""),
    }


def filesystem_magic(path: pathlib.Path) -> int:
    class StatFs(ctypes.Structure):
        _fields_ = [
            ("f_type", ctypes.c_long), ("f_bsize", ctypes.c_long),
            ("f_blocks", ctypes.c_ulonglong), ("f_bfree", ctypes.c_ulonglong),
            ("f_bavail", ctypes.c_ulonglong), ("f_files", ctypes.c_ulonglong),
            ("f_ffree", ctypes.c_ulonglong), ("f_fsid", ctypes.c_int * 2),
            ("f_namelen", ctypes.c_long), ("f_frsize", ctypes.c_long),
            ("f_flags", ctypes.c_long), ("f_spare", ctypes.c_long * 4),
        ]

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
    result = []
    with open("/proc/self/mountinfo", encoding="utf-8") as stream:
        for line in stream:
            if " - overlay " in line:
                fields = line.split()
                result.append(fields[4] if len(fields) > 4 else line.strip())
    return result


def xfs_configuration(backing: pathlib.Path, mount: dict[str, str]) -> tuple[str, str]:
    if mount["fstype"] == "ext4":
        return "ext4_noreflink", ""
    output = command_output(["xfs_info", str(backing)])
    matches = re.findall(r"(?:^|\s)reflink=([01])(?:\s|$)", output)
    if len(matches) != 1:
        raise E3Error("xfs_info did not report exactly one reflink=0/1 value")
    return ("xfs_reflink" if matches[0] == "1" else "xfs_noreflink"), output


def validate_device_stat(backing: pathlib.Path, supplied: pathlib.Path) -> pathlib.Path:
    if supplied.is_symlink():
        raise E3Error("DEVICE_STAT must be the stat file, not a symlink")
    canonical = supplied.resolve(strict=True)
    if not canonical.is_file():
        raise E3Error("DEVICE_STAT must be a readable regular sysfs file")
    device = os.stat(backing).st_dev
    major_minor = f"{os.major(device)}:{os.minor(device)}"
    sys_device = pathlib.Path("/sys/dev/block") / major_minor
    try:
        expected = (sys_device.resolve(strict=True) / "stat").resolve(strict=True)
    except FileNotFoundError as exc:
        raise E3Error(f"backing device has no sysfs stat: {major_minor}") from exc
    if canonical != expected:
        raise E3Error(
            f"DEVICE_STAT does not match BACKING_DIR device {major_minor}: "
            f"expected {expected}, got {canonical}"
        )
    fields = canonical.read_text(encoding="ascii").split()
    if len(fields) < 7 or any(not field.isdecimal() for field in fields[:7]):
        raise E3Error("DEVICE_STAT has an invalid block-stat record")
    return canonical


def validate_output_dir(path: pathlib.Path) -> pathlib.Path:
    if path.is_symlink():
        raise E3Error("OUT_DIR must not be a symlink")
    canonical = canonical_new_path(path)
    if canonical.exists():
        if not canonical.is_dir() or any(canonical.iterdir()):
            raise E3Error("OUT_DIR must not exist or must be an empty directory")
    else:
        canonical.mkdir(mode=0o700)
    return canonical


def validate_environment(backing_arg: pathlib.Path, device_arg: pathlib.Path,
                         out_arg: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path,
                                                        pathlib.Path, dict[str, str],
                                                        str, str]:
    if os.geteuid() != 0:
        raise E3Error("E3 must run as root")
    for command in ("findmnt", "mount", "umount", "dmesg", "xfs_info"):
        if shutil.which(command) is None:
            raise E3Error(f"required command not found: {command}")
    if backing_arg.is_symlink():
        raise E3Error("BACKING_DIR must not be a symlink")
    backing = backing_arg.resolve(strict=True)
    if not backing.is_dir() or any(backing.iterdir()):
        raise E3Error("BACKING_DIR must be a real empty directory")
    out_candidate = canonical_new_path(out_arg)
    if is_relative_to(out_candidate, backing):
        raise E3Error("OUT_DIR must not be inside BACKING_DIR")
    if overlay_mounts():
        raise E3Error("refusing to run while an OverlayFS mount exists")
    mount = find_mount(backing)
    if mount["fstype"] not in FILESYSTEM_MAGICS or \
            filesystem_magic(backing) != FILESYSTEM_MAGICS[mount["fstype"]]:
        raise E3Error("BACKING_DIR must be on matching ext4 or XFS")
    device = validate_device_stat(backing, device_arg)
    fs_config, xfs_info = xfs_configuration(backing, mount)
    out_dir = validate_output_dir(out_arg)
    if out_dir.stat().st_dev == backing.stat().st_dev:
        raise E3Error("OUT_DIR must be on a different device from BACKING_DIR")
    return backing, device, out_dir, mount, fs_config, xfs_info


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def kernel_config_sha256() -> str:
    candidates = (
        pathlib.Path("/proc/config.gz"), pathlib.Path(f"/boot/config-{platform.release()}"),
        pathlib.Path(__file__).resolve().parents[4] / ".config",
    )
    for candidate in candidates:
        if candidate.is_file():
            return sha256_file(candidate)
    return ""


def git_commit() -> str:
    root = pathlib.Path(__file__).resolve().parents[4]
    process = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
    )
    return process.stdout.strip() if process.returncode == 0 else "unknown"


def build_manifest(preset: str, run_index: int, event_path: pathlib.Path,
                   values: list[dict[str, Any]],
                   mount: dict[str, str], fs_config: str, xfs_info: str,
                   device_arg: pathlib.Path, device: pathlib.Path) -> dict[str, Any]:
    configuration = events.PRESETS[preset]
    return {
        "schema": SCHEMA,
        "preset": preset,
        "run_index": run_index,
        "seed": events.SEED,
        "event_file": event_path.name,
        "event_file_sha256": sha256_file(event_path),
        "event_count": len(values),
        "git_commit": git_commit(),
        "kernel_release": platform.release(),
        "kernel_config_sha256": kernel_config_sha256(),
        "fs_type": mount["fstype"],
        "fs_config": fs_config,
        "fs_uuid": mount["uuid"],
        "backing_source": mount["source"],
        "backing_mount_options": mount["options"],
        "xfs_info": xfs_info,
        "device_stat": str(device_arg.absolute()),
        "canonical_device_stat": str(device),
        "overlay_mount_options": list(MOUNT_FEATURES),
        "deltafs_abi_version": DELTAFS_ABI_VERSION,
        "initial_generation": INITIAL_GENERATION,
        "checkpoint_generation": CHECKPOINT_GENERATION,
        "copyup_source": COPYUP_SOURCE,
        "started_at": utc_now(),
        "legal_cells": [[size * 1024, dirty] for size, dirty in events.legal_cells()],
        "warm_count_per_cell": configuration["warm"],
        "cold_count_per_cell": configuration["cold"],
        "independent_runs": configuration["runs"],
        "noop_interval": NOOP_INTERVAL,
        "noop_repetitions": NOOP_REPETITIONS,
        "settle_interval_ms": 100,
        "settle_stable_comparisons": 3,
        "settle_timeout_ms": 10000,
    }


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


def sample_id(event: dict[str, Any], sample_number: int) -> str:
    return f"r{event['run']:02d}-{event['cache_mode']}-s{sample_number:05d}-{event['event_id']}"


def create_sample(sample_dir: pathlib.Path, event: dict[str, Any] | None) -> None:
    (sample_dir / "base").mkdir(parents=True)
    (sample_dir / "generation-1" / "upper").mkdir(parents=True)
    (sample_dir / "generation-1" / "work").mkdir()
    (sample_dir / "generation-2" / "upper").mkdir(parents=True)
    (sample_dir / "generation-2" / "work").mkdir()
    (sample_dir / "layers").mkdir()
    (sample_dir / "merged").mkdir()
    if event is not None:
        before, _ = events.event_images(event)
        target = sample_dir / "generation-1" / "upper" / event["relative_path"]
        target.write_bytes(before)
        target.chmod(0o644)
        with target.open("rb") as stream:
            os.fsync(stream.fileno())
        if sha256_file(target) != event["expected_before_sha256"]:
            raise E3Error("generated generation-1 upper preimage hash mismatch")
    syncfs(sample_dir)
    device = sample_dir.stat().st_dev
    for path in sample_dir.rglob("*"):
        if path.stat().st_dev != device:
            raise E3Error(f"sample path crosses backing superblock: {path}")


def mount_sample(sample_dir: pathlib.Path, logs: Logs) -> None:
    options = [
        "lowerdir=base", "upperdir=generation-1/upper",
        "workdir=generation-1/work", *MOUNT_FEATURES,
    ]
    logs.subprocess(
        ["mount", "-t", "overlay", "overlay", "-o", ",".join(options), "merged"],
        cwd=sample_dir,
    )
    mount = find_mount(sample_dir / "merged")
    if mount["fstype"] != "overlay":
        raise E3Error("sample mount is not OverlayFS")
    conflicting = {
        "index=on", "nfs_export=on", "metacopy=on", "xino=on", "xino=auto",
        "uuid=on", "uuid=auto", "redirect_dir=on", "redirect_dir=follow",
        "redirect_dir=off",
    }
    found = sorted(set(mount["options"].split(",")) & conflicting)
    if found:
        raise E3Error(f"mounted OverlayFS feature mismatch: {found}")


def checkpoint_sample(sample_dir: pathlib.Path, helper: pathlib.Path, logs: Logs) -> None:
    initial_upper = sample_dir / "generation-1" / "upper"
    frozen_upper = sample_dir / "layers" / "g1"
    os.rename(initial_upper, frozen_upper)
    logs.subprocess([
        str(helper), str(sample_dir / "merged"), str(INITIAL_GENERATION),
        str(sample_dir / "generation-2" / "upper"),
        str(sample_dir / "generation-2" / "work"),
    ])


def unmount_sample(sample_dir: pathlib.Path, logs: Logs) -> None:
    logs.subprocess(["umount", "--", str(sample_dir / "merged")])


def read_helper_result(path: pathlib.Path, kind: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="ascii"))
    except (FileNotFoundError, json.JSONDecodeError, UnicodeError) as exc:
        raise E3Error(f"invalid helper result: {path}") from exc
    if not isinstance(value, dict) or set(value) != STAT_FIELDS or \
            value["schema"] != SCHEMA or value["kind"] != kind or \
            value["status"] not in ("ok", "invalid", "failed"):
        raise E3Error("helper result schema mismatch")
    return value


def raw_base(event: dict[str, Any], number: int, identity: str,
             fs_config: str, batch: int, fiemap: pathlib.Path,
             out_dir: pathlib.Path) -> dict[str, Any]:
    return {
        "schema": SCHEMA, "run": event["run"], "sample": number,
        "sample_id": identity, "sample_kind": "edit", "control_batch": batch,
        "cache_mode": event["cache_mode"], "fs_config": fs_config,
        "event_id": event["event_id"], "file_size_before": event["file_size_before"],
        "size_bin": event["size_bin"], "offset": event["offset"],
        "logical_bytes_changed": event["write_bytes"],
        "dirty_blocks": event["dirty_blocks"], "copyup_bytes": 0,
        "shared_bytes": 0, "allocated_bytes_total": 0,
        "copyup_amplification": 0.0, "sectors_before": 0, "sectors_after": 0,
        "physical_io_bytes": 0, "settle_timeout": False, "pre_sha256": "",
        "post_sha256": "", "upper_sha256": "", "lower_sha256": "",
        "fiemap_path": str(fiemap.relative_to(out_dir)), "fiemap_block_size": 0,
        "status": "failed", "errno": 0, "invalid_reason": None,
    }


def merge_edit_result(row: dict[str, Any], result: dict[str, Any]) -> None:
    for key in (
        "copyup_bytes", "shared_bytes", "allocated_bytes_total", "sectors_before",
        "sectors_after", "physical_io_bytes", "settle_timeout", "pre_sha256",
        "post_sha256", "upper_sha256", "lower_sha256", "fiemap_block_size",
        "status", "errno", "invalid_reason",
    ):
        row[key] = result[key]
    if row["logical_bytes_changed"]:
        row["copyup_amplification"] = (
            row["copyup_bytes"] / row["logical_bytes_changed"]
        )


def exception_reason(stage: str, error: BaseException) -> str:
    return f"{stage}: {str(error).replace(chr(10), ' ')}"[:1000]


def run_edit(backing: pathlib.Path, helper: pathlib.Path,
             checkpoint_helper: pathlib.Path, out_dir: pathlib.Path,
             raw_path: pathlib.Path, logs: Logs, device: pathlib.Path,
             fs_config: str, event: dict[str, Any], number: int,
             batch: int) -> dict[str, Any]:
    identity = sample_id(event, number)
    sample_dir = backing / ".e3-work" / identity
    result_path = sample_dir / "helper-result.json"
    fiemap = out_dir / "fiemap" / f"{identity}.json"
    row = raw_base(event, number, identity, fs_config, batch, fiemap, out_dir)
    mounted = False
    preserve = True
    stage = "prepare_sample"
    try:
        create_sample(sample_dir, event)
        stage = "mount_sample"
        mount_sample(sample_dir, logs)
        mounted = True
        stage = "checkpoint_sample"
        checkpoint_sample(sample_dir, checkpoint_helper, logs)
        stage = "run_helper"
        command = [
            str(helper), "edit", "--merged", str(sample_dir / "merged"),
            "--target", str(sample_dir / "merged" / "edit.bin"),
            "--upper", str(sample_dir / "generation-2" / "upper" / "edit.bin"),
            "--lower", str(sample_dir / "layers" / "g1" / "edit.bin"),
            "--device-stat", str(device), "--file-size", str(event["file_size_before"]),
            "--offset", str(event["offset"]), "--write-bytes", str(event["write_bytes"]),
            "--payload-seed", str(event["payload_seed"]),
            "--expected-before", event["expected_before_sha256"],
            "--expected-after", event["expected_after_sha256"],
            "--cache-mode", event["cache_mode"], "--fiemap-out", str(fiemap),
            "--out", str(result_path),
        ]
        process = logs.subprocess(command, check=False)
        result = read_helper_result(result_path, "edit")
        merge_edit_result(row, result)
        if process.returncode or result["status"] != "ok":
            raise E3Error(f"copyup_bench returned {result['status']}")
        stage = "unmount_sample"
        unmount_sample(sample_dir, logs)
        mounted = False
        preserve = False
    except BaseException as error:
        if row["status"] == "ok":
            row["status"] = "invalid"
        existing = row.get("invalid_reason")
        reason = exception_reason(stage, error)
        row["invalid_reason"] = f"{existing}; {reason}" if existing else reason
        logs.error(f"FAIL: {identity}: {row['invalid_reason']}")
    finally:
        if not mounted and (sample_dir / "merged").is_dir():
            mounted = os.path.ismount(sample_dir / "merged")
        if mounted:
            try:
                unmount_sample(sample_dir, logs)
                mounted = False
            except BaseException as error:
                row["status"] = "failed"
                reason = exception_reason("umount_after_failure", error)
                row["invalid_reason"] = f"{row['invalid_reason']}; {reason}"
        if not preserve and row["status"] == "ok":
            try:
                shutil.rmtree(sample_dir)
            except BaseException as error:
                row["status"] = "failed"
                row["invalid_reason"] = exception_reason("remove_sample", error)
        append_jsonl(raw_path, row)
    return row


def run_control(backing: pathlib.Path, helper: pathlib.Path,
                checkpoint_helper: pathlib.Path, controls_path: pathlib.Path,
                logs: Logs, device: pathlib.Path, fs_config: str, run: int,
                cache_mode: str, sample: int, batch: int,
                replica: int) -> dict[str, Any]:
    identity = f"r{run:02d}-{cache_mode}-b{batch:04d}-control-{replica}"
    sample_dir = backing / ".e3-work" / identity
    result_path = sample_dir / "helper-result.json"
    row = {
        "schema": SCHEMA, "run": run, "sample": sample, "sample_id": identity,
        "sample_kind": "control", "cache_mode": cache_mode, "fs_config": fs_config,
        "control_batch": batch, "replica": replica, "sectors_before": 0,
        "sectors_after": 0, "physical_io_bytes": 0, "settle_timeout": False,
        "status": "failed", "errno": 0, "invalid_reason": None,
    }
    mounted = False
    preserve = True
    stage = "prepare_control"
    try:
        create_sample(sample_dir, None)
        stage = "mount_control"
        mount_sample(sample_dir, logs)
        mounted = True
        stage = "checkpoint_control"
        checkpoint_sample(sample_dir, checkpoint_helper, logs)
        stage = "run_control_helper"
        process = logs.subprocess([
            str(helper), "control", "--merged", str(sample_dir / "merged"),
            "--device-stat", str(device), "--out", str(result_path),
        ], check=False)
        result = read_helper_result(result_path, "control")
        for key in ("sectors_before", "sectors_after", "physical_io_bytes",
                    "settle_timeout", "status", "errno", "invalid_reason"):
            row[key] = result[key]
        if process.returncode or result["status"] != "ok":
            raise E3Error(f"control helper returned {result['status']}")
        stage = "unmount_control"
        unmount_sample(sample_dir, logs)
        mounted = False
        preserve = False
    except BaseException as error:
        if row["status"] == "ok":
            row["status"] = "invalid"
        existing = row.get("invalid_reason")
        reason = exception_reason(stage, error)
        row["invalid_reason"] = f"{existing}; {reason}" if existing else reason
        logs.error(f"FAIL: {identity}: {row['invalid_reason']}")
    finally:
        if not mounted and (sample_dir / "merged").is_dir():
            mounted = os.path.ismount(sample_dir / "merged")
        if mounted:
            try:
                unmount_sample(sample_dir, logs)
            except BaseException as error:
                row["status"] = "failed"
                row["invalid_reason"] = exception_reason("umount_control_failure", error)
        if not preserve and row["status"] == "ok":
            try:
                shutil.rmtree(sample_dir)
            except BaseException as error:
                row["status"] = "failed"
                row["invalid_reason"] = exception_reason("remove_control", error)
        append_jsonl(controls_path, row)
    return row


def planned_batches(values: list[dict[str, Any]]) -> list[tuple[int, str, int, list[dict[str, Any]]]]:
    result = []
    for (run, cache), group_iterator in itertools.groupby(
            values, key=lambda value: (value["run"], value["cache_mode"])):
        group = list(group_iterator)
        for start in range(0, len(group), NOOP_INTERVAL):
            result.append((run, cache, start // NOOP_INTERVAL + 1,
                           group[start:start + NOOP_INTERVAL]))
    return result


def read_dmesg(logs: Logs) -> str:
    process = subprocess.run(["dmesg"], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, check=False)
    if process.returncode:
        if process.stderr:
            logs.stderr.write(process.stderr)
        raise E3Error("cannot read dmesg as root")
    return process.stdout


def new_dmesg(before: str, after: str) -> str:
    if after.startswith(before):
        return after[len(before):]
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    for overlap in range(min(len(before_lines), len(after_lines)), 0, -1):
        if before_lines[-overlap:] == after_lines[:overlap]:
            return "\n".join(after_lines[overlap:])
    raise E3Error("dmesg ring changed without a detectable overlap")


def summarize(manifest: dict[str, Any], rows: list[dict[str, Any]],
              controls: list[dict[str, Any]], dmesg_failures: list[dict[str, Any]],
              completed: bool) -> dict[str, Any]:
    counts = collections.Counter(row["status"] for row in rows)
    control_counts = collections.Counter(row["status"] for row in controls)
    cells = len(events.legal_cells())
    planned_batches_count = sum(
        (cells * manifest[field] + NOOP_INTERVAL - 1) // NOOP_INTERVAL
        for field in ("warm_count_per_cell", "cold_count_per_cell")
    )
    planned_controls = planned_batches_count * NOOP_REPETITIONS
    passed = (
        completed and len(rows) == manifest["event_count"] and counts == {"ok": len(rows)}
        and len(controls) == planned_controls and control_counts == {"ok": len(controls)}
        and not dmesg_failures
    )
    return {
        "schema": SCHEMA, "preset": manifest["preset"], "completed": completed,
        "passed": passed,
        "counts": {status: counts[status] for status in ("ok", "invalid", "failed")},
        "control_counts": {
            status: control_counts[status] for status in ("ok", "invalid", "failed")
        },
        "planned_edits": manifest["event_count"], "planned_controls": planned_controls,
        "dmesg_failures": dmesg_failures, "finished_at": utc_now(),
    }


def run_benchmark(preset: str, run_index: int, backing: pathlib.Path,
                  device: pathlib.Path,
                  device_arg: pathlib.Path, out_dir: pathlib.Path,
                  mount: dict[str, str], fs_config: str, xfs_info: str) -> int:
    logs = Logs(out_dir)
    helper = pathlib.Path(__file__).resolve().with_name("copyup_bench")
    checkpoint_helper = pathlib.Path(__file__).resolve().with_name("checkpoint_v2")
    event_path = out_dir / "events.jsonl"
    raw_path = out_dir / "raw.jsonl"
    controls_path = out_dir / "controls.jsonl"
    values = events.generate_events(preset, run_index)
    atomic_write_bytes(event_path, events.canonical_jsonl(values))
    manifest = build_manifest(
        preset, run_index, event_path, values, mount, fs_config, xfs_info,
        device_arg, device,
    )
    atomic_write_json(out_dir / "manifest.json", manifest)
    rows: list[dict[str, Any]] = []
    controls: list[dict[str, Any]] = []
    dmesg_failures: list[dict[str, Any]] = []
    completed = True
    dmesg_before = ""
    dmesg_cursor = ""
    try:
        if not helper.is_file() or not os.access(helper, os.X_OK):
            raise E3Error("copyup_bench is not built; run make -C tools/deltafs e3-bench")
        if not checkpoint_helper.is_file() or not os.access(checkpoint_helper, os.X_OK):
            raise E3Error("checkpoint_v2 is not built; run make -C tools/deltafs e3-bench")
        (out_dir / "fiemap").mkdir()
        (backing / ".e3-work").mkdir(mode=0o700)
        dmesg_before = read_dmesg(logs)
        dmesg_cursor = dmesg_before
        atomic_write_bytes(out_dir / "dmesg-before.log", dmesg_before.encode())
        edit_number = 0
        control_number = 0
        current_run = 0
        run_failed = False
        for run, cache_mode, batch, group in planned_batches(values):
            if run != current_run:
                if current_run:
                    after = read_dmesg(logs)
                    delta = new_dmesg(dmesg_cursor, after)
                    match = DMESG_FAILURE.search(delta)
                    if match:
                        dmesg_failures.append({"run": current_run, "keyword": match.group(0)})
                        completed = False
                        break
                    dmesg_cursor = after
                current_run = run
                logs.info(f"E3: independent run {run}/{events.PRESETS[preset]['runs']}")
            for event in group:
                edit_number += 1
                row = run_edit(
                    backing, helper, checkpoint_helper, out_dir, raw_path, logs,
                    device, fs_config, event, edit_number, batch,
                )
                rows.append(row)
                if row["status"] != "ok":
                    run_failed = True
                    completed = False
                    break
            if run_failed:
                break
            for replica in range(1, NOOP_REPETITIONS + 1):
                control_number += 1
                control = run_control(
                    backing, helper, checkpoint_helper, controls_path, logs,
                    device, fs_config, run, cache_mode, control_number, batch,
                    replica,
                )
                controls.append(control)
                if control["status"] != "ok":
                    run_failed = True
                    completed = False
                    break
            if run_failed:
                break
        after = read_dmesg(logs)
        atomic_write_bytes(out_dir / "dmesg-after.log", after.encode())
        if current_run and not dmesg_failures:
            delta = new_dmesg(dmesg_cursor, after)
            match = DMESG_FAILURE.search(delta)
            if match:
                dmesg_failures.append({"run": current_run, "keyword": match.group(0)})
                completed = False
        if overlay_mounts():
            completed = False
            logs.error("FAIL: E3 left an OverlayFS mount")
        work = backing / ".e3-work"
        if work.is_dir() and not any(work.iterdir()):
            work.rmdir()
    except BaseException as error:
        completed = False
        logs.error(f"FAIL: E3 runner: {error}")
        if dmesg_before and not (out_dir / "dmesg-after.log").exists():
            try:
                atomic_write_bytes(out_dir / "dmesg-after.log", read_dmesg(logs).encode())
            except BaseException:
                pass
    summary = summarize(manifest, rows, controls, dmesg_failures, completed)
    atomic_write_json(out_dir / "summary.json", summary)
    counts = summary["counts"]
    prefix = "PASS" if summary["passed"] else "FAIL"
    message = (
        f"{prefix}: E3 {preset} {fs_config} completed; ok={counts['ok']} "
        f"invalid={counts['invalid']} failed={counts['failed']} "
        f"controls={summary['control_counts']['ok']}"
    )
    (logs.info if summary["passed"] else logs.error)(message)
    logs.close()
    return 0 if summary["passed"] else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure DeltaFS v2 post-checkpoint copy-up and device I/O",
    )
    subparsers = parser.add_subparsers(dest="preset", required=True)
    smoke = subparsers.add_parser("smoke", help="run one sample per legal cell")
    full = subparsers.add_parser("run", help="run one indexed full-result shard")
    full.add_argument("run_index", type=int, metavar="RUN_INDEX")
    for subparser in (smoke, full):
        subparser.add_argument("backing_dir", type=pathlib.Path, metavar="BACKING_DIR")
        subparser.add_argument("device_stat", type=pathlib.Path, metavar="DEVICE_STAT")
        subparser.add_argument("out_dir", type=pathlib.Path, metavar="OUT_DIR")
    parsed = parser.parse_args(argv)
    if parsed.preset == "smoke":
        parsed.run_index = 1
    elif not 1 <= parsed.run_index <= events.PRESETS["run"]["runs"]:
        full.error("RUN_INDEX must be an integer from 1 to 5")
    return parsed


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        environment = validate_environment(
            arguments.backing_dir, arguments.device_stat, arguments.out_dir,
        )
    except (E3Error, OSError) as error:
        print(f"FAIL: E3 environment: {error}", file=sys.stderr)
        return 2
    backing, device, out_dir, mount, fs_config, xfs_info = environment
    try:
        return run_benchmark(
            arguments.preset, arguments.run_index, backing, device, arguments.device_stat,
            out_dir, mount, fs_config, xfs_info,
        )
    except (E3Error, OSError, events.EventError) as error:
        print(f"FAIL: E3 runner: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
