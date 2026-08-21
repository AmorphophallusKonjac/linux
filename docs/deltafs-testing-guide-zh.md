# DeltaFS E0-E3 测试用户手册

本文是 DeltaFS 当前测试的统一中文入口。`v2` 表示内核 ABI 版本；实验编号固定为：

| 实验 | 内容 | 公开入口 |
|---|---|---|
| E0 | 正确性与一致性门禁 | `make -C tools/deltafs test-e0-acceptance` |
| E1 | checkpoint/restore ioctl 延迟 | `tools/deltafs/bench/e1/run.py` |
| E2 | copy-up、FIEMAP 与物理 I/O | `tools/deltafs/bench/e2/run.py` |
| E3 | 跨 generation 的用户可见写延迟 | `tools/deltafs/bench/e3/run.py` |

当前 DeltaFS 不支持 checkpoint/restore 前保留普通文件 fd、目录 fd、cwd、mmap 或
io_uring。E0-E3 都要求 workload 静止，并且除 merged-root control fd 外没有跨切换
引用。E3 只使用 checkpoint 后重新打开路径的 `reopen` 模式，没有 `fd-run`。

## 1. Host 构建与静态门禁

在 host 的源码根目录执行：

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

以上命令必须全部退出 0。本环境只执行这些静态测试，不在 host 加载模块或运行功能
实验。

## 2. 启动 QEMU

准备一个 rootfs 和四个已格式化、无重要数据的实验盘。E2 必须使用独立 block device
才能正确读取 `/sys/block/*/stat`；不要对未知或已挂载设备执行 `mkfs`。

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

若 root 分区不是 `/dev/vda1`，只修改 `root=`。完整 E0 建议使用启用了 KASAN、
KFENCE、UBSAN、lockdep、kmemleak 和 function error injection 的 debug kernel。

## 3. Guest 初始化

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

第一条应输出 `ext4`，后两条必须分别匹配 `reflink=0` 和 `reflink=1`，第四条应输出 `f2fs`。

## 4. E0 正确性门禁

E0 必须在任何性能实验之前通过。两个 backing root 必须位于不同 filesystem：

```bash
rm -rf /mnt/deltafs/xfs-reflink/e0-main \
       /mnt/deltafs/xfs-noreflink/e0-extra
make -C tools/deltafs test-e0-acceptance \
  BACKING_ROOT=/mnt/deltafs/xfs-reflink/e0-main \
  EXTRA_BACKING_ROOT=/mnt/deltafs/xfs-noreflink/e0-extra
```

完整成功终止行为：

```text
All DeltaFS E0 acceptance checks passed
```

退出码 4 或任何 `SKIP` 只表示 guest 缺少 debug capability，不能作为完整 E0 证据。
acceptance 会卸载模块；开始 E1-E3 前执行 `modprobe overlay`。

## 5. E1 ioctl 延迟

每种 backing filesystem 分别使用新的空目录和结果目录：

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

预期终止行：

```text
PASS: E1 smoke completed; ok=28 invalid=0 failed=0 expected_reject=1
PASS: E1 run completed; ok=17600 invalid=0 failed=0 expected_reject=5
PASS: E1 analysis completed; groups=16
```

对 ext4、XFS noreflink 和 XFS reflink 重复运行，目录和结果不得复用。

## 6. E2 copy-up 与物理 I/O

下例执行 smoke。`DEVICE_STAT` 必须对应 backing 所在的准确设备：

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

每个 runner 预期 `ok=36 invalid=0 failed=0 controls=6`，分析器预期输出
`PASS: E2 analysis completed`。完整实验使用新目录将 preset 改为 `run`；路径深度实验
依次使用 `depth-smoke` 和 `depth-run`。完整计数分别为：

```text
run:       ok=11700 invalid=0 failed=0 controls=1755
depth-run: ok=7800  invalid=0 failed=0 controls=1170
```

## 7. E3 写延迟

E3 不读取设备 sector counter，但结果目录必须在与 backing 不同的 device 上：

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

每个 smoke 应报告 `sequences=36 rows=7938`；每个事件现在有 `lower_direct`、
`upper_resident` 和 `first_touch`/`steady_after_copyup` 三个配对样本，分析器应输出
`PASS: E3 analysis completed`。`analysis/deltafs-overhead.tsv` 和
`analysis/latency-vs-lower.png` 展示各 DeltaFS 路径相对直接 lower base 文件系统写入的
比值（`ratio_to_lower_direct`）和额外纳秒数（`overhead_ns`）。这项对比使用独立的
`base/` 文件树，不挂载 OverlayFS，也不执行 checkpoint。
完整 temporal 实验使用 `run`，每个 filesystem 为 `sequences=1800 rows=396900`；
历史层深度实验使用 `depth-smoke`/`depth-run`，其预期分别为
`sequences=30 rows=630` 和 `sequences=1500 rows=31500`。

## 8. 失败诊断与归档

任何错误都应停止后续实验。不要删除 `BACKING_DIR/.eN-work`、`OUT_DIR`、raw JSONL、
FIEMAP dump 或失败 sample。收集：

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

同时保存命令、退出码、完整 stdout/stderr、kernel `.config`、`overlay.ko` 和对应结果
目录。任何 hash/generation oracle 错误，或 BUG/WARNING/KASAN/KFENCE/UBSAN/lockdep/
RCU stall，都会使本次运行无效。详细实验契约见
[E0](deltafs-e0-test-plan.md)、[E1](deltafs-e1-test-plan.md)、
[E2](deltafs-e2-test-plan.md) 和 [E3](deltafs-e3-test-plan.md)。
