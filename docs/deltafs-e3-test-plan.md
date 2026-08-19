# DeltaFS E3 cache-neutral copy-up benchmark detailed design

> E3 measures one copy-up edit after a native DeltaFS v2 checkpoint. It is
> independent of the E2 ioctl-latency benchmark and does not use `deltafsctl`.
> Its synthetic-event schema is version 3, independent
> of the DeltaFS kernel ABI version recorded in the manifest. Every sample must
> prove a generation 1 to generation 2 checkpoint with the current v2 request;
> stock OverlayFS and the removed v1 ABI are rejected before measurement.
>
> Without the authors' SWE-Search event corpus, the fixed synthetic preset is a
> method reproduction. Its output must not be described as an exact Fig. 9
> artifact reproduction.

## 1. Questions, scope, and terminology

E3 compares three backing filesystem configurations:

- `ext4_noreflink`;
- `xfs_noreflink` (`mkfs.xfs -m reflink=0`);
- `xfs_reflink` (`mkfs.xfs -m reflink=1`).

E3 contains two fixed experiments. `copyup_matrix` retains the original
file-size/dirty-range matrix. `path_depth` isolates the cost of copying parent
directories into the fresh upper after checkpoint. For each immutable edit
event it measures:

1. logical bytes changed by one aligned `pwrite`;
2. upper-file bytes mapped to shared and unshared FIEMAP extents after
   `fsync`/`syncfs`;
3. backing-device sectors written between stable counter readings;
4. preimage, merged postimage, upper, and lower SHA-256 oracles.
5. the number of parent directories materialized in generation-2 upper.

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
      run BACKING_DIR DEVICE_STAT OUT_DIR

    sudo python3 tools/deltafs/bench/e3/run.py \
      depth-smoke BACKING_DIR DEVICE_STAT OUT_DIR

    sudo python3 tools/deltafs/bench/e3/run.py \
      depth-run BACKING_DIR DEVICE_STAT OUT_DIR

    python3 tools/deltafs/bench/e3/analyze.py RESULTS_ROOT

`BACKING_DIR` is an empty dedicated directory on ext4 or XFS and may be a
subdirectory below the filesystem mount point. `DEVICE_STAT` is
the explicit `/sys/dev/.../stat` file for the exact device containing
`BACKING_DIR`. `OUT_DIR` must not exist or must be empty and must be outside
`BACKING_DIR` on a different device, so result logging cannot enter the measured
`syncfs` traffic. For any one preset, `RESULTS_ROOT` contains one successful
output directory per filesystem. Results from different presets must not share
one root. Analysis is written to
`RESULTS_ROOT/analysis`.

Each `run` or `depth-run` invocation owns one backing test object and executes
all five independent workloads serially. There is no public run-index shard parameter,
manifest, reset hook, event file, sample count, seed, bootstrap count, cache
mode, mount option, or filesystem label parameter.
Changing the event schedule or measured settings requires an event-schema
revision; the DeltaFS lifecycle is identified independently by mandatory
manifest ABI/generation fields. The runner automatically:

- recognizes ext4/XFS and obtains XFS `reflink=0/1` by passing the `findmnt`
  mount target, rather than `BACKING_DIR`, to `xfs_info`;
- verifies that `DEVICE_STAT` resolves to `/sys/dev/block/MAJOR:MINOR/stat` for
  `BACKING_DIR`;
- generates and saves the immutable preset event JSONL;
- creates and mounts a fresh generation-1 sandbox per sample;
- freezes the generation-1 upper by rename and issues one native v2 checkpoint
  to a fresh generation-2 upper/work pair before starting the counter interval;
- verifies, unmounts, and removes the sandbox after measurement;
- captures manifest, raw data, FIEMAP dumps, stdout/stderr, dmesg, and failed
  sandboxes.

`checkpoint_v2`, `copyup_bench`, and event-generator modules are runner
internals, not additional user interfaces.

## 3. Code layout and ownership

    tools/deltafs/bench/e3/
      Makefile
      bench_common.h
      bench_common.c
      deltafs_v2_common.h
      deltafs_v2_common.c
      checkpoint_v2.c
      copyup_bench.c
      events.py
      run.py
      analyze.py
      tests/
        fiemap_fixture_test.c
        blockstat_test.c
        common_test.c
        v2_request_test.c
        test_events.py
        test_runner.py
        test_analyze.py

`checkpoint_v2` owns the native v2 request construction and ioctl. The
`copyup_bench` C helper owns the measured edit sequence, SHA-256, FIEMAP
collection, and block-stat settling. Python standard-library code owns JSON,
fixed event generation, mount/checkpoint orchestration, artifact schemas, and
analysis. E3 does not import E2 code or artifacts.

## 4. Fixed synthetic events

### 4.1 Event schema

`OUT_DIR/events.jsonl` contains one canonical compact schema-3 JSON object per
line:

    {
      "schema": 3,
      "experiment": "copyup_matrix",
      "event_id": "e3-w01-f004-d01-n000",
      "case_id": "e3-w01-f004-d01-n000",
      "workload": 1,
      "relative_path": "edit.bin",
      "directory_depth": 0,
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
schema/experiment mismatch, duplicate event IDs, inconsistent case IDs,
absolute or non-normal relative paths, `.` or `..`, NUL, a path whose parent
count differs from `directory_depth`, unknown size bin, non-4-KiB alignment, zero values, range
overflow, writes beyond the preimage, inconsistent size bins, and a hash that
does not match regenerated deterministic bytes.

The byte stream is defined as concatenated SHA-256 digests of:

    "deltafs-e3-v3\0" || little_endian_u64(seed) || little_endian_u64(counter)

The preimage seed is the low little-endian 64 bits of
`SHA256("deltafs-e3-preimage\0" || case_id)`. Depth variants share one
`case_id`, preimage, payload, and offset; only `event_id`, `relative_path`, and
`directory_depth` differ. The fixed benchmark seed 14857
feeds a specified SplitMix64 generator for payload seeds, offsets, and
Fisher-Yates ordering. This avoids depending on Python's PRNG implementation.

Each runner independently generates the complete canonical event set for its
preset. The manifest records its SHA-256. Analysis requires byte-identical event
hashes across all three filesystems before pairing by `event_id`.

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
example an 8-block in-place write cannot fit in a 4-KiB file. E3 v2 treats the
18 legal cells, rather than 24 impossible combinations, as the completeness
gate.

### 4.3 Path-depth cells

The `path_depth` experiment fixes the write to one aligned 4-KiB block and
crosses two file sizes, 4 KiB and 192 KiB, with these directory depths:

    0, 1, 2, 4, 8, 16

Depth is the number of parent directories between the merged root and
`edit.bin`; the merged root itself is not counted. Paths are canonical prefixes:

    depth 0:  edit.bin
    depth 1:  dir-01/edit.bin
    depth 2:  dir-01/dir-02/edit.bin
    depth 16: dir-01/.../dir-16/edit.bin

Every parent and the file initially exist only in generation-1 upper, which is
then frozen as `layers/g1`. Base and generation-2 upper are empty. Therefore an
edit can succeed only after OverlayFS materializes the full parent chain in the
fresh upper. This produces 12 fixed depth cells. The 4-KiB file exposes a
metadata-dominated case; the 192-KiB file retains the large-file/small-edit
copy-up case.

### 4.4 Presets and ordering

| preset | events per cell/workload | independent workloads |
|---|---:|---:|
| `smoke` | 2 | 1 |
| `run` | 130 | 5 |
| `depth-smoke` | 2 | 1 |
| `depth-run` | 130 | 5 |

Each workload has a distinct number and the same experiment-specific fixed
matrix. Main-matrix cells are deterministically shuffled. For `path_depth`, a
matched case block contains all six depth variants; case blocks and their
internal depth order are deterministically shuffled. The same order is
reproduced for every filesystem. A `run` invocation contains 11,700 edits and
1,755 controls per filesystem. A `depth-run` invocation contains 7,800 edits
and 1,170 controls per filesystem. Smoke counts are respectively 36 and 24
edits; both have 6 controls.

Filesystem configurations are executed serially so their device-sector counters
cannot interfere. The five workloads for one filesystem stay in one runner
invocation and one artifact directory.

Cache state is not an E3 variable. The helper does not issue
`POSIX_FADV_DONTNEED` or write `/proc/sys/vm/drop_caches`; every event follows
the same cache-neutral protocol.

After each group of at most 20 edit samples the schedule runs three fresh
no-op controls. A control uses the first event in its batch to create the same
frozen file and parent tree, then performs the same mount, rename, v2 checkpoint,
pre-window `syncfs`, stable-counter, measured no-op `syncfs`, and stable-counter
lifecycle but no file edit. Generation-2 upper must remain empty. Its
triplicate median is the fixed batch baseline for sensitivity analysis. Raw
physical I/O is always preserved; the corrected value is
`max(0, raw - no_op_median)`.

## 5. Fresh v2 sample lifecycle

Each attempt exclusively uses:

    BACKING_DIR/.e3-work/<sample-id>/
      base/
      generation-1/upper/<relative-path>
      generation-1/work/
      generation-2/upper/
      generation-2/work/
      layers/g1/                 # generation-1 upper after rename
      merged/
      helper-result.json

Before an edit sample, the runner creates the event's canonical parent chain,
regenerates the exact preimage at `generation-1/upper/<relative-path>`, fixes
directory modes/timestamps, checks its SHA-256, `fsync`s it, calls `syncfs` on
the backing filesystem, and verifies that generation-2 upper is empty.
It mounts generation 1 with:

    lowerdir=base,upperdir=generation-1/upper,workdir=generation-1/work,
    index=off,nfs_export=off,metacopy=off,xino=off,uuid=off,
    redirect_dir=nofollow

While the mount is live, the runner renames `generation-1/upper` to `layers/g1`
and invokes `checkpoint_v2` with expected generation 1 and the fresh
`generation-2/upper` and `generation-2/work` paths. Success establishes this
stack before measurement:

    [generation-2/upper (RW), layers/g1 (RO), base (RO)]

All sample paths must remain on the backing superblock. The v2 checkpoint is
outside the block-stat counter interval. A failed ioctl, including `ENOTTY`
from stock OverlayFS or a v1-only module, fails the sample and preserves its
sandbox. A successful sample is unmounted and deleted. An invalid/failed sample
is unmounted if possible and preserved. Any remaining OverlayFS mount aborts
the run.

The edit helper then performs this fixed sequence:

1. confirm frozen `layers/g1/<relative-path>` exists, generation-2 upper does not, and
   merged/frozen paths are regular files;
2. hash frozen/merged and verify size and expected preimage;
3. call `syncfs` on the merged root to drain checkpoint and precheck writes;
4. wait for the explicit sectors-written counter to stabilize and record
   `sectors_before`;
5. open merged, issue exactly one complete positional write, `fsync` the file,
   and `syncfs` the merged root;
6. wait for the counter to stabilize again and reject counter regression or
   multiplication overflow;
7. collect upper FIEMAP with `FIEMAP_FLAG_SYNC`;
8. hash merged, generation-2 upper, and frozen generation-1 upper and apply all
   postimage oracles;
9. atomically write helper JSON and the separate FIEMAP JSON dump;
10. verify generation-2 upper contains exactly the target file and its declared
    parent chain, and record `copied_up_parent_dirs=directory_depth`.

No-op controls execute the same event-shaped generation-1 mount, rename, v2 checkpoint, and
pre-window `syncfs`, then steps 4--6 without opening or modifying an edit file.
They verify the merged/frozen preimage is unchanged and generation-2 upper has
no entries. Setup, checkpoint, precheck, and unmount I/O occur outside the counter interval.

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

The manifest records schema/preset/experiment/seed, event path/hash/count, git
commit, kernel release/config hash, filesystem type/config/UUID/source/options,
`xfs_info`, explicit and canonical device-stat paths, OverlayFS options,
`deltafs_abi_version=2`, initial/checkpoint generations `1` and `2`, copy-up
source `checkpoint_frozen_upper`, start time, experiment cells and directory
depths, samples per cell,
independent workload count, no-op interval/repetitions, and settle constants. Analysis
requires these exact v2 lifecycle values, so it cannot mix legacy plain-
OverlayFS E3 artifacts with current results.

Every edit raw row contains:

    schema, experiment, workload, sample, sample_id, sample_kind, control_batch,
    fs_config, event_id, case_id, relative_path, directory_depth,
    file_size_before, size_bin, offset, logical_bytes_changed, dirty_blocks, copyup_bytes,
    shared_bytes, allocated_bytes_total, copyup_amplification,
    copied_up_parent_dirs,
    sectors_before, sectors_after, physical_io_bytes, settle_timeout,
    pre_sha256, post_sha256, upper_sha256, lower_sha256,
    fiemap_path, fiemap_block_size, status, errno, invalid_reason

Control rows use a separate `controls.jsonl` schema with experiment, workload,
sample, sample/control IDs, template event/case/path/depth, batch, replica,
sector values, physical bytes,
settle timeout, status, errno, and reason. Status is `ok`, `invalid`, or
`failed`. Only `status=ok` edit rows enter statistics. No row or failed FIEMAP
dump is deleted.

An edit is valid only when both settle operations and FIEMAP succeed, merged
preimage matches the event, merged/upper postimages match the event, upper and
merged match each other, lower remains unchanged, and physical counter math is
valid. A helper or oracle error stops the current independent workload. Reset/mount
failure stops the entire benchmark and preserves the sandbox.

New dmesg `BUG`, `WARNING`, KASAN, KFENCE, UBSAN, lockdep, or RCU failure text
makes the run fail even if all sample rows are otherwise valid.

## 9. Analysis contract

`analyze.py RESULTS_ROOT` recursively discovers E3 manifests outside its own
analysis directory. Every preset requires exactly one passed result
set for each filesystem. Every result set must share preset/schema/seed and the
three filesystems must share one event hash and complete edit/control schedules.

It first performs an exact `event_id` join across all three configurations.
Missing/duplicate/status-invalid events are written to pairing/invalid reports
and make analysis fail. It is forbidden to subtract independent medians.

Fixed bootstrap count and seed are 10,000 and 14857. The independent unit is a
workload: each replicate resamples workload clusters with replacement and includes
the complete fixed event schedule from every selected workload. Paired benefit
resamples paired event deltas with the same workload clustering. This does not treat
the deterministic within-workload synthetic schedule as a second independent random
sample. No values are winsorized and no outlier is silently removed.

For `copyup_matrix`, artifacts are:

    analysis/summary.tsv
    analysis/paired-benefit.tsv
    analysis/paired-benefit-summary.tsv
    analysis/regression.tsv
    analysis/noop-sensitivity.tsv
    analysis/amplification-summary.tsv
    analysis/pairing-errors.tsv
    analysis/invalid.jsonl
    analysis/copyup-amplification-by-write-size.png
    analysis/physical-write-amplification-by-write-size.png
    analysis/summary.json

For each filesystem, size bin, and metric, `summary.tsv` reports n,
p25, p50, p75, p95, and the median CI95. Physical I/O is reported raw and
no-op-corrected. Paired outputs first compute two per-event mechanism deltas:
`xfs_metadata` is `ext4_noreflink - xfs_noreflink`; `reflink` is
`xfs_noreflink - xfs_reflink`. They report absolute bytes saved and source to
target ratio, then aggregate the paired deltas. A direct subtraction of
independent medians is forbidden.

For each filesystem, ordinary least squares fits:

    log2(copyup_bytes) = alpha + beta * log2(file_size_before)

and reports beta with a workload-cluster bootstrap CI. The two fixed PNG plots show
write amplification rather than absolute bytes. Their x axis is the aligned
logical write-request size (4, 8, 16, or 32 KiB) and their logarithmic y axis
is respectively `copyup_bytes / logical_bytes_changed` and
`physical_io_bytes / logical_bytes_changed`. Each figure has one panel per
filesystem configuration and one series per pre-edit file size, so file size
remains visible without being used as the primary x axis. Illegal cells are
absent and are not connected across missing request sizes.

Cache state is absent from the event and artifact schemas because it is not a
write-amplification dimension. Points show per-cell medians without
confidence-interval whiskers. The physical-write plot uses raw device writes,
while the sensitivity TSV carries no-op-corrected bytes.
`amplification-summary.tsv` records the cache-neutral cell count and plotted
median for both amplification metrics.

For `path_depth`, analysis instead emits:

    analysis/depth-summary.tsv
    analysis/depth-paired.tsv
    analysis/depth-paired-summary.tsv
    analysis/depth-slope.tsv
    analysis/noop-sensitivity.tsv
    analysis/pairing-errors.tsv
    analysis/invalid.jsonl
    analysis/path-depth-physical-write-amplification.png
    analysis/path-depth-copyup-amplification.png
    analysis/summary.json

`depth-summary.tsv` groups raw and corrected physical bytes plus copy-up bytes
by filesystem, file size, and directory depth. `depth-paired.tsv` joins all six
depth variants of each `case_id` within one filesystem and reports the delta
and ratio against depth 0. `depth-slope.tsv` reports the median within-case OLS
slope in bytes per additional parent directory with workload-cluster bootstrap
CI95. The two depth plots use directory depth on the x axis and one series per
file size. The corrected physical-write plot is the primary result;
copy-up amplification is a negative control because file FIEMAP does not
include parent-directory metadata. `copyup_depth_invariant` reports whether
the median depth-16 minus depth-0 copy-up byte delta is zero in all six
filesystem/file-size groups. Analysis summary records, separately from
artifact validity, whether the paired depth-16 corrected physical-I/O CI is
strictly above zero for every filesystem/file-size group.

Passing requires every experiment-specific planned cell at its preset count, all three
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

The host gate covers v2 userspace tools and E3. E2 also uses the v2 ABI, but its
latency benchmark is independent and is not an E3 prerequisite. No module is
loaded and no OverlayFS mount or real block-stat measurement is run by this
gate; functional and physical-I/O claims remain subject to the QEMU procedure
below.

## 11. QEMU/KVM functional handoff

All commands in this section are for the user in an environment that can boot
the current worktree kernel.

### 11.1 Build kernel and tools on the host

From the repository root:

    make -j"$(nproc)" bzImage modules
    make -C tools/deltafs clean v2-tools e3-bench
    make -C tools/deltafs check-v2-layout test-v2-controller
    make -C tools/deltafs check-v2-checkpoints CHECKPOINT_MODE=auto

Expected files include:

    arch/x86/boot/bzImage
    fs/overlayfs/overlay.ko
    tools/deltafs/deltafsctl
    tools/deltafs/bench/e3/checkpoint_v2
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

### 11.3 Guest setup and v2 acceptance gate

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
    make -C tools/deltafs v2-tools e3-bench

Use two dedicated roots on different filesystems to run the v2 gate before E3:

    make -C tools/deltafs test-v2-controller
    mkdir -p /mnt/e3/xfs-reflink/v2-main /mnt/e3/xfs-noreflink/v2-extra
    tools/deltafs/deltafs_v2_acceptance_test.sh \
      --backing-root /mnt/e3/xfs-reflink/v2-main \
      --extra-backing-root /mnt/e3/xfs-noreflink/v2-extra

Expected final line:

    All DeltaFS v2 acceptance checks passed

The acceptance harness preserves its result directory under `v2-main`. A debug
guest must report the target-depth, `keep_bottom`, fault-injection, teardown,
sanitizer, and kmemleak checks as `PASS`. Exit code 4 means capability-dependent
checks were skipped and is not a complete acceptance result. Do not continue to
E3 after a skipped or failed v2 acceptance run. The acceptance cleanup unloads
the module, so reload the installed worktree module before E3:

    modprobe overlay

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

Each runner must log a successful native `checkpoint: generation 1 -> 2` for
every edit and control, end with `PASS: E3 smoke ... invalid=0 failed=0`, and
analysis must end with `PASS: E3 analysis completed`. There are 36 edit events
per filesystem in smoke (18 cells with two samples), complete no-op triplets,
one raw row and one FIEMAP dump per event, and no pairing errors. Every manifest
must contain
`"deltafs_abi_version": 2`, `"initial_generation": 1`,
`"checkpoint_generation": 2`, and
`"copyup_source": "checkpoint_frozen_upper"`.

The exact successful runner counts are `ok=36 invalid=0 failed=0 controls=6`.

### 11.5 Full E3 run

Use one new empty directory per filesystem; do not reuse smoke or prior run
directories. The following function maps each filesystem to its backing and stat
file:

    mkdir -p /var/tmp/e3-results/run

    run_e3() {
      FS=$1
      case "$FS" in
        ext4_noreflink) BACKING=/mnt/e3/ext4 STAT=/sys/block/vdb/stat ;;
        xfs_noreflink) BACKING=/mnt/e3/xfs-noreflink STAT=/sys/block/vdc/stat ;;
        xfs_reflink)   BACKING=/mnt/e3/xfs-reflink STAT=/sys/block/vdd/stat ;;
        *) return 2 ;;
      esac
      WORK="$BACKING/e3-run"
      OUT="/var/tmp/e3-results/run/$FS"
      mkdir "$WORK"
      python3 tools/deltafs/bench/e3/run.py run "$WORK" "$STAT" "$OUT"
    }

    run_e3 xfs_noreflink
    run_e3 xfs_reflink
    run_e3 ext4_noreflink

    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/run

Each filesystem run must end with 11,700 valid edits and 1,755 valid controls:
18 cells times 130 samples times 5 independent workloads. Analysis must find
all three filesystem outputs, all six bins, 18 complete cells, zero
invalid/failed rows, complete paired IDs, and the fixed PNG/TSV artifacts. Keep
unrelated services off the measured devices.

The exact successful runner counts are
`ok=11700 invalid=0 failed=0 controls=1755`.

### 11.6 Path-depth smoke and full run

Use new empty backing directories and separate result roots:

    mkdir -p /var/tmp/e3-results/depth-smoke /var/tmp/e3-results/depth-run

    run_depth_e3() {
      PRESET=$1 FS=$2 ROOT=$3
      case "$FS" in
        ext4_noreflink) BACKING=/mnt/e3/ext4 STAT=/sys/block/vdb/stat ;;
        xfs_noreflink) BACKING=/mnt/e3/xfs-noreflink STAT=/sys/block/vdc/stat ;;
        xfs_reflink)   BACKING=/mnt/e3/xfs-reflink STAT=/sys/block/vdd/stat ;;
        *) return 2 ;;
      esac
      WORK="$BACKING/e3-$PRESET"
      mkdir "$WORK"
      python3 tools/deltafs/bench/e3/run.py "$PRESET" "$WORK" "$STAT" "$ROOT/$FS"
    }

    for FS in ext4_noreflink xfs_noreflink xfs_reflink; do
      run_depth_e3 depth-smoke "$FS" /var/tmp/e3-results/depth-smoke
    done
    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/depth-smoke

Expected per filesystem: `ok=24 invalid=0 failed=0 controls=6`. Every raw row
must have `copied_up_parent_dirs == directory_depth`; each case must have all
six depths, and analysis must generate both path-depth PNGs plus the four depth
TSVs without pairing errors.

After smoke passes, reboot or use new disposable filesystems/directories, then:

    for FS in ext4_noreflink xfs_noreflink xfs_reflink; do
      run_depth_e3 depth-run "$FS" /var/tmp/e3-results/depth-run
    done
    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/depth-run

Expected per filesystem: `ok=7800 invalid=0 failed=0 controls=1170`. Inspect
`analysis/summary.json` for `depth_hypothesis_supported`; a false value does not
invalidate artifact correctness, but means the run did not demonstrate the
pre-registered positive depth effect and must be reported as such.

### 11.7 Failure collection

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
A reset/mount/rename/v2-checkpoint error, hash mismatch, unsupported FIEMAP
flag, counter parse or settle error, missing pair, incomplete no-op triplet, or
new kernel diagnostic is a failed run; it must not be hidden by removing the
corresponding row. Include the preserved sample tree and `stderr.log`; an
`Inappropriate ioctl for device` checkpoint error usually means the guest
loaded stock OverlayFS or a non-v2 module.
