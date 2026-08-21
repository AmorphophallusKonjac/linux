# DeltaFS E3 temporal write-latency benchmark design

> E3 is a standalone benchmark for the time cost of copy-up and generation
> transitions during a continuous edit workload. It does not extend E2's
> space/physical-I/O experiment and does not replace E1's ioctl-only latency
> benchmark.

## 1. Scope

E3 answers five questions:

1. How much slower is the first write to a frozen-layer file than an
   equivalent write to a file already resident in the active upper?
2. Does that latency change as the edit sequence advances through more
   checkpoint generations?
3. How much of the cost is paid once per file (copy-up) and how much is paid by
   subsequent writes in the same generation?
4. How does a short-lived editor access behave across generations?
5. What latency overhead does the DeltaFS/OverlayFS path add relative to
   writing the same file directly on the lower backing filesystem?

The dependent variable is wall-clock latency, not duplicated bytes:

    latency_amplification = temporal_edit_latency / upper_resident_baseline

E3 also records a direct lower-filesystem control. `lower_direct` replays the
same event against a private copy of the sequence's `base/` tree without
mounting OverlayFS or performing a checkpoint. It estimates the overhead of
the DeltaFS/OverlayFS path separately from the copy-up effect.

The result must be described as **write-latency amplification**. The unqualified
term `write amplification` remains reserved for E2 byte/device-I/O ratios.

E3 compares the same four backing configurations as E2:

- `ext4`;
- `xfs_noreflink` (`mkfs.xfs -m reflink=0`);
- `xfs_reflink` (`mkfs.xfs -m reflink=1`).
- `f2fs` (`mkfs.f2fs`), which has no reflink mode.

The benchmark never formats a device and never loads the kernel module on the
host. Filesystem creation, module loading, and functional execution happen in
the user's QEMU guest.

## 2. Explicit boundaries

E1 measures only the interval around one native checkpoint/restore ioctl:

    clock_gettime(CLOCK_MONOTONIC_RAW)
    ioctl(...)
    clock_gettime(CLOCK_MONOTONIC_RAW)

E3 does not include that ioctl in its primary write metric. E3 uses checkpoint
only to establish the next generation and records any setup time separately.
The E3 primary interval is the user-visible edit operation:

    openat (reopen mode only)
    pwrite/pwritev of the fixed payload
    fsync(file)
    close (reopen mode only)

`syncfs`, result serialization, hashing, directory scans, and checkpoint setup
are outside `edit_e2e_ns`. A secondary `pwrite_only_ns` and `fsync_ns` are
recorded to identify whether an effect comes from data-path entry or durability.

E2 remains the authority for FIEMAP copy-up bytes, shared extents, and block
device sectors. E3 may record those values only as optional diagnostic context;
they are not used in E3 latency statistics.

## 3. Hypotheses and interpretation

The following are pre-registered hypotheses, not pass/fail gates:

- `first_touch` is slower than `upper_resident` because it performs generation
  revalidation and copy-up.
- Within a burst, the first write pays most of the copy-up cost and later writes
  to the same active-upper file have lower latency.
- Increasing historical layer depth may increase lookup/revalidation latency,
  but the slope is an observed result and must not be assumed to be O(1) or O(D)
  without data.
- Reflink can reduce E2's copied bytes without guaranteeing the same reduction
  in E3 latency; metadata, locks, and cache effects are reported separately.
- A held file descriptor may exercise lazy generation refresh and can differ
  from a close/reopen editor workload.
- `upper_resident` isolates the DeltaFS/OverlayFS path overhead relative to
  `lower_direct`; `first_touch` additionally includes copy-up and generation
  refresh costs.

An unsupported or negative hypothesis does not invalidate a technically valid
run. Missing samples, failed oracles, CPU migration, clock faults, or kernel
diagnostics do invalidate the run.

## 4. Workload model

The public interface is:

    sudo python3 tools/deltafs/bench/e3/run.py PRESET BACKING_DIR OUT_DIR
    python3 tools/deltafs/bench/e3/analyze.py RESULTS_ROOT

`OUT_DIR` must be outside `BACKING_DIR` on a different device. Because E3 does
not read block-sector counters, it has no `DEVICE_STAT` argument.

### 4.1 Fixed workload families

Every sequence uses deterministic 4-KiB aligned payloads and a fixed seed. A
sequence is the independent statistical unit; rows from one sequence are not
treated as independent observations.

| family | files | writes after each checkpoint | purpose |
|---|---:|---:|---|
| `single` | 1 | 1 | repeated first-touch cost over generations |
| `burst4` | 1 | 4 | first-touch versus same-generation steady writes |
| `multi16` | 16 | 16, one per file | working-set copy-up fan-out |

For `single` and `burst4`, the target is a 256-KiB file. For `multi16`, each
file is 64 KiB and files are placed below a deterministic two-level directory
tree. Payload offsets and file selection are generated by the E3 event schema,
not by the host PRNG.

### 4.2 Access mode

The E3 presets use `reopen` mode, which matches a short-lived editor: open the
path after the checkpoint, write, fsync, and close. Cross-generation ordinary
file descriptors are outside the current DeltaFS support boundary and are not
part of E3.

### 4.3 Temporal and history matrices

The primary temporal sweep starts from one frozen lower and runs a continuous
sequence for these measured generation counts:

    1, 2, 4, 8, 16, 32

The separate history-depth sweep creates a disposable frozen chain of:

    1, 4, 16, 64, 128 lower generations

and then executes exactly one measured generation. The 128-lower case is the
v2 maximum and is a required boundary sample. No checkpoint is performed after
that measured operation, so the run never attempts to create a 129th lower.
History construction is outside the edit timing interval.

The two sweeps are intentionally not a Cartesian product. A temporal sequence
starts at history depth 1 and may checkpoint repeatedly; a history-depth sample
uses one measured generation at a prebuilt depth. This is required by the v2
128-lower limit and keeps the independent variables interpretable.

Presets are fixed and have no user-supplied dimensions:

| preset | sequences per cell | independent runs | access mode |
|---|---:|---:|---|
| `smoke` | 2 | 1 | temporal sweep, reopen |
| `run` | 20 | 5 | temporal sweep, reopen |
| `depth-smoke` | 2 | 1 | history-depth sweep, reopen |
| `depth-run` | 20 | 5 | history-depth sweep, reopen |

Each independent run regenerates the same canonical event schedule. Results
from different presets must use different result roots.

The schedule contains 36 temporal cells for `run`/`smoke` (three families
times six generation counts), and 15 history-depth cells for
`depth-run`/`depth-smoke` (three families times five depths). Thus `run` has
1,800 independent sequence instances per filesystem (`36 * 20 * 5`) and
`depth-run` has 1,500. These are sequence counts, not the number of timed rows:
`burst4` and `multi16` contribute multiple edit rows per generation.
With all three variants, the expected timed-row counts per filesystem are
7,938 for `smoke`, 396,900 for `run`, 630 for `depth-smoke`, and 31,500 for
`depth-run`.

## 5. Baselines and paired metrics

Every measured sequence has matched sibling control sequences on the same
backing filesystem and CPU pinning policy:

- `upper_resident`: equivalent file is created in the active upper before each
  timed write; no copy-up is needed, but the checkpoint schedule is otherwise
  identical;
- `first_touch`: equivalent file exists only in the frozen lower immediately
  after checkpoint;
- `steady_after_copyup`: in `burst4`, the second through fourth writes to the
  same file in the same generation.
- `lower_direct`: the same write issued directly to the backing filesystem's
  `base/` tree. This control has no generation transition and is not pooled
  with the OverlayFS controls.

The payload, offset, file size, access mode, generation, and history depth are
identical between paired controls. Ratios are computed per sequence before
aggregation:

    first_touch_ratio = first_touch_edit_e2e_ns / upper_resident_edit_e2e_ns
    steady_ratio      = steady_after_copyup_edit_e2e_ns / upper_resident_edit_e2e_ns
    copyup_overhead_ns = first_touch_edit_e2e_ns - upper_resident_edit_e2e_ns

For each OverlayFS sample, E3 also reports the paired direct-filesystem
overhead:

    deltafs_ratio = overlay_edit_e2e_ns / lower_direct_edit_e2e_ns
    deltafs_overhead_ns = overlay_edit_e2e_ns - lower_direct_edit_e2e_ns

These values are computed per event before aggregation. `lower_direct` uses the
same deterministic preimage, payload, offset, file size, access mode, CPU
pinning, and cache policy. It is a path baseline, not a copy-up baseline:
`first_touch_ratio` and `steady_ratio` remain normalized by `upper_resident`.

For a complete temporal sequence, also report:

    cumulative_edit_ns = sum(edit_e2e_ns over measured generations)
    amortized_edit_ns  = cumulative_edit_ns / measured edit count

No ratio is formed by dividing independent group medians. The analyzer must
retain both numerator and denominator rows and report missing-pair errors.

`upper_resident` and `first_touch` use the same checkpoint schedule; the
resident file is materialized in the fresh active upper before the timed open.

## 6. Sample lifecycle

Each sequence owns a fresh tree on one backing superblock:

    BACKING_DIR/.e3-work/<sequence-id>/
      base/
      generations/g001/upper/
      generations/g001/work/
      ...
      active/upper/
      active/work/
      merged/
      events.jsonl
      raw.jsonl

The `lower_direct` control uses the same `base/` contents in a separate sample
tree and writes it directly; the mounted `merged/` tree is never involved.

The runner creates the initial file tree, mounts OverlayFS with the fixed E2
feature options, and performs the prelude checkpoints needed for the declared
history depth. In a temporal-sweep sequence, the first measured generation is
started after the prelude; each later generation starts after the preceding
measured edit has been checkpointed to a fresh active upper. In a history-depth
sequence, the prebuilt chain is followed by exactly one measured generation.
The edit timer starts only after mount and request setup are complete.

After each timed edit in a temporal sweep, the runner verifies the postimage
hash, fsyncs as part of the measured operation, and checkpoints the resulting
active upper outside the timing interval. The next generation therefore starts
from an immutable snapshot and a fresh active upper. The final generation is
not checkpointed when doing so would exceed the declared 128-lower boundary. A
failed oracle preserves the complete sequence directory.

The runner must reject stock OverlayFS or a non-v2 module before collecting a
valid row. Every successful checkpoint must prove the expected generation
transition in the raw artifact.

## 7. Timing helper contract

The C helper performs all timed operations after opening and touching request
buffers. The timed region must not call `malloc`, `printf`, `readdir`, hash
functions, `syncfs`, or JSON serialization. It records:

- `clock_id = CLOCK_MONOTONIC_RAW`;
- `cpu_before` and `cpu_after`;
- `major_faults_delta`;
- `open_ns` (reopen mode);
- `pwrite_only_ns`;
- `fsync_ns`;
- `close_ns` (reopen mode);
- `edit_e2e_ns`;
- return value and errno for each syscall.

For `reopen`, `edit_e2e_ns = open_ns + pwrite_only_ns + fsync_ns + close_ns`.

CPU migration, a major fault, clock reversal, short write, unexpected file
size, or syscall error makes the row `invalid` or `failed` and retains its
reason. The row is never silently dropped.

Warm-up rows use the same lifecycle but are excluded from estimates. A warm-up
does not reset or alter the deterministic event schedule.

The cache policy is fixed warm-cache: the runner does not write
`/proc/sys/vm/drop_caches` and does not issue `POSIX_FADV_DONTNEED`. It fsyncs
the prelude and uses the same event order for all filesystems. Cell order is
deterministically shuffled, and paired numerator/denominator sequences are
alternated to reduce device-background and thermal drift.

## 8. Artifact schema

E3 has an independent artifact schema version 1. The manifest records:

    schema, preset, experiment, seed, git_commit, kernel_release,
    kernel_config_sha256, fs_type, fs_config, fs_uuid, backing_source,
    overlay_mount_options, deltafs_abi_version, access_mode, direct_baseline,
    workload_families, generation_counts, history_depths,
    sequences_per_cell, independent_runs, cpu, clocksource, started_at

Each result directory also contains canonical `events.jsonl`, measured
`raw.jsonl`, diagnostic `setup.jsonl`, `summary.json`, `stdout.log`,
`stderr.log`, `dmesg-before.log`, and `dmesg-after.log`. Failed sequence trees
are preserved below `.e3-work/`.

Each raw JSONL row records at least:

    schema, run, sequence_id, pair_id, event_id, sample, warmup, sample_kind,
    workload_family, access_mode, generation, history_depth,
    file_id, file_size, offset, write_bytes,
    open_ns, pwrite_only_ns, fsync_ns, close_ns, edit_e2e_ns,
    checkpoint_generation_before, checkpoint_generation_after,
    cpu_before, cpu_after, major_faults_delta,
    status, errno, invalid_reason, pre_sha256, post_sha256

`sample_kind` is one of `lower_direct`, `upper_resident`, `first_touch`,
`steady_after_copyup`, or `temporal`. Checkpoint setup rows are retained
separately and never included
in write-latency summaries.

`lower_direct` rows record `checkpoint_generation_before = 0` and
`checkpoint_generation_after = 0`; all DeltaFS rows must prove a one-generation
transition. The manifest records `direct_baseline = lower_direct` so analysis
rejects older two-variant result sets.

`pair_id` identifies the matched numerator/denominator sequence and is stable
across all four filesystem result sets. `event_id` identifies one deterministic
write within a sequence. The event file is canonical JSONL and its SHA-256 is
recorded in the manifest; analysis rejects a result root if any filesystem has a
different event hash.

For every measured row the runner also records a separate setup record with
`setup_kind` (`mount`, `checkpoint`, `syncfs`, or `unmount`) and its wall time.
Setup records are useful for diagnosing sequence cost but cannot be included in
`edit_e2e_ns`, `latency_amplification`, or any E3 primary plot.

## 9. Analysis outputs

`analyze.py RESULTS_ROOT` validates all four filesystem result sets, event
hashes, exact cell counts, paired controls, generation transitions, and raw
schema before producing:

    analysis/summary.tsv
    analysis/latency-by-generation.tsv
    analysis/latency-by-history-depth.tsv
    analysis/amplification.tsv
    analysis/deltafs-overhead.tsv
    analysis/burst-amortization.tsv
    analysis/invalid.jsonl
    analysis/pairing-errors.tsv
    analysis/latency-vs-generation.png
    analysis/latency-vs-history-depth.png
    analysis/latency-amplification.png
    analysis/latency-vs-lower.png
    analysis/summary.json

Tables report n, mean, p50, p95, p99, standard deviation, and a 95% CI. The CI
uses a two-level cluster bootstrap: sample sequences first, then rows within a
sequence, with fixed seed 14857. The analyzer reports both raw latency and
per-sequence paired ratios. It does not winsorize or remove outliers.

The primary plots show `edit_e2e_ns`, `first_touch_ratio`, and the direct-base
comparison separately. `deltafs-overhead.tsv` contains both
`ratio_to_lower_direct` and `overhead_ns` for `upper_resident`, `first_touch`,
and `steady_after_copyup`. A checkpoint latency plot is forbidden in E3's
primary output because that would overlap E1's ioctl question.

## 10. Validity and pass criteria

A preset passes only when:

1. all planned sequences and generations are present;
2. all four filesystem configurations have byte-identical event schedules;
3. every successful row has matching pre/post SHA-256 oracles;
4. every checkpoint reports the expected generation transition;
5. there are no invalid/failed rows, missing pairs, CPU migrations, major
   faults, or new dmesg diagnostics;
6. `lower_direct`, `upper_resident`, `first_touch`, and required
   `steady_after_copyup` controls are complete for every declared cell.

The hypotheses in section 3 are reported as supported or unsupported, but are
not numeric gates. A valid run can show no generation trend or no reflink
latency benefit.

## 11. Static verification

After implementation, host-side verification is limited to:

    make -C tools/deltafs clean all
    make -C tools/deltafs e3-bench
    make -C tools/deltafs/bench/e3 check
    python3 -m unittest discover tools/deltafs/bench/e3/tests
    make C=2 CHECK=sparse M=fs/overlayfs

The host must not load `overlay.ko`, mount OverlayFS, or claim functional
latency results. The helper is also compiled with `-Wall -Wextra -Werror` and a
separate GCC `-fanalyzer` pass.

## 12. QEMU handoff

All commands below are for the user in a QEMU guest capable of booting the
worktree kernel. The host-side build is static only; do not load the module on
the host.

From the repository root on the host:

    make -j"$(nproc)" bzImage modules
    make -C tools/deltafs clean e0-tools e3-bench
    make -C tools/deltafs check-e0-layout test-e0-controller
    make C=2 CHECK=sparse M=fs/overlayfs

Create four explicitly named disposable images and format them:

    truncate -s 20G /absolute/path/e3-ext4.raw
    truncate -s 20G /absolute/path/e3-xfs-noreflink.raw
    truncate -s 20G /absolute/path/e3-xfs-reflink.raw
    truncate -s 20G /absolute/path/e3-f2fs.raw
    mkfs.ext4 -F /absolute/path/e3-ext4.raw
    mkfs.xfs -f -m reflink=0 /absolute/path/e3-xfs-noreflink.raw
    mkfs.xfs -f -m reflink=1 /absolute/path/e3-xfs-reflink.raw
    mkfs.f2fs -f /absolute/path/e3-f2fs.raw

Boot with the worktree kernel, a guest root image, and the four data images:

    KERNEL=/absolute/path/to/arch/x86/boot/bzImage
    ROOTFS=/absolute/path/to/rootfs.qcow2
    REPO=/home/wangmingyu/repos/agentfs/fs/deltafs
    qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
      -kernel "$KERNEL" \
      -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
      -drive if=virtio,format=qcow2,file="$ROOTFS" \
      -drive if=virtio,format=raw,file=/absolute/path/e3-ext4.raw \
      -drive if=virtio,format=raw,file=/absolute/path/e3-xfs-noreflink.raw \
      -drive if=virtio,format=raw,file=/absolute/path/e3-xfs-reflink.raw \
      -drive if=virtio,format=raw,file=/absolute/path/e3-f2fs.raw \
      -virtfs local,path="$REPO",mount_tag=host,security_model=none

In the guest, verify the device names before mounting. The commands below
assume `/dev/vdb` through `/dev/vde`:

    mkdir -p /mnt/host
    mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
    bash /mnt/host/tools/deltafs/dev/guest-align.sh
    modprobe xfs f2fs overlay
    mkdir -p /mnt/e3/{ext4,xfs-noreflink,xfs-reflink,f2fs}
    mount /dev/vdb /mnt/e3/ext4
    mount /dev/vdc /mnt/e3/xfs-noreflink
    mount /dev/vdd /mnt/e3/xfs-reflink
    mount /dev/vde /mnt/e3/f2fs
    xfs_info /mnt/e3/xfs-noreflink | grep -w 'reflink=0'
    xfs_info /mnt/e3/xfs-reflink | grep -w 'reflink=1'
    install -D -m 0644 /mnt/host/fs/overlayfs/overlay.ko \
      "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
    depmod -a
    modprobe -r overlay 2>/dev/null || true
    modprobe overlay
    cd /mnt/host
    make -C tools/deltafs e3-bench

Run the E0 acceptance gate before the latency workload (the gate may unload the
module; reload it before E3):

    make -C tools/deltafs test-e0-controller
    mkdir -p /mnt/e3/xfs-reflink/v2-main /mnt/e3/xfs-noreflink/v2-extra
    tools/deltafs/deltafs_e0_acceptance_test.sh \
      --backing-root /mnt/e3/xfs-reflink/v2-main \
      --extra-backing-root /mnt/e3/xfs-noreflink/v2-extra
    modprobe overlay

The expected acceptance terminator is `All DeltaFS E0 acceptance checks
passed`; a skipped capability check is not a complete E3 gate.

Use a fresh result root and empty directories for the smoke run:

    mkdir -p /var/tmp/e3-results/smoke
    mkdir /mnt/e3/ext4/e3-smoke /mnt/e3/xfs-noreflink/e3-smoke \
      /mnt/e3/xfs-reflink/e3-smoke /mnt/e3/f2fs/e3-smoke
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/ext4/e3-smoke \
      /var/tmp/e3-results/smoke/ext4
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/xfs-noreflink/e3-smoke \
      /var/tmp/e3-results/smoke/xfs_noreflink
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/xfs-reflink/e3-smoke \
      /var/tmp/e3-results/smoke/xfs_reflink
    python3 tools/deltafs/bench/e3/run.py smoke \
      /mnt/e3/f2fs/e3-smoke \
      /var/tmp/e3-results/smoke/f2fs
    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/smoke

Expected final lines are `PASS: E3 smoke ...` for all four runners and
`PASS: E3 analysis completed`. Run `run` only after smoke passes, using a new
result root and fresh empty backing directories. The history-depth experiment
uses the same command shape with `depth-smoke`/`depth-run`; it includes the
128-lower boundary but performs no post-sample checkpoint at that boundary.
There is no held-fd preset because cross-generation ordinary file descriptors
are outside the supported DeltaFS contract.

For example, the depth smoke commands are:

    mkdir -p /var/tmp/e3-results/depth-smoke
    mkdir /mnt/e3/ext4/e3-depth-smoke /mnt/e3/xfs-noreflink/e3-depth-smoke \
      /mnt/e3/xfs-reflink/e3-depth-smoke /mnt/e3/f2fs/e3-depth-smoke
    python3 tools/deltafs/bench/e3/run.py depth-smoke \
      /mnt/e3/ext4/e3-depth-smoke \
      /var/tmp/e3-results/depth-smoke/ext4
    python3 tools/deltafs/bench/e3/run.py depth-smoke \
      /mnt/e3/xfs-noreflink/e3-depth-smoke \
      /var/tmp/e3-results/depth-smoke/xfs_noreflink
    python3 tools/deltafs/bench/e3/run.py depth-smoke \
      /mnt/e3/xfs-reflink/e3-depth-smoke \
      /var/tmp/e3-results/depth-smoke/xfs_reflink
    python3 tools/deltafs/bench/e3/run.py depth-smoke \
      /mnt/e3/f2fs/e3-depth-smoke \
      /var/tmp/e3-results/depth-smoke/f2fs
    python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/depth-smoke

The expected smoke sequence counts are 36 per filesystem for `smoke` and 30 for
`depth-smoke`; the runner and analyzer must report zero invalid/failed rows and
no pairing errors. Full `run` and `depth-run` use the same four serial commands
with new result roots and fresh empty directories. Their expected sequence
counts are 1,800 and 1,500 per filesystem respectively.

For every full preset, run the four filesystem commands serially, then invoke
the analyzer once. On any failure preserve `.e3-work`, `raw.jsonl`, and the
result directory. Collect diagnostics with:

    R=/var/tmp/e3-results/run/xfs_reflink
    dmesg -T > "$R/dmesg-failure-full.log"
    findmnt -J > "$R/findmnt-failure.json"
    cat /proc/mounts > "$R/proc-mounts-failure.txt"
    uname -a > "$R/uname.txt"
    lsblk -o NAME,MAJ:MIN,FSTYPE,SIZE,MOUNTPOINTS > "$R/lsblk.txt"
    cp -a "$R" /mnt/host/

On failure, preserve the sequence directory and collect `dmesg -T`, `findmnt -J`,
`/proc/mounts`, `uname -a`, the module file, the full result root, and the
printed `stderr.log`. Functional claims remain QEMU-only.

## 13. Relationship to E1 and E2

| benchmark | primary independent variable | primary dependent variable |
|---|---|---|
| E1 | target lower depth / switch operation | native ioctl latency |
| E2 | file size, dirty range, parent depth, backing FS | copy-up bytes and physical I/O |
| E3 | generation ordinal, history depth, burst/working-set mode | user-visible write latency and latency amplification |

E3 may reuse deterministic content generators and QEMU image conventions, but
it has its own runner, helper, schema, result roots, tests, and analyzer.
