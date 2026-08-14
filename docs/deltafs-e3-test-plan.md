# DeltaFS v1 E3 copy-up benchmark detailed design

> E3 measures one OverlayFS copy-up edit. It is independent of the E2 switch
> ioctl benchmark and does not use `deltafsctl` or any DeltaFS switch request.
>
> Without the authors' SWE-Search event corpus, the fixed synthetic preset is a
> method reproduction. Its output must not be described as an exact Fig. 9
> artifact reproduction.

## 1. Questions, scope, and terminology

E3 compares three backing filesystem configurations:

- `ext4_noreflink`;
- `xfs_noreflink` (`mkfs.xfs -m reflink=0`);
- `xfs_reflink` (`mkfs.xfs -m reflink=1`).

For each immutable edit event it measures:

1. logical bytes changed by one aligned `pwrite`;
2. upper-file bytes mapped to shared and unshared FIEMAP extents after
   `fsync`/`syncfs`;
3. backing-device sectors written between stable counter readings;
4. preimage, merged postimage, upper, and lower SHA-256 oracles.

`copyup_bytes` means upper-file unshared FIEMAP bytes. It is not the same as
physical device traffic. `physical_io_bytes` means the delta of field 7 in the
explicit device `stat` file multiplied by 512. Journal and metadata writes are
therefore included.

The benchmark never formats a device. Filesystem creation and mounting of the
dedicated backing volume remain QEMU test-environment responsibilities.

## 2. Public interface

E3 deliberately follows E2's fixed-preset interface. Users run only:

    sudo python3 tools/deltafs/bench/e3/run.py \
      smoke BACKING_DIR DEVICE_STAT OUT_DIR

    sudo python3 tools/deltafs/bench/e3/run.py \
      run RUN_INDEX BACKING_DIR DEVICE_STAT OUT_DIR

    python3 tools/deltafs/bench/e3/analyze.py RESULTS_ROOT

`BACKING_DIR` is an empty dedicated directory on ext4 or XFS. `DEVICE_STAT` is
the explicit `/sys/dev/.../stat` file for the exact device containing
`BACKING_DIR`. `OUT_DIR` must not exist or must be empty and must be outside
`BACKING_DIR` on a different device, so result logging cannot enter the measured
`syncfs` traffic. For smoke, `RESULTS_ROOT` contains one successful output
directory per filesystem; full run contains five indexed shards per filesystem.
Analysis is written to `RESULTS_ROOT/analysis`.

For `run`, `RUN_INDEX` is one integer from 1 through 5. Each invocation owns one
independent-run shard, so the QEMU driver can interleave filesystem order without
running measured devices concurrently. There is no public manifest, reset hook,
event file, sample count, seed,
bootstrap count, cache mode, mount option, or filesystem label parameter.
Changing one of those fixed values requires an E3 schema revision. The runner
automatically:

- recognizes ext4/XFS and obtains XFS `reflink=0/1` from `xfs_info`;
- verifies that `DEVICE_STAT` resolves to `/sys/dev/block/MAJOR:MINOR/stat` for
  `BACKING_DIR`;
- generates and saves the immutable preset event JSONL;
- creates, mounts, verifies, unmounts, and removes a fresh sandbox per sample;
- captures manifest, raw data, FIEMAP dumps, stdout/stderr, dmesg, and failed
  sandboxes.

`copyup_bench` and event-generator modules are runner internals, not additional
user interfaces.

## 3. Code layout and ownership

    tools/deltafs/bench/e3/
      Makefile
      bench_common.h
      bench_common.c
      copyup_bench.c
      events.py
      run.py
      analyze.py
      tests/
        fiemap_fixture_test.c
        blockstat_test.c
        common_test.c
        test_events.py
        test_runner.py
        test_analyze.py

The C helper owns the measured edit sequence, SHA-256, FIEMAP collection, and
block-stat settling. Python standard-library code owns JSON, fixed event
generation, mount orchestration, artifact schemas, and analysis. E3 does not
import E2 code or artifacts.

## 4. Fixed synthetic events

### 4.1 Event schema

`OUT_DIR/events.jsonl` contains one canonical compact JSON object per line:

    {
      "schema": 1,
      "event_id": "e3-r01-warm-f004-d01-n000",
      "run": 1,
      "cache_mode": "warm",
      "relative_path": "edit.bin",
      "file_size_before": 4096,
      "offset": 0,
      "write_bytes": 4096,
      "payload_seed": 123456789,
      "expected_before_sha256": "...64 lowercase hex...",
      "expected_after_sha256": "...64 lowercase hex...",
      "size_bin": "4KiB",
      "dirty_blocks": 1
    }

Validation rejects unknown or missing fields, non-integer numeric fields,
schema mismatch, duplicate IDs, absolute or non-normal relative paths, `.` or
`..`, NUL, unknown cache mode/bin, non-4-KiB alignment, zero values, range
overflow, writes beyond the preimage, inconsistent size bins, and a hash that
does not match regenerated deterministic bytes.

The byte stream is defined as concatenated SHA-256 digests of:

    "deltafs-e3-v1\0" || little_endian_u64(seed) || little_endian_u64(counter)

The preimage seed is the low little-endian 64 bits of
`SHA256("deltafs-e3-preimage\0" || event_id)`. The fixed benchmark seed 14857
feeds a specified SplitMix64 generator for payload seeds, offsets, and
Fisher-Yates ordering. This avoids depending on Python's PRNG implementation.

Each runner independently generates the canonical event shard for its preset
and `RUN_INDEX`. The manifest records its SHA-256. Analysis requires
byte-identical event hashes across all three filesystems for each run index
before pairing by `event_id`.

### 4.2 Legal experiment cells

Files are 4, 12, 24, 48, 96, and 192 KiB. Dirty-block candidates are 1, 2, 4,
and 8 blocks, where one block is fixed at 4096 logical bytes. An edit never
extends the file, so only `dirty_blocks * 4096 <= file_size_before` is legal.
The resulting 18 cells are normative:

| file size | legal dirty blocks | size bin |
|---:|---|---|
| 4 KiB | 1 | `4KiB` |
| 12 KiB | 1, 2 | `8-16KiB` |
| 24 KiB | 1, 2, 4 | `16-32KiB` |
| 48 KiB | 1, 2, 4, 8 | `32-64KiB` |
| 96 KiB | 1, 2, 4, 8 | `64-128KiB` |
| 192 KiB | 1, 2, 4, 8 | `128-256KiB` |

The rough 6-by-4 matrix was internally inconsistent for small files: for
example an 8-block in-place write cannot fit in a 4-KiB file. E3 v1 treats the
18 legal cells, rather than 24 impossible combinations, as the completeness
gate.

### 4.3 Presets and ordering

| preset | warm events per cell/run | cold events per cell/run | independent runs |
|---|---:|---:|---:|
| `smoke` | 1 | 1 | 1 |
| `run` | 100 | 30 | 5 shards (`RUN_INDEX=1..5`) |

Warm and cold events have distinct IDs but identical fixed matrices. Within
each `(independent run, cache mode)`, cells are deterministically shuffled.
The same order is reproduced for every filesystem. A `run` invocation contains
2,340 edits (1,800 warm and 540 cold) and 351 no-op controls; all five shards
together contain 11,700 edits per filesystem.

Filesystem execution order is a fixed-seed Latin square, executed serially to
avoid sector-counter interference:

| run index | filesystem order |
|---:|---|
| 1 | `xfs_noreflink`, `xfs_reflink`, `ext4_noreflink` |
| 2 | `xfs_reflink`, `ext4_noreflink`, `xfs_noreflink` |
| 3 | `ext4_noreflink`, `xfs_noreflink`, `xfs_reflink` |
| 4 | same as run 1 |
| 5 | same as run 2 |

This order is derived by shuffling the three configurations with E3 SplitMix64
seed `14857 ^ 0xe3`, then rotating one place per run.

Cold sensitivity uses `posix_fadvise(..., POSIX_FADV_DONTNEED)` on the lower and
merged target after the preimage hash and before the first stable block-stat
reading. It does not write `/proc/sys/vm/drop_caches`. Warm samples retain the
cache populated by the preimage hash. Cold and warm rows are never pooled.

After each group of at most 20 edit samples the schedule runs three fresh
no-op controls. A control performs the same mount, stable-counter, `syncfs`, and
stable-counter lifecycle but no file edit. Its triplicate median is the fixed
batch baseline for sensitivity analysis. Raw physical I/O is always preserved;
the corrected value is `max(0, raw - no_op_median)`.

## 5. Fresh sample lifecycle

Each attempt exclusively uses:

    BACKING_DIR/.e3-work/<sample-id>/
      lower/edit.bin
      upper/
      work/
      merged/
      helper-result.json

Before an edit sample, the runner regenerates the exact preimage in lower,
checks its SHA-256, `fsync`s it, calls `syncfs` on the backing filesystem, and
verifies that upper has no `edit.bin`. It mounts OverlayFS with:

    lowerdir=lower,upperdir=upper,workdir=work,
    index=off,nfs_export=off,metacopy=off,xino=off,uuid=off,
    redirect_dir=nofollow

All sample paths must remain on the backing superblock. A successful sample is
unmounted and deleted. An invalid/failed sample is unmounted if possible and
preserved. Any remaining OverlayFS mount aborts the run.

The edit helper then performs this fixed sequence:

1. confirm lower exists, upper does not, merged and lower are regular files;
2. hash lower/merged and verify size and expected preimage;
3. apply cold-cache advice when requested;
4. wait for the explicit sectors-written counter to stabilize;
5. open merged, issue exactly one complete positional write, `fsync` the file,
   and `syncfs` the merged root;
6. wait for the counter to stabilize again and reject counter regression or
   multiplication overflow;
7. collect upper FIEMAP with `FIEMAP_FLAG_SYNC`;
8. hash merged, upper, and lower and apply all postimage oracles;
9. atomically write helper JSON and the separate FIEMAP JSON dump.

No-op controls execute steps 4--6 without opening or modifying an edit file.
Setup and unmount I/O occur outside the counter interval.

## 6. FIEMAP contract

The common helper exposes:

    struct bench_fiemap_summary {
            uint64_t mapped_bytes;
            uint64_t shared_bytes;
            uint64_t unshared_bytes;
            uint64_t hole_bytes;
            uint32_t block_size;
            uint32_t extent_count;
    };

FIEMAP is requested in bounded chunks and continued from the end of the last
extent until `FIEMAP_EXTENT_LAST`. Extents must be non-empty, monotonically
ordered, non-overlapping, non-overflowing, within the aligned test file, and
terminate with exactly one final `LAST`. E3 v1 understands only `LAST`,
`SHARED`, and `MERGED`; `UNKNOWN`, `DELALLOC`, `ENCODED`, encrypted, inline,
tail, unwritten, unaligned, or unknown future flags invalidate the sample.

Gaps count as holes. A `SHARED` extent contributes only to `shared_bytes`; any
other understood data extent contributes only to `unshared_bytes`.

    allocated_bytes_total = mapped_bytes
    copyup_bytes = unshared_bytes
    copyup_amplification = copyup_bytes / write_bytes

The FIEMAP dump records schema, file size, filesystem block size, and each raw
logical/physical/length/flags tuple. It is never embedded in aggregate TSV.

## 7. Block-stat contract

`bench_read_sectors_written(PATH, &value)` parses whitespace-separated unsigned
decimal fields and returns the seventh 1-based field. Fewer than seven fields,
a sign, non-decimal text, or `uint64_t` overflow is an error.

Stability starts with one reading, then sleeps 100 ms and reads again. Three
consecutive equal comparisons are required. Any change resets the consecutive
count. A 10-second deadline produces `settle_timeout=true` and invalidates the
sample. Interrupted sleeps resume. The device path is never inferred by C.

    physical_io_bytes = (sectors_after - sectors_before) * 512

The Python preflight resolves the supplied path and the backing `st_dev` sysfs
path to the same inode target. This prevents silently measuring another disk.

## 8. Artifacts and status

Each output directory contains:

    manifest.json
    events.jsonl
    raw.jsonl
    controls.jsonl
    summary.json
    stdout.log
    stderr.log
    dmesg-before.log
    dmesg-after.log
    fiemap/<sample-id>.json

The manifest records schema/preset/run-index/seed, event path/hash/count, git commit,
kernel release/config hash, filesystem type/config/UUID/source/options,
`xfs_info`, explicit and canonical device-stat paths, OverlayFS options, start
time, legal matrix, cache counts, independent runs, no-op interval/repetitions,
and settle constants.

Every edit raw row contains:

    schema, run, sample, sample_id, sample_kind, control_batch,
    cache_mode, fs_config, event_id, file_size_before, size_bin,
    offset, logical_bytes_changed, dirty_blocks, copyup_bytes,
    shared_bytes, allocated_bytes_total, copyup_amplification,
    sectors_before, sectors_after, physical_io_bytes, settle_timeout,
    pre_sha256, post_sha256, upper_sha256, lower_sha256,
    fiemap_path, fiemap_block_size, status, errno, invalid_reason

Control rows use a separate `controls.jsonl` schema with run, sample,
sample/control IDs, cache mode, batch, replica, sector values, physical bytes,
settle timeout, status, errno, and reason. Status is `ok`, `invalid`, or
`failed`. Only `status=ok` edit rows enter statistics. No row or failed FIEMAP
dump is deleted.

An edit is valid only when both settle operations and FIEMAP succeed, merged
preimage matches the event, merged/upper postimages match the event, upper and
merged match each other, lower remains unchanged, and physical counter math is
valid. A helper or oracle error stops the current independent run. Reset/mount
failure stops the entire benchmark and preserves the sandbox.

New dmesg `BUG`, `WARNING`, KASAN, KFENCE, UBSAN, lockdep, or RCU failure text
makes the run fail even if all sample rows are otherwise valid.

## 9. Analysis contract

`analyze.py RESULTS_ROOT` recursively discovers E3 manifests outside its own
analysis directory. Smoke requires one passed shard for each filesystem. Full
run requires exactly 15 passed shards covering each `(filesystem, run index)`
combination. Every shard must share preset/schema/seed, the three filesystems
must share an event hash at each run index, and edit/control schedules must be
complete.

It first performs an exact `event_id` join across all three configurations.
Missing/duplicate/status-invalid events are written to pairing/invalid reports
and make analysis fail. It is forbidden to subtract independent medians.

Fixed bootstrap count and seed are 10,000 and 14857. The independent unit is a
run shard: each replicate resamples run clusters with replacement and includes
the complete fixed event schedule from every selected run. Paired benefit
resamples paired event deltas with the same run clustering. This does not treat
the deterministic within-run synthetic schedule as a second independent random
sample. No values are winsorized and no outlier is silently removed.

Artifacts are:

    analysis/summary.tsv
    analysis/paired-benefit.tsv
    analysis/paired-benefit-summary.tsv
    analysis/regression.tsv
    analysis/noop-sensitivity.tsv
    analysis/pairing-errors.tsv
    analysis/invalid.jsonl
    analysis/copyup-by-size.png
    analysis/physical-io-by-size.png
    analysis/summary.json

For each filesystem, cache mode, size bin, and metric, `summary.tsv` reports n,
p25, p50, p75, p95, and the median CI95. Physical I/O is reported raw and
no-op-corrected. Paired outputs first compute two per-event mechanism deltas:
`xfs_metadata` is `ext4_noreflink - xfs_noreflink`; `reflink` is
`xfs_noreflink - xfs_reflink`. They report absolute bytes saved and source to
target ratio, then aggregate the paired deltas. A direct subtraction of
independent medians is forbidden.

For each filesystem/cache mode, ordinary least squares fits:

    log2(copyup_bytes) = alpha + beta * log2(file_size_before)

and reports beta with a run-cluster bootstrap CI. The two fixed PNG plots use a
logarithmic y axis and show p50 by size bin; the physical-I/O plot uses raw
values and the sensitivity TSV carries corrected values.

Passing requires all 18 planned cells at their preset counts, all three
filesystem configurations, exact event pairing, complete control triplets,
zero invalid/failed/hash/FIEMAP/block-stat/dmesg errors, and successful artifact
generation. Paper values and trends are references, not numeric pass gates.

## 10. Host-safe static verification

From the repository root, development verification is limited to:

    make -C tools/deltafs clean all
    make -C tools/deltafs e3-bench
    make -C tools/deltafs/bench/e3 check
    python3 -m unittest discover tools/deltafs/bench/e3/tests
    make C=2 CHECK=sparse M=fs/overlayfs

The E3 Makefile builds with `-Wall -Wextra -Werror`; `check` runs only C fixture
tests and Python unit tests that do not mount or access a real block device. A
separate GCC `-fanalyzer` compile is also used during implementation. This
environment must not load the module or mount OverlayFS, and no host result may
be presented as functional, copy-up, or physical-I/O verification.

2026-08-13 this worktree passed the complete static gate: all existing
userspace tools plus E2/E3 helpers compiled and linked with warnings as errors;
E2's C fixture and 14 Python tests passed; E3's three C fixtures and 19 Python
tests passed; Python byte compilation, GCC `-fanalyzer`, every
`tools/deltafs/*.sh` syntax check, `git diff --check`, and sparse for all
OverlayFS sources passed. No module was loaded and no OverlayFS mount or real
block-stat measurement was run here. Functional and physical-I/O claims remain
pending the QEMU procedure below.

## 11. QEMU/KVM functional handoff

All commands in this section are for the user in an environment that can boot
the current worktree kernel.

### 11.1 Build kernel and tools on the host

From the repository root:

    make -j"$(nproc)" bzImage modules
    make -C tools/deltafs clean all
    make -C tools/deltafs p7-tools e2-bench e3-bench

Expected files include:

    arch/x86/boot/bzImage
    fs/overlayfs/overlay.ko
    tools/deltafs/deltafsctl
    tools/deltafs/bench/e2/switch_once
    tools/deltafs/bench/e3/copyup_bench

### 11.2 Prepare dedicated images and boot

Use three disposable data images so filesystem results have isolated counters.
The following destroys only the explicitly named new image files:

    truncate -s 20G /absolute/path/e3-ext4.raw
    truncate -s 20G /absolute/path/e3-xfs-noreflink.raw
    truncate -s 20G /absolute/path/e3-xfs-reflink.raw
    mkfs.ext4 -F /absolute/path/e3-ext4.raw
    mkfs.xfs -f -m reflink=0 /absolute/path/e3-xfs-noreflink.raw
    mkfs.xfs -f -m reflink=1 /absolute/path/e3-xfs-reflink.raw

Boot with the worktree kernel and the three images:

    KERNEL=/absolute/path/to/arch/x86/boot/bzImage
    ROOTFS=/absolute/path/to/rootfs.qcow2
    REPO=/home/wangmingyu/repos/agentfs/fs/deltafs

    qemu-system-x86_64 \
      -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
      -kernel "$KERNEL" \
      -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
      -drive if=virtio,format=qcow2,file="$ROOTFS" \
      -drive if=virtio,format=raw,file=/absolute/path/e3-ext4.raw \
      -drive if=virtio,format=raw,file=/absolute/path/e3-xfs-noreflink.raw \
      -drive if=virtio,format=raw,file=/absolute/path/e3-xfs-reflink.raw \
      -virtfs local,path="$REPO",mount_tag=host,security_model=none

The examples below assume the guest names the data disks `/dev/vdb`,
`/dev/vdc`, and `/dev/vdd`; verify with `lsblk -f` before mounting.

### 11.3 Guest setup and P1--P7 acceptance gate

    mkdir -p /mnt/host /mnt/e3/{ext4,xfs-noreflink,xfs-reflink,results}
    mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
    mount /dev/vdb /mnt/e3/ext4
    mount /dev/vdc /mnt/e3/xfs-noreflink
    mount /dev/vdd /mnt/e3/xfs-reflink

    xfs_info /mnt/e3/xfs-noreflink | grep -w 'reflink=0'
    xfs_info /mnt/e3/xfs-reflink   | grep -w 'reflink=1'
    for M in /mnt/e3/ext4 /mnt/e3/xfs-noreflink /mnt/e3/xfs-reflink; do
      findmnt -no TARGET,SOURCE,FSTYPE "$M"
    done

    install -D -m 0644 /mnt/host/fs/overlayfs/overlay.ko \
      "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
    depmod -a
    modprobe -r overlay 2>/dev/null || true
    modprobe overlay
    cd /mnt/host
    make -C tools/deltafs p7-tools e2-bench e3-bench

Use two dedicated roots to run the existing final P1--P7 gate before E3:

    make -C tools/deltafs test-p6-controller
    tools/deltafs/p5_commit_test.sh \
      --backing-root /mnt/e3/xfs-reflink/p5
    tools/deltafs/p5_checkpoint_test.sh \
      --backing-root /mnt/e3/xfs-reflink/p5
    tools/deltafs/p6_controller_test.sh \
      --backing-root /mnt/e3/xfs-reflink/p6

    mkdir -p /mnt/e3/xfs-reflink/p7-main /mnt/e3/xfs-noreflink/p7-extra
    tools/deltafs/p7_acceptance_test.sh \
      --backing-root /mnt/e3/xfs-reflink/p7-main \
      --extra-backing-root /mnt/e3/xfs-noreflink/p7-extra

The P7 script prints and preserves its result directory. Remove only its empty
temporary backing directories after copying or retaining those results; do not
blindly `rmdir` the paths above.

Expected final line:

    All P7 DeltaFS v1 acceptance checks passed

There are no current standalone P1--P4 scripts; those names refer to historical
development phases. Expected output is controller unit PASS, both P5 suites
PASS, P6 suite PASS, then the P7 line above. The saved P7 result must show
sections 1--8 PASS and deep fault injection
`N >= 128`. Do not continue to E3 after a skipped or failed acceptance run.

### 11.4 E3 smoke

Create empty dedicated benchmark directories and keep results on the guest root
filesystem or another non-measured volume and device, not inside a measured
backing filesystem:

    mkdir /mnt/e3/ext4/e3-smoke /mnt/e3/xfs-noreflink/e3-smoke \
      /mnt/e3/xfs-reflink/e3-smoke
    mkdir -p /var/tmp/e3-results/smoke

    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/ext4/e3-smoke /sys/block/vdb/stat \
      /var/tmp/e3-results/smoke/ext4_noreflink
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/xfs-noreflink/e3-smoke /sys/block/vdc/stat \
      /var/tmp/e3-results/smoke/xfs_noreflink
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/xfs-reflink/e3-smoke /sys/block/vdd/stat \
      /var/tmp/e3-results/smoke/xfs_reflink
    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/smoke

Each runner must end with `PASS: E3 smoke ... invalid=0 failed=0`, and analysis
must end with `PASS: E3 analysis completed`. There are 36 edit events per
filesystem (18 warm and 18 cold), complete no-op triplets, one raw row and one
FIEMAP dump per event, and no pairing errors.

### 11.5 Full E3 run

Use new empty directories per filesystem/run shard; do not reuse smoke or prior
run directories. The following function maps each filesystem to its backing and
stat file, then the explicit schedule implements the fixed Latin square:

    mkdir -p /var/tmp/e3-results/run

    run_e3_shard() {
      RUN=$1 FS=$2
      case "$FS" in
        ext4_noreflink) BACKING=/mnt/e3/ext4 STAT=/sys/block/vdb/stat ;;
        xfs_noreflink) BACKING=/mnt/e3/xfs-noreflink STAT=/sys/block/vdc/stat ;;
        xfs_reflink)   BACKING=/mnt/e3/xfs-reflink STAT=/sys/block/vdd/stat ;;
        *) return 2 ;;
      esac
      WORK="$BACKING/e3-run-$RUN"
      OUT="/var/tmp/e3-results/run/$FS-run-$RUN"
      mkdir "$WORK"
      python3 tools/deltafs/bench/e3/run.py run "$RUN" "$WORK" "$STAT" "$OUT"
    }

    run_e3_shard 1 xfs_noreflink
    run_e3_shard 1 xfs_reflink
    run_e3_shard 1 ext4_noreflink
    run_e3_shard 2 xfs_reflink
    run_e3_shard 2 ext4_noreflink
    run_e3_shard 2 xfs_noreflink
    run_e3_shard 3 ext4_noreflink
    run_e3_shard 3 xfs_noreflink
    run_e3_shard 3 xfs_reflink
    run_e3_shard 4 xfs_noreflink
    run_e3_shard 4 xfs_reflink
    run_e3_shard 4 ext4_noreflink
    run_e3_shard 5 xfs_reflink
    run_e3_shard 5 ext4_noreflink
    run_e3_shard 5 xfs_noreflink

    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/run

Each shard must end with 2,340 valid edits and 351 valid controls. Each
filesystem totals 9,000 warm and 2,700 cold valid edits: 18 cells times 100/30
samples times 5 independent runs. Analysis must find all 15 shards, all six
bins, 18 complete cells, zero invalid/failed rows, complete paired IDs, and the
fixed PNG/TSV artifacts. Keep unrelated services off the measured devices.

### 11.6 Failure collection

On any failure, do not delete `.e3-work`, result JSONL, or FIEMAP dumps. Replace
`R` and `DEV` with the failing output and measured device:

    R=/var/tmp/e3-results/run/xfs_reflink
    DEV=/sys/block/vdd/stat
    dmesg -T > "$R/dmesg-failure-full.log"
    findmnt -J > "$R/findmnt-failure.json"
    cat /proc/mounts > "$R/proc-mounts-failure.txt"
    cat "$DEV" > "$R/device-stat-failure.txt"
    cat /proc/config.gz > "$R/kernel-config.gz" 2>/dev/null || \
      cp "/boot/config-$(uname -r)" "$R/kernel-config"
    uname -a > "$R/uname.txt"
    lsblk -o NAME,MAJ:MIN,FSTYPE,SIZE,MOUNTPOINTS > "$R/lsblk.txt"
    cp -a "$R" /mnt/host/

Also copy the printed preserved sandbox from its measured backing directory.
A reset/mount error, hash mismatch, unsupported FIEMAP flag, counter parse or
settle error, missing pair, incomplete no-op triplet, or new kernel diagnostic
is a failed run; it must not be hidden by removing the corresponding row.
