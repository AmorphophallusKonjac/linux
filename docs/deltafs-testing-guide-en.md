# DeltaFS E0-E3 Testing Guide

This is the consolidated English entry point for the current DeltaFS tests.
`v2` names the kernel ABI; the experiment numbers are:

| Experiment | Purpose | Public entry point |
|---|---|---|
| E0 | Correctness and consistency gate | `make -C tools/deltafs test-e0-acceptance` |
| E1 | checkpoint/restore ioctl latency | `tools/deltafs/bench/e1/run.py` |
| E2 | copy-up, FIEMAP, and physical I/O | `tools/deltafs/bench/e2/run.py` |
| E3 | user-visible write latency across generations | `tools/deltafs/bench/e3/run.py` |

DeltaFS does not currently support carrying ordinary file descriptors,
directory descriptors, cwd, mmap, or io_uring across checkpoint/restore. E0-E3
require a quiesced workload with no cross-switch references other than the
merged-root control fd. E3 is reopen-only and has no `fd-run` preset.

## 1. Host build and static gates

Run from the source root on the host:

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make M=fs/overlayfs W=1
make C=2 CHECK=sparse M=fs/overlayfs
make -C tools/deltafs clean e0-tools e1-bench e2-bench e3-bench
make -C tools/deltafs check-e0-layout
make -C tools/deltafs test-e0-controller
make -C tools/deltafs check-e0-checkpoints CHECKPOINT_MODE=auto
make -C tools/deltafs check-e0-fast-path
make -C tools/deltafs/bench/e1 check
make -C tools/deltafs/bench/e2 check
make -C tools/deltafs/bench/e3 check
for f in tools/deltafs/*.sh; do bash -n "$f"; done
```

Every command must exit zero. This environment runs static tests only; do not
load the module or run functional experiments on the host.

## 2. Boot QEMU

Prepare a rootfs and four formatted, disposable data disks. E2 needs dedicated
block devices so `/sys/block/*/stat` is meaningful. Never format an unknown or
mounted device.

```bash
KERNEL=/home/wangmingyu/repos/agentfs/fs/deltafs/arch/x86/boot/bzImage
ROOTFS=/absolute/path/to/rootfs.qcow2
EXT4=/absolute/path/to/ext4.raw
XFS_NR=/absolute/path/to/xfs-noreflink.raw
XFS_RF=/absolute/path/to/xfs-reflink.raw
F2FS=/absolute/path/to/f2fs.raw

truncate -s 20G "$EXT4" "$XFS_NR" "$XFS_RF" "$F2FS"
mkfs.ext4 -F "$EXT4"
mkfs.xfs -f -m reflink=0 "$XFS_NR"
mkfs.xfs -f -m reflink=1 "$XFS_RF"
mkfs.f2fs -f "$F2FS"

qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 8192 -nographic \
  -kernel "$KERNEL" \
  -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
  -drive if=virtio,format=qcow2,file="$ROOTFS" \
  -drive if=virtio,format=raw,file="$EXT4" \
  -drive if=virtio,format=raw,file="$XFS_NR" \
  -drive if=virtio,format=raw,file="$XFS_RF" \
  -drive if=virtio,format=raw,file="$F2FS" \
  -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,\
mount_tag=host,security_model=none
```

Adjust only `root=` when the guest root partition differs. Complete E0 evidence
should use a debug kernel with KASAN, KFENCE, UBSAN, lockdep, kmemleak, and
function error injection enabled.

## 3. Initialize the guest

```bash
sudo -i
mkdir -p /mnt/host
mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
cd /mnt/host
bash tools/deltafs/dev/guest-align.sh
modprobe xfs f2fs overlay
mkdir -p /mnt/deltafs/{ext4,xfs-noreflink,xfs-reflink,f2fs}
mount /dev/vdb /mnt/deltafs/ext4
mount /dev/vdc /mnt/deltafs/xfs-noreflink
mount /dev/vdd /mnt/deltafs/xfs-reflink
mount /dev/vde /mnt/deltafs/f2fs
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
findmnt -no FSTYPE /mnt/deltafs/ext4
xfs_info /mnt/deltafs/xfs-noreflink | grep -w 'reflink=0'
xfs_info /mnt/deltafs/xfs-reflink | grep -w 'reflink=1'
findmnt -no FSTYPE /mnt/deltafs/f2fs
make -C tools/deltafs e0-tools e1-bench e2-bench e3-bench
```

The checks must report `ext4`, `reflink=0`, `reflink=1`, and `f2fs`, respectively.

## 4. E0 correctness gate

E0 must pass before any performance experiment. The two backing roots must be
on different filesystems:

```bash
rm -rf /mnt/deltafs/xfs-reflink/e0-main \
       /mnt/deltafs/xfs-noreflink/e0-extra
make -C tools/deltafs test-e0-acceptance \
  BACKING_ROOT=/mnt/deltafs/xfs-reflink/e0-main \
  EXTRA_BACKING_ROOT=/mnt/deltafs/xfs-noreflink/e0-extra
```

The complete success terminator is:

```text
All DeltaFS E0 acceptance checks passed
```

Exit code 4 or any `SKIP` means a debug capability was unavailable and is not
complete E0 evidence. Acceptance unloads the module; run `modprobe overlay`
before E1-E3.

## 5. E1 ioctl latency

Use fresh backing and result directories for each filesystem:

```bash
modprobe overlay
mkdir /mnt/deltafs/xfs-reflink/e1-smoke
python3 tools/deltafs/bench/e1/run.py smoke \
  /mnt/deltafs/xfs-reflink/e1-smoke /var/tmp/e1-smoke

mkdir /mnt/deltafs/xfs-reflink/e1-run
python3 tools/deltafs/bench/e1/run.py run \
  /mnt/deltafs/xfs-reflink/e1-run /var/tmp/e1-run
python3 tools/deltafs/bench/e1/analyze.py /var/tmp/e1-run
```

Expected terminators:

```text
PASS: E1 smoke completed; ok=28 invalid=0 failed=0 expected_reject=1
PASS: E1 run completed; ok=17600 invalid=0 failed=0 expected_reject=5
PASS: E1 analysis completed; groups=16
```

Repeat on ext4, XFS without reflink, and XFS with reflink without reusing any
directory.

## 6. E2 copy-up and physical I/O

This example runs smoke. `DEVICE_STAT` must identify the exact backing device:

```bash
mkdir -p /var/tmp/e2-results/smoke
for SPEC in \
  'ext4 /mnt/deltafs/ext4 /sys/block/vdb/stat' \
  'xfs_noreflink /mnt/deltafs/xfs-noreflink /sys/block/vdc/stat' \
  'xfs_reflink /mnt/deltafs/xfs-reflink /sys/block/vdd/stat' \
  'f2fs /mnt/deltafs/f2fs /sys/block/vde/stat'
do
  set -- $SPEC
  mkdir "$2/e2-smoke"
  python3 tools/deltafs/bench/e2/run.py smoke \
    "$2/e2-smoke" "$3" "/var/tmp/e2-results/smoke/$1"
done
python3 tools/deltafs/bench/e2/analyze.py /var/tmp/e2-results/smoke
```

Each runner must report `ok=36 invalid=0 failed=0 controls=6`; analysis must
end with `PASS: E2 analysis completed`. Run `run`, `depth-smoke`, and
`depth-run` with fresh directories. Full expected counts are:

```text
run:       ok=11700 invalid=0 failed=0 controls=1755
depth-run: ok=7800  invalid=0 failed=0 controls=1170
```

## 7. E3 write latency

E3 does not read device sector counters, but each result directory must reside
on a different device from its backing directory:

```bash
mkdir -p /var/tmp/e3-results/smoke
for SPEC in \
  'ext4 /mnt/deltafs/ext4' \
  'xfs_noreflink /mnt/deltafs/xfs-noreflink' \
  'xfs_reflink /mnt/deltafs/xfs-reflink' \
  'f2fs /mnt/deltafs/f2fs'
do
  set -- $SPEC
  mkdir "$2/e3-smoke"
  python3 tools/deltafs/bench/e3/run.py smoke \
    "$2/e3-smoke" "/var/tmp/e3-results/smoke/$1"
done
python3 tools/deltafs/bench/e3/analyze.py /var/tmp/e3-results/smoke
```

Each smoke run must report `sequences=36 rows=7938`; each event now has three paired rows:
`lower_direct`, `upper_resident`, and `first_touch`/`steady_after_copyup`.
Analysis must end with `PASS: E3 analysis completed`. The
`analysis/deltafs-overhead.tsv` and `analysis/latency-vs-lower.png` artifacts
report each DeltaFS path relative to a direct write to the lower base
filesystem, both as `ratio_to_lower_direct` and `overhead_ns`. This control uses
an independent `base/` tree, with no OverlayFS mount or checkpoint. The full
temporal `run` has `sequences=1800 rows=396900` per filesystem. The expected
history-depth counts are `sequences=30 rows=630` for `depth-smoke` and
`sequences=1500 rows=31500` for `depth-run`.

## 8. Failure diagnostics and archival

Stop after any failure. Preserve `BACKING_DIR/.eN-work`, `OUT_DIR`, raw JSONL,
FIEMAP dumps, and failed samples. Collect:

```bash
R=/var/tmp/deltafs-failure
mkdir -p "$R"
dmesg -T > "$R/dmesg.log"
findmnt -J > "$R/findmnt.json"
cat /proc/mounts > "$R/proc-mounts.txt"
uname -a > "$R/uname.txt"
lsblk -o NAME,MAJ:MIN,FSTYPE,SIZE,MOUNTPOINTS > "$R/lsblk.txt"
cp /sys/kernel/debug/kmemleak "$R/kmemleak.txt" 2>/dev/null || true
cp -a "$R" /mnt/host/
```

Also archive the exact command, exit status, complete stdout/stderr, kernel
configuration, `overlay.ko`, and result tree. Any hash/generation oracle error
or BUG/WARNING/KASAN/KFENCE/UBSAN/lockdep/RCU-stall diagnostic invalidates the
run. Detailed contracts are in [E0](deltafs-e0-test-plan.md),
[E1](deltafs-e1-test-plan.md), [E2](deltafs-e2-test-plan.md), and
[E3](deltafs-e3-test-plan.md).
