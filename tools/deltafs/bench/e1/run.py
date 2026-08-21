#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""DeltaFS E1 fixed-preset switch ioctl benchmark runner."""

from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import errno
import fcntl
import hashlib
import json
import os
import pathlib
import platform
import random
import re
import resource
import shutil
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


SCHEMA = 2
DELTAFS_ABI_VERSION = 2
SEED = 14857
MAX_LOWERS = 128
MOUNT_FEATURES = (
    "index=off",
    "nfs_export=off",
    "metacopy=off",
    "xino=off",
    "uuid=off",
    "redirect_dir=nofollow",
)
PRESETS = {
    "smoke": {
        "checkpoint_depths": (1, 127),
        "restore_depths": (1, 128),
        "warmup": 2,
        "measured": 5,
        "runs": 1,
    },
    "run": {
        "checkpoint_depths": (1, 2, 4, 8, 16, 32, 64, 127),
        "restore_depths": (1, 2, 4, 8, 16, 32, 64, 128),
        "warmup": 20,
        "measured": 200,
        "runs": 5,
    },
}
MARKER_PREFIX = ".e1-marker-"
WRITE_PROBE = ".e1-write-probe"
WORK_ENTRIES = frozenset(("work", "index"))
FILESYSTEM_MAGICS = {"ext4": 0xEF53, "xfs": 0x58465342}
DMESG_FAILURE = re.compile(
    r"(?:\bBUG:|\bWARNING:|KASAN|KFENCE|UBSAN|lockdep|"
    r"RCU (?:stall|warning)|rcu_preempt detected stalls)",
    re.IGNORECASE,
)

# Linux asm-generic ioctl encoding used by the DeltaFS UAPI.
_IOC_WRITE = 1
_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = 8
_IOC_SIZESHIFT = 16
_IOC_DIRSHIFT = 30
RESTORE_REQUEST_FORMAT = "=IIQQII130i4Q"
REQUEST_SIZE = 584
DELTAFS_IOC_RESTORE = (
    (_IOC_WRITE << _IOC_DIRSHIFT)
    | (REQUEST_SIZE << _IOC_SIZESHIFT)
    | (0xDF << _IOC_TYPESHIFT)
    | (0x02 << _IOC_NRSHIFT)
)


class E1Error(RuntimeError):
    """An E1 contract or execution failure."""


@dataclass(frozen=True)
class Attempt:
    operation: str
    source_depth: int
    target_depth: int
    request_depth: int
    rollback_distance: int
    warmup: bool
    negative: bool = False


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
            command,
            cwd=cwd,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if process.stdout:
            self.stdout.write(process.stdout)
            self.stdout.flush()
        if process.stderr:
            self.stderr.write(process.stderr)
            self.stderr.flush()
        if check and process.returncode:
            detail = process.stderr.strip() or process.stdout.strip()
            raise E1Error(
                f"command failed ({process.returncode}): {' '.join(command)}"
                + (f": {detail}" if detail else "")
            )
        return process


def atomic_write_bytes(path: pathlib.Path, data: bytes) -> None:
    temporary = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC
    fd = os.open(temporary, flags, 0o600)
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


def write_all_fd(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        if written == 0:
            raise OSError(errno.EIO, "zero-length write")
        view = view[written:]


def atomic_write_json(path: pathlib.Path, value: Any) -> None:
    data = (json.dumps(value, sort_keys=True, indent=2, ensure_ascii=False) + "\n").encode()
    atomic_write_bytes(path, data)


def append_jsonl(path: pathlib.Path, value: dict[str, Any]) -> None:
    data = (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()
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
        raise E1Error(
            f"command failed ({process.returncode}): {' '.join(command)}: "
            f"{process.stderr.strip()}"
        )
    return process.stdout


def canonical_new_path(path: pathlib.Path) -> pathlib.Path:
    if path.exists():
        return path.resolve(strict=True)
    parent = path.parent.resolve(strict=True)
    return parent / path.name


def is_relative_to(path: pathlib.Path, parent: pathlib.Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def find_mount(path: pathlib.Path) -> dict[str, str]:
    output = command_output(
        ["findmnt", "-J", "-T", str(path),
         "-o", "SOURCE,TARGET,FSTYPE,OPTIONS,FS-OPTIONS,UUID"]
    )
    try:
        filesystems = json.loads(output)["filesystems"]
        entry = filesystems[0]
    except (KeyError, IndexError, TypeError, json.JSONDecodeError) as exc:
        raise E1Error("findmnt returned an invalid JSON document") from exc
    return {
        "source": str(entry.get("source", "")),
        "target": str(entry.get("target", "")),
        "fstype": str(entry.get("fstype", "")),
        "options": str(entry.get("options", "")),
        "fs_options": str(entry.get("fs-options", "")),
        "uuid": str(entry.get("uuid") or ""),
    }


def validate_output_dir(path: pathlib.Path) -> pathlib.Path:
    if path.is_symlink():
        raise E1Error("OUT_DIR must not be a symlink")
    canonical = canonical_new_path(path)
    if canonical.exists():
        if not canonical.is_dir():
            raise E1Error("OUT_DIR must be a directory")
        if any(canonical.iterdir()):
            raise E1Error("OUT_DIR must not exist or must be empty")
    else:
        canonical.mkdir(mode=0o700)
    return canonical


def overlay_mounts() -> list[str]:
    mounts = []
    with open("/proc/self/mountinfo", encoding="utf-8") as stream:
        for line in stream:
            if " - overlay " in line:
                fields = line.split()
                mounts.append(fields[4] if len(fields) > 4 else line.strip())
    return mounts


def filesystem_magic(path: pathlib.Path) -> int:
    class StatFs(ctypes.Structure):
        _fields_ = [
            ("f_type", ctypes.c_long),
            ("f_bsize", ctypes.c_long),
            ("f_blocks", ctypes.c_ulonglong),
            ("f_bfree", ctypes.c_ulonglong),
            ("f_bavail", ctypes.c_ulonglong),
            ("f_files", ctypes.c_ulonglong),
            ("f_ffree", ctypes.c_ulonglong),
            ("f_fsid", ctypes.c_int * 2),
            ("f_namelen", ctypes.c_long),
            ("f_frsize", ctypes.c_long),
            ("f_flags", ctypes.c_long),
            ("f_spare", ctypes.c_long * 4),
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


def validate_environment(backing_arg: pathlib.Path,
                         out_arg: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path, dict[str, str], int]:
    if os.geteuid() != 0:
        raise E1Error("E1 must run as root")
    if not hasattr(time_module(), "CLOCK_MONOTONIC_RAW"):
        raise E1Error("CLOCK_MONOTONIC_RAW is unavailable")
    time_module().clock_gettime(time_module().CLOCK_MONOTONIC_RAW)
    for command in ("findmnt", "mount", "umount", "dmesg"):
        if shutil.which(command) is None:
            raise E1Error(f"required command not found: {command}")
    if backing_arg.is_symlink():
        raise E1Error("BACKING_DIR must not be a symlink")
    backing = backing_arg.resolve(strict=True)
    if not backing.is_dir():
        raise E1Error("BACKING_DIR must be a real directory")
    if any(backing.iterdir()):
        raise E1Error("BACKING_DIR must be empty")
    out_candidate = canonical_new_path(out_arg)
    if is_relative_to(out_candidate, backing):
        raise E1Error("OUT_DIR must not be inside BACKING_DIR")
    if overlay_mounts():
        raise E1Error("refusing to run while an OverlayFS mount exists")
    mount = find_mount(backing)
    if mount["fstype"] not in ("ext4", "xfs"):
        raise E1Error(
            f"BACKING_DIR must be on ext4 or XFS, found {mount['fstype']!r}"
        )
    magic = filesystem_magic(backing)
    if magic != FILESYSTEM_MAGICS[mount["fstype"]]:
        raise E1Error(
            f"findmnt/statfs filesystem mismatch: {mount['fstype']} magic=0x{magic:x}"
        )
    if not os.access(backing, os.R_OK | os.W_OK | os.X_OK):
        raise E1Error("BACKING_DIR is not accessible")
    soft_limit, _ = resource.getrlimit(resource.RLIMIT_NOFILE)
    if soft_limit < 192:
        raise E1Error("RLIMIT_NOFILE must be at least 192")
    affinity = sorted(os.sched_getaffinity(0))
    if not affinity:
        raise E1Error("the process has no available CPU")
    os.sched_setaffinity(0, {affinity[0]})
    out_dir = validate_output_dir(out_arg)
    return backing, out_dir, mount, affinity[0]


def time_module():
    # Kept behind a function so host-safe tests can exercise preset logic
    # without patching module globals.
    import time
    return time


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def kernel_config() -> tuple[str, str]:
    candidates = (
        pathlib.Path("/proc/config.gz"),
        pathlib.Path(f"/boot/config-{platform.release()}"),
        pathlib.Path(__file__).resolve().parents[4] / ".config",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate), sha256_file(candidate)
    return "", ""


def git_commit(repo_root: pathlib.Path) -> str:
    process = subprocess.run(
        ["git", "rev-parse", "HEAD"], cwd=repo_root, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False,
    )
    return process.stdout.strip() if process.returncode == 0 else "unknown"


def build_manifest(preset_name: str, mount: dict[str, str], cpu: int) -> dict[str, Any]:
    preset = PRESETS[preset_name]
    repo_root = pathlib.Path(__file__).resolve().parents[4]
    _, config_hash = kernel_config()
    clocksource_path = pathlib.Path(
        "/sys/devices/system/clocksource/clocksource0/current_clocksource"
    )
    clocksource = clocksource_path.read_text(encoding="utf-8").strip() \
        if clocksource_path.is_file() else "unknown"
    return {
        "schema": SCHEMA,
        "deltafs_abi_version": DELTAFS_ABI_VERSION,
        "preset": preset_name,
        "seed": SEED,
        "git_commit": git_commit(repo_root),
        "kernel_release": platform.release(),
        "kernel_config_sha256": config_hash,
        "fs_type": mount["fstype"],
        "fs_uuid": mount["uuid"],
        "backing_source": mount["source"],
        "backing_mount_options": mount["options"],
        "deltafs_mount_options": list(MOUNT_FEATURES),
        "cpu": cpu,
        "clocksource": clocksource,
        "started_at": utc_now(),
        "depth_matrix": {
            "checkpoint": list(preset["checkpoint_depths"]),
            "restore": list(preset["restore_depths"]),
        },
        "warmup_count": preset["warmup"],
        "measured_count": preset["measured"],
        "independent_runs": preset["runs"],
    }


def planned_attempts(preset_name: str, run_number: int) -> list[Attempt]:
    preset = PRESETS[preset_name]
    warmups: list[Attempt] = []
    measured: list[Attempt] = []
    for operation, depths in (
        ("checkpoint", preset["checkpoint_depths"]),
        ("restore", preset["restore_depths"]),
    ):
        for depth in depths:
            source_depth = depth if operation == "checkpoint" else MAX_LOWERS
            target_depth = depth + 1 if operation == "checkpoint" else depth
            request_depth = target_depth
            rollback = 0 if operation == "checkpoint" else MAX_LOWERS - depth
            for _ in range(preset["warmup"]):
                warmups.append(Attempt(operation, source_depth, target_depth,
                                       request_depth, rollback, True))
            for _ in range(preset["measured"]):
                measured.append(Attempt(operation, source_depth, target_depth,
                                        request_depth, rollback, False))
    randomizer = random.Random(SEED + run_number)
    randomizer.shuffle(warmups)
    randomizer.shuffle(measured)
    negative = Attempt("checkpoint", MAX_LOWERS, MAX_LOWERS + 1,
                       MAX_LOWERS + 1, 0, False, True)
    return [negative, *warmups, *measured]


def marker(run_number: int, attempt: Attempt, layer: int) -> tuple[str, str]:
    name = f"{MARKER_PREFIX}{layer:03d}"
    identity = (
        f"schema={SCHEMA};seed={SEED};run={run_number};"
        f"operation={attempt.operation};source={attempt.source_depth};"
        f"target={attempt.target_depth};layer={layer}"
    )
    digest = hashlib.sha256(identity.encode()).hexdigest()
    return name, f"{identity};sha256={digest}\n"


def source_lower_paths(sample_dir: pathlib.Path, depth: int) -> list[pathlib.Path]:
    paths = [sample_dir / "layers" / f"l{layer:03d}"
             for layer in range(depth - 1, 0, -1)]
    paths.append(sample_dir / "base")
    return paths


def target_lower_paths(sample_dir: pathlib.Path, attempt: Attempt) -> list[pathlib.Path]:
    if attempt.operation == "restore":
        return source_lower_paths(sample_dir, attempt.target_depth)
    frozen = sample_dir / "layers" / f"l{attempt.source_depth:03d}"
    return [frozen, *source_lower_paths(sample_dir, attempt.source_depth)]


def expected_markers(run_number: int, attempt: Attempt, depth: int) -> dict[str, str]:
    return dict(marker(run_number, attempt, layer) for layer in range(depth))


def create_sample(sample_dir: pathlib.Path, run_number: int, attempt: Attempt) -> None:
    (sample_dir / "base").mkdir(parents=True)
    (sample_dir / "layers").mkdir()
    (sample_dir / "active" / "upper").mkdir(parents=True)
    (sample_dir / "active" / "work").mkdir()
    (sample_dir / "next" / "upper").mkdir(parents=True)
    (sample_dir / "next" / "work").mkdir()
    (sample_dir / "merged").mkdir()
    for layer in range(attempt.source_depth):
        directory = sample_dir / "base" if layer == 0 else \
            sample_dir / "layers" / f"l{layer:03d}"
        if layer:
            directory.mkdir()
        name, contents = marker(run_number, attempt, layer)
        marker_path = directory / name
        marker_path.write_text(contents, encoding="ascii")
        marker_path.chmod(0o444)
    backing_dev = sample_dir.stat().st_dev
    for path in sample_dir.rglob("*"):
        if path.stat().st_dev != backing_dev:
            raise E1Error(f"sample path crosses a backing superblock: {path}")


def mount_source(sample_dir: pathlib.Path, attempt: Attempt, logs: Logs) -> None:
    relative_lowers = [str(path.relative_to(sample_dir))
                       for path in source_lower_paths(sample_dir, attempt.source_depth)]
    options = [
        f"lowerdir={':'.join(relative_lowers)}",
        "upperdir=active/upper",
        "workdir=active/work",
        *MOUNT_FEATURES,
    ]
    logs.subprocess(
        ["mount", "-t", "overlay", "overlay", "-o", ",".join(options), "merged"],
        cwd=sample_dir,
    )
    mount = find_mount(sample_dir / "merged")
    if mount["fstype"] != "overlay":
        raise E1Error("mounted source is not OverlayFS")
    # show_options omits explicit values that equal the kernel default. Reject
    # a conflicting value when one is reported; the mount command above still
    # supplies every required DeltaFS option explicitly.
    option_set = set(mount["options"].split(","))
    conflicts = {
        "index=on", "nfs_export=on", "metacopy=on", "xino=on",
        "xino=auto", "uuid=on", "uuid=auto", "redirect_dir=on",
        "redirect_dir=follow", "redirect_dir=off",
    }
    reported = sorted(option_set & conflicts)
    if reported:
        raise E1Error(f"mounted OverlayFS feature mismatch: {reported}")


def merged_markers(merged: pathlib.Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for entry in merged.iterdir():
        if entry.name.startswith(MARKER_PREFIX):
            if not entry.is_file():
                raise E1Error(f"marker is not a regular file: {entry}")
            result[entry.name] = entry.read_text(encoding="ascii")
        elif entry.name != WRITE_PROBE:
            raise E1Error(f"unexpected merged entry: {entry.name}")
    return result


def verify_merged(merged: pathlib.Path, expected: dict[str, str]) -> None:
    actual = merged_markers(merged)
    if actual != expected:
        missing = sorted(set(expected) - set(actual))
        extra = sorted(set(actual) - set(expected))
        wrong = sorted(name for name in set(actual) & set(expected)
                       if actual[name] != expected[name])
        raise E1Error(
            f"merged marker mismatch: missing={missing} extra={extra} wrong={wrong}"
        )


def lower_snapshot(sample_dir: pathlib.Path) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    candidates = [sample_dir / "base"]
    candidates.extend(sorted((sample_dir / "layers").glob("l[0-9][0-9][0-9]")))
    for directory in candidates:
        stat = directory.stat()
        markers = sorted(directory.glob(f"{MARKER_PREFIX}*"))
        result[str(directory)] = {
            "inode": stat.st_ino,
            "size": stat.st_size,
            "mtime_ns": stat.st_mtime_ns,
            "markers": [
                {
                    "name": path.name,
                    "inode": path.stat().st_ino,
                    "size": path.stat().st_size,
                    "mtime_ns": path.stat().st_mtime_ns,
                    "sha256": sha256_file(path),
                }
                for path in markers
            ],
        }
    return result


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


def write_spec(sample_dir: pathlib.Path, attempt: Attempt, cpu: int) -> pathlib.Path:
    lower_prefix: list[pathlib.Path] = []
    keep_bottom = attempt.target_depth if attempt.operation == "restore" else 0
    all_paths = [sample_dir / "merged", sample_dir / "next" / "upper",
                 sample_dir / "next" / "work", *lower_prefix]
    if any(len(str(path)) >= 4096 for path in all_paths):
        raise E1Error("an E1 switch path reaches PATH_MAX")
    spec = {
        "schema": SCHEMA,
        "operation": attempt.operation,
        "cpu": cpu,
        "source_depth": attempt.source_depth,
        "expected_generation": 1,
        "keep_bottom": keep_bottom,
        "merged": str(sample_dir / "merged"),
        "upper": str(sample_dir / "next" / "upper"),
        "work": str(sample_dir / "next" / "work"),
        "lower_prefix": [str(path) for path in lower_prefix],
    }
    spec_path = sample_dir / "spec.json"
    atomic_write_json(spec_path, spec)
    return spec_path


def prepare_spec(sample_dir: pathlib.Path, attempt: Attempt, cpu: int) -> pathlib.Path:
    if attempt.operation == "checkpoint" and not attempt.negative:
        frozen = sample_dir / "layers" / f"l{attempt.source_depth:03d}"
        (sample_dir / "active" / "upper").rename(frozen)
    return write_spec(sample_dir, attempt, cpu)


def run_driver(switch_once: pathlib.Path, spec_path: pathlib.Path,
               result_path: pathlib.Path, logs: Logs) -> dict[str, Any]:
    process = logs.subprocess(
        [str(switch_once), str(spec_path), str(result_path)], check=False
    )
    if not result_path.is_file():
        raise E1Error(f"switch_once exited {process.returncode} without result.json")
    try:
        result = json.loads(result_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise E1Error("switch_once produced invalid result JSON") from exc
    required = {
        "schema", "ioctl_attempted", "ioctl_ret", "errno",
        "ioctl_latency_ns", "cpu_before", "cpu_after", "major_faults",
        "status", "invalid_reason",
    }
    if set(result) != required or result["schema"] != SCHEMA or \
            result["status"] not in ("ok", "expected_reject", "invalid", "failed"):
        raise E1Error(f"switch_once result violates schema {SCHEMA}")
    if process.returncode and result["status"] != "failed":
        raise E1Error(
            f"switch_once exited {process.returncode} for {result['status']} result"
        )
    return result


def invalid_generation_request(generation: int) -> bytearray:
    import struct
    values = [REQUEST_SIZE, DELTAFS_ABI_VERSION, 0, generation, 1, 2]
    values.extend([-1] * (MAX_LOWERS + 2))
    values.extend([0] * 4)
    request = bytearray(struct.pack(RESTORE_REQUEST_FORMAT, *values))
    if len(request) != REQUEST_SIZE:
        raise E1Error("Python DeltaFS request layout mismatch")
    return request


def expect_ioctl_errno(fd: int, generation: int, expected_errno: int) -> None:
    request = invalid_generation_request(generation)
    try:
        fcntl.ioctl(fd, DELTAFS_IOC_RESTORE, request, True)
    except OSError as exc:
        if exc.errno == expected_errno:
            return
        raise E1Error(
            f"generation {generation}: expected errno {expected_errno}, got {exc.errno}"
        ) from exc
    raise E1Error(
        f"generation {generation}: expected errno {expected_errno}, got success"
    )


def probe_generation(merged: pathlib.Path, generation: int) -> int:
    fd = os.open(merged, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        expect_ioctl_errno(fd, generation - 1, errno.ESTALE)
        expect_ioctl_errno(fd, generation, errno.EBADF)
    finally:
        os.close(fd)
    return generation


def verify_workdir(work: pathlib.Path) -> None:
    entries = {entry.name for entry in work.iterdir()}
    if not entries.issubset(WORK_ENTRIES):
        raise E1Error(f"fresh work contains non-OverlayFS entries: {sorted(entries)}")


def unescape_mount_option(value: str) -> str:
    result = bytearray()
    index = 0
    encoded = value.encode("utf-8")
    while index < len(encoded):
        if encoded[index:index + 1] == b"\\" and index + 3 < len(encoded) and \
                all(48 <= byte <= 55 for byte in encoded[index + 1:index + 4]):
            result.append(int(encoded[index + 1:index + 4], 8))
            index += 4
        else:
            result.append(encoded[index])
            index += 1
    return result.decode("utf-8")


def verify_published_paths(sample_dir: pathlib.Path, attempt: Attempt) -> None:
    mount = find_mount(sample_dir / "merged")
    options = split_mount_options(mount["fs_options"])
    lowers = [unescape_mount_option(option.split("=", 1)[1])
              for option in options if option.startswith("lowerdir+=")]
    expected_lowers = [str(path) for path in target_lower_paths(sample_dir, attempt)]
    upper = [unescape_mount_option(option.split("=", 1)[1])
             for option in options if option.startswith("upperdir=")]
    work = [unescape_mount_option(option.split("=", 1)[1])
            for option in options if option.startswith("workdir=")]
    if lowers != expected_lowers:
        raise E1Error("published lowerdir order differs from the switch request")
    if upper != [str(sample_dir / "next" / "upper")] or \
            work != [str(sample_dir / "next" / "work")]:
        raise E1Error("published upperdir/workdir differ from the switch request")


def split_mount_options(value: str) -> list[str]:
    options: list[str] = []
    start = 0
    index = 0
    while index < len(value):
        if value[index] == "\\" and index + 3 < len(value) and \
                all("0" <= character <= "7" for character in value[index + 1:index + 4]):
            index += 4
            continue
        if value[index] == ",":
            options.append(value[start:index])
            start = index + 1
        index += 1
    options.append(value[start:])
    return options


def verify_success(sample_dir: pathlib.Path, run_number: int,
                   attempt: Attempt, before: dict[str, dict[str, Any]]) -> int:
    target_marker_depth = attempt.source_depth if attempt.operation == "checkpoint" \
        else attempt.target_depth
    expected = expected_markers(run_number, attempt, target_marker_depth)
    verify_merged(sample_dir / "merged", expected)
    verify_published_paths(sample_dir, attempt)
    if lower_snapshot(sample_dir) != before:
        raise E1Error("lower metadata or marker content changed across switch")
    fresh_upper = sample_dir / "next" / "upper"
    if any(fresh_upper.iterdir()):
        raise E1Error("fresh upper is not empty before write probe")
    verify_workdir(sample_dir / "next" / "work")
    probe_path = sample_dir / "merged" / WRITE_PROBE
    probe_contents = "deltafs-e1-write-probe\n"
    probe_path.write_text(probe_contents, encoding="ascii")
    upper_probe = fresh_upper / WRITE_PROBE
    if not upper_probe.is_file() or upper_probe.read_text(encoding="ascii") != probe_contents:
        raise E1Error("write probe did not land in the fresh upper")
    for directory in (sample_dir / "base", sample_dir / "layers", sample_dir / "active"):
        if any(directory.rglob(WRITE_PROBE)):
            raise E1Error("write probe appeared outside the fresh upper")
    return probe_generation(sample_dir / "merged", 2)


def verify_negative(sample_dir: pathlib.Path, run_number: int,
                    attempt: Attempt, before: dict[str, dict[str, Any]],
                    result: dict[str, Any]) -> int:
    if result["status"] != "expected_reject" or result["errno"] != errno.E2BIG or \
            result["ioctl_attempted"]:
        raise E1Error("checkpoint@128 was not rejected by E2BIG preflight")
    verify_merged(
        sample_dir / "merged",
        expected_markers(run_number, attempt, attempt.source_depth),
    )
    if lower_snapshot(sample_dir) != before:
        raise E1Error("checkpoint@128 preflight changed lower metadata")
    if any((sample_dir / "next" / "upper").iterdir()) or \
            any((sample_dir / "next" / "work").iterdir()):
        raise E1Error("checkpoint@128 preflight changed fresh switch paths")
    # No generation-probe ioctl is allowed in this negative sample. Since the
    # driver did not issue an ioctl and every observable path is unchanged,
    # generation remains at the mount-time value.
    return 1


def raw_base(run_number: int, sample_number: int, attempt: Attempt) -> dict[str, Any]:
    return {
        "schema": SCHEMA,
        "run": run_number,
        "sample": sample_number,
        "warmup": attempt.warmup,
        "operation": attempt.operation,
        "source_depth": attempt.source_depth,
        "target_depth": attempt.target_depth,
        "request_depth": attempt.request_depth,
        "rollback_distance": attempt.rollback_distance,
        "keep_bottom": attempt.target_depth
        if attempt.operation == "restore" else 0,
        "prefix_depth": 0,
        "request_fd_count": 2,
        "expected_generation": 1,
        "generation_after": None,
        "cpu_before": -1,
        "cpu_after": -1,
        "major_faults": 0,
        "ioctl_ret": -1,
        "errno": 0,
        "ioctl_latency_ns": 0,
        "status": "failed",
        "invalid_reason": None,
    }


def merge_driver_result(row: dict[str, Any], result: dict[str, Any]) -> None:
    for key in (
        "cpu_before", "cpu_after", "major_faults", "ioctl_ret", "errno",
        "ioctl_latency_ns", "status", "invalid_reason",
    ):
        row[key] = result[key]


def exception_reason(stage: str, exc: BaseException) -> str:
    text = str(exc).replace("\n", " ")
    return f"{stage}: {text}"[:1000]


def unmount_sample(sample_dir: pathlib.Path, logs: Logs) -> None:
    logs.subprocess(["umount", "--", str(sample_dir / "merged")])


def run_attempt(backing: pathlib.Path, switch_once: pathlib.Path,
                raw_path: pathlib.Path, logs: Logs, cpu: int,
                run_number: int, sample_number: int,
                attempt: Attempt) -> dict[str, Any]:
    tag = "negative" if attempt.negative else ("warmup" if attempt.warmup else "measured")
    operation = "cp" if attempt.operation == "checkpoint" else "rs"
    sample_id = (
        f"r{run_number:02d}-{operation}-d{attempt.request_depth:03d}-"
        f"{tag}-s{sample_number:05d}"
    )
    sample_dir = backing / ".e1-work" / sample_id
    result_path = sample_dir / "result.json"
    row = raw_base(run_number, sample_number, attempt)
    mounted = False
    stage = "prepare_sample"
    preserve = True
    try:
        create_sample(sample_dir, run_number, attempt)
        spec_path = write_spec(sample_dir, attempt, cpu)
        stage = "mount_source"
        mount_source(sample_dir, attempt, logs)
        mounted = True
        stage = "verify_source"
        verify_merged(
            sample_dir / "merged",
            expected_markers(run_number, attempt, attempt.source_depth),
        )
        stage = "prepare_request"
        spec_path = prepare_spec(sample_dir, attempt, cpu)
        before = lower_snapshot(sample_dir)
        syncfs(sample_dir / "merged")
        stage = "run_once"
        result = run_driver(switch_once, spec_path, result_path, logs)
        merge_driver_result(row, result)
        stage = "verify_target"
        if attempt.negative:
            row["generation_after"] = verify_negative(
                sample_dir, run_number, attempt, before, result
            )
        elif result["ioctl_ret"] == 0:
            row["generation_after"] = verify_success(
                sample_dir, run_number, attempt, before
            )
        if attempt.negative:
            if result["status"] != "expected_reject":
                raise E1Error("negative gate did not return expected_reject")
        elif result["status"] != "ok":
            raise E1Error(f"switch_once returned {result['status']}")
        stage = "umount"
        unmount_sample(sample_dir, logs)
        mounted = False
        preserve = False
    except BaseException as exc:
        if row["status"] == "ok" or row["status"] == "expected_reject":
            row["status"] = "invalid"
        elif row["status"] not in ("invalid", "failed"):
            row["status"] = "failed"
        existing = row.get("invalid_reason")
        reason = exception_reason(stage, exc)
        row["invalid_reason"] = f"{existing}; {reason}" if existing else reason
        if isinstance(exc, OSError) and exc.errno and not row["errno"]:
            row["errno"] = exc.errno
        logs.error(f"FAIL: {sample_id}: {row['invalid_reason']}")
    finally:
        if not mounted and (sample_dir / "merged").is_dir():
            mounted = os.path.ismount(sample_dir / "merged")
        if mounted:
            try:
                unmount_sample(sample_dir, logs)
                mounted = False
            except BaseException as exc:
                row["status"] = "failed"
                reason = exception_reason("umount_after_failure", exc)
                existing = row.get("invalid_reason")
                row["invalid_reason"] = f"{existing}; {reason}" if existing else reason
        if not preserve and row["status"] in ("ok", "expected_reject"):
            try:
                shutil.rmtree(sample_dir)
            except BaseException as exc:
                row["status"] = "failed"
                row["invalid_reason"] = exception_reason("remove_sample", exc)
        append_jsonl(raw_path, row)
    return row


def read_dmesg(logs: Logs) -> str:
    process = subprocess.run(
        ["dmesg"], text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False,
    )
    if process.returncode:
        if process.stderr:
            logs.stderr.write(process.stderr)
            logs.stderr.flush()
        raise E1Error("cannot read dmesg as root")
    return process.stdout


def new_dmesg(before: str, after: str) -> str:
    if after.startswith(before):
        return after[len(before):]
    before_lines = before.splitlines()
    after_lines = after.splitlines()
    for overlap in range(min(len(before_lines), len(after_lines)), 0, -1):
        if before_lines[-overlap:] == after_lines[:overlap]:
            return "\n".join(after_lines[overlap:])
    raise E1Error("dmesg ring changed without a detectable overlap")


def summarize(manifest: dict[str, Any], rows: list[dict[str, Any]],
              dmesg_failures: list[dict[str, Any]], completed: bool) -> dict[str, Any]:
    counts = {status: 0 for status in ("ok", "expected_reject", "invalid", "failed")}
    for row in rows:
        counts[row["status"]] += 1
    preset = PRESETS[manifest["preset"]]
    cells = len(preset["checkpoint_depths"]) + len(preset["restore_depths"])
    planned_ok = preset["runs"] * cells * (preset["warmup"] + preset["measured"])
    planned_reject = preset["runs"]
    passed = (
        completed
        and counts == {
            "ok": planned_ok,
            "expected_reject": planned_reject,
            "invalid": 0,
            "failed": 0,
        }
        and not dmesg_failures
    )
    return {
        "schema": SCHEMA,
        "preset": manifest["preset"],
        "completed": completed,
        "passed": passed,
        "planned_ok": planned_ok,
        "planned_expected_reject": planned_reject,
        "counts": counts,
        "dmesg_failures": dmesg_failures,
        "finished_at": utc_now(),
    }


def run_benchmark(preset_name: str, backing: pathlib.Path, out_dir: pathlib.Path,
                  mount: dict[str, str], cpu: int) -> int:
    logs = Logs(out_dir)
    raw_path = out_dir / "raw.jsonl"
    try:
        manifest = build_manifest(preset_name, mount, cpu)
        atomic_write_json(out_dir / "manifest.json", manifest)
    except BaseException as exc:
        logs.error(f"FAIL: E1 manifest: {exc}")
        logs.close()
        return 1
    switch_once = pathlib.Path(__file__).resolve().with_name("switch_once")
    rows: list[dict[str, Any]] = []
    dmesg_failures: list[dict[str, Any]] = []
    completed = True
    dmesg_before = ""
    dmesg_cursor = ""
    try:
        if not switch_once.is_file() or not os.access(switch_once, os.X_OK):
            raise E1Error("switch_once is not built; run make -C tools/deltafs e1-bench")
        (backing / ".e1-work").mkdir(mode=0o700)
        dmesg_before = read_dmesg(logs)
        dmesg_cursor = dmesg_before
        atomic_write_bytes(out_dir / "dmesg-before.log", dmesg_before.encode())
        for run_number in range(1, PRESETS[preset_name]["runs"] + 1):
            logs.info(f"E1: independent run {run_number}/{PRESETS[preset_name]['runs']}")
            run_failed = False
            for sample_number, attempt in enumerate(
                    planned_attempts(preset_name, run_number), start=1):
                row = run_attempt(
                    backing, switch_once, raw_path, logs, cpu,
                    run_number, sample_number, attempt,
                )
                rows.append(row)
                if row["status"] not in ("ok", "expected_reject"):
                    run_failed = True
                    completed = False
                    break
            after_run = read_dmesg(logs)
            try:
                delta = new_dmesg(dmesg_cursor, after_run)
                match = DMESG_FAILURE.search(delta)
                if match:
                    dmesg_failures.append({
                        "run": run_number,
                        "keyword": match.group(0),
                        "excerpt": delta[max(0, match.start() - 240):match.end() + 480],
                    })
                    run_failed = True
                    completed = False
            except E1Error as exc:
                dmesg_failures.append({"run": run_number, "keyword": "dmesg-overlap",
                                       "excerpt": str(exc)})
                run_failed = True
                completed = False
            dmesg_cursor = after_run
            atomic_write_bytes(out_dir / "dmesg-after.log", after_run.encode())
            if run_failed:
                logs.error(f"FAIL: independent run {run_number} stopped")
            residual_mounts = overlay_mounts()
            if residual_mounts:
                completed = False
                logs.error(
                    "FAIL: aborting E1 because OverlayFS mounts remain: "
                    + ", ".join(residual_mounts)
                )
                break
        work_root = backing / ".e1-work"
        if work_root.is_dir() and not any(work_root.iterdir()):
            work_root.rmdir()
    except BaseException as exc:
        completed = False
        logs.error(f"FAIL: E1 runner: {exc}")
        if dmesg_before and not (out_dir / "dmesg-after.log").exists():
            try:
                after = read_dmesg(logs)
                atomic_write_bytes(out_dir / "dmesg-after.log", after.encode())
            except BaseException:
                pass
    finally:
        summary = summarize(manifest, rows, dmesg_failures, completed)
        atomic_write_json(out_dir / "summary.json", summary)
        counts = summary["counts"]
        prefix = "PASS" if summary["passed"] else "FAIL"
        message = (
            f"{prefix}: E1 {preset_name} completed; ok={counts['ok']} "
            f"invalid={counts['invalid']} failed={counts['failed']} "
            f"expected_reject={counts['expected_reject']}"
        )
        if summary["passed"]:
            logs.info(message)
        else:
            logs.error(message)
        logs.close()
    return 0 if summary["passed"] else 1


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure DeltaFS checkpoint/restore switch ioctl latency"
    )
    parser.add_argument("preset", choices=tuple(PRESETS))
    parser.add_argument("backing_dir", type=pathlib.Path)
    parser.add_argument("out_dir", type=pathlib.Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        backing, out_dir, mount, cpu = validate_environment(
            arguments.backing_dir, arguments.out_dir
        )
    except (E1Error, OSError) as exc:
        print(f"FAIL: E1 environment: {exc}", file=sys.stderr)
        return 2
    try:
        return run_benchmark(arguments.preset, backing, out_dir, mount, cpu)
    except (E1Error, OSError) as exc:
        print(f"FAIL: E1 runner: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
