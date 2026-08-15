# DeltaFS v2 第一阶段测试交接

本文只验收 v2 工作包一：checkpoint/restore UABI、kernel target builder、layer
source-path 生命周期和 native ioctl helper。format-2 controller、自动化 guest
acceptance、E2/E3 更新属于后续工作包；旧 v1 P1-P7 binary 使用已删除的 v1 request，
不能作为 v2 结果运行或引用。

## 1. Host 静态构建

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make M=fs/overlayfs W=1
make C=2 CHECK=sparse M=fs/overlayfs
make -C tools/deltafs clean v2-phase1-tools
make -C tools/deltafs check-v2-layout
```

期望 `bzImage`、`modules`、`W=1`、sparse 和 userspace `-Werror` 均退出 0，layout
测试输出：

```text
DeltaFS v2 UABI layout checks passed
```

再执行：

```bash
for f in tools/deltafs/*.sh; do bash -n "$f"; done
shellcheck tools/deltafs/deltafs_v2_phase1_test.sh
for f in \
  include/uapi/linux/deltafs.h \
  fs/overlayfs/ovl_entry.h \
  fs/overlayfs/deltafs.c \
  tools/deltafs/deltafs_v2_layout_test.c \
  tools/deltafs/deltafs_v2_ioctl_test.c
do
  scripts/checkpatch.pl --no-tree --strict --file "$f"
done
rg -n "call_rcu|kfree_rcu|rcu_assign_pointer|synchronize_rcu" fs/overlayfs/
make -C tools/deltafs check-p7-checkpoints CHECKPOINT_MODE=auto
```

`super.c` 和 `params.c` 是 upstream 6.8 文件，整文件 strict checkpatch 存在与本次
source-path 修改无关的基线告警；其改动由 `W=1` 和 sparse 覆盖。`rg` 不得出现由 v2
layer-stack 发布或回收引入的匹配。本开发环境不得加载 module 或 mount DeltaFS。

## 2. 启动 QEMU

准备可启动 rootfs 和两个已经格式化的数据盘。不要对未知或已挂载设备执行 `mkfs`。

```bash
KERNEL=/home/wangmingyu/repos/agentfs/fs/deltafs/arch/x86/boot/bzImage
ROOTFS=/absolute/path/to/rootfs.qcow2
DATA1=/absolute/path/to/deltafs-v2-data1.raw
DATA2=/absolute/path/to/deltafs-v2-data2.raw

qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
  -kernel "$KERNEL" \
  -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
  -drive if=virtio,format=qcow2,file="$ROOTFS" \
  -drive if=virtio,format=raw,file="$DATA1" \
  -drive if=virtio,format=raw,file="$DATA2" \
  -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,mount_tag=host,security_model=none
```

若 root 分区不是 `/dev/vda1`，只调整 `root=`。debug kernel 建议启用
`CONFIG_OVERLAY_FS=m`、`CONFIG_MODULE_UNLOAD=y`、`CONFIG_DEBUG_FS=y`、
`CONFIG_DEBUG_KMEMLEAK=y`、`CONFIG_FUNCTION_ERROR_INJECTION=y`、`CONFIG_KASAN=y`、
`CONFIG_KFENCE=y`、`CONFIG_UBSAN=y` 和 `CONFIG_PROVE_LOCKING=y`。

## 3. Guest 准备

```bash
mkdir -p /mnt/host /mnt/deltafs-v2/{disk1,disk2}
mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
mount /dev/vdb /mnt/deltafs-v2/disk1
mount /dev/vdc /mnt/deltafs-v2/disk2
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
cd /mnt/host
```

第 4 节脚本会自动构建 userspace helper、安装并加载 `overlay.ko`、设置 `ulimit` 和
执行 layout check。

## 4. 一键运行

第一阶段 guest 测试已经收成一个入口。debug guest 直接执行：

```bash
cd /mnt/host
sudo make -C tools/deltafs test-v2-phase1 \
  BACKING_ROOT=/mnt/deltafs-v2/disk1/phase1-run

# 等价的直接入口：
sudo tools/deltafs/deltafs_v2_phase1_test.sh \
  --backing-root /mnt/deltafs-v2/disk1/phase1-run
```

脚本会自动完成 module 重载、userspace 构建、临时测试树、UABI 负向矩阵、
checkpoint/restore、source-path rename、generation probe、ownership fault injection、
unmount/module unload 和诊断日志采集。成功输出：

```text
All DeltaFS v2 phase1 checks passed
```

结果保留在 `--backing-root` 下的 `results-*` 目录。没有
`CONFIG_FUNCTION_ERROR_INJECTION` 或 `fail_function` 时，脚本仍运行 smoke/ABI 测试，
但返回 4 并明确标记 fault injection 为 `SKIP`；只需要 smoke 时可显式传
`--no-fault-injection`。脚本自身会构建 `v2-phase1-tools`，因此第 3 节的手工 `make`
只用于提前检查，不是运行脚本的额外要求。

## 5. Checkpoint 与 restore（手工分解）

```bash
R=/mnt/deltafs-v2/disk1/phase1
rm -rf "$R"
mkdir -p "$R"/{base,layers,merged}
mkdir -p "$R"/branches/{g1,g2,g3}/{upper,work}
printf 'base\n' > "$R/base/base-only"

OPTS="lowerdir=$R/base,upperdir=$R/branches/g1/upper"
OPTS+=",workdir=$R/branches/g1/work,index=off,nfs_export=off"
OPTS+=",metacopy=off,xino=off,uuid=off,redirect_dir=nofollow"
mount -t overlay overlay -o "$OPTS" "$R/merged"
tools/deltafs/deltafs_v2_ioctl_test negative \
  "$R/merged" 1 "$R/branches/g2/upper" "$R/branches/g2/work"
# 期望：DeltaFS v2 negative ioctl checks passed

printf 'checkpoint-one\n' > "$R/merged/checkpoint-one"
sync -f "$R/merged"

mv "$R/branches/g1/upper" "$R/layers/c1"
tools/deltafs/deltafs_v2_ioctl_test checkpoint \
  "$R/merged" 1 "$R/branches/g2/upper" "$R/branches/g2/work"
# 期望：checkpoint: generation 1 -> 2

test "$(cat "$R/merged/checkpoint-one")" = checkpoint-one
printf 'generation-two\n' > "$R/merged/generation-two"

tools/deltafs/deltafs_v2_ioctl_test restore \
  "$R/merged" 2 1 "$R/branches/g3/upper" "$R/branches/g3/work"
# 期望：restore: generation 2 -> 3 (keep_bottom=1, prefix=0)

test "$(cat "$R/merged/base-only")" = base
test ! -e "$R/merged/checkpoint-one"
test ! -e "$R/merged/generation-two"
tools/deltafs/deltafs_v2_ioctl_test probe-generation "$R/merged" 3
# 期望：generation 3 is current

umount "$R/merged"
modprobe -r overlay
```

checkpoint 在 `mv` 后成功证明 kernel 使用 stable source path 派生旧 upper；restore
证明 `prefix + current bottom suffix` 的 `keep_bottom=1` 语义。helper 的 restore
命令允许在 work 参数后继续传 top-to-bottom lower prefix 路径，用于手工覆盖
`keep_bottom=0` 和混合 prefix/suffix。

## 6. Ownership fault injection（手工分解）

下面的 debug-guest 测试从第 1 个 ownership checkpoint 开始逐点注入 `-ENOMEM`；
每次失败后验证 generation 和可见内容不变，直到第一个未命中的序号成功提交。测试使用
独立 mount，不与上一节目录复用。

```bash
set -Eeuo pipefail
modprobe overlay
FROOT=/mnt/deltafs-v2/disk1/phase1-fault
rm -rf "$FROOT"
mkdir -p "$FROOT"/{base,layers,merged,initial/upper,initial/work,trials}
printf 'base\n' > "$FROOT/base/base-only"
OPTS="lowerdir=$FROOT/base,upperdir=$FROOT/initial/upper"
OPTS+=",workdir=$FROOT/initial/work,index=off,nfs_export=off"
OPTS+=",metacopy=off,xino=off,uuid=off,redirect_dir=nofollow"
mount -t overlay overlay -o "$OPTS" "$FROOT/merged"
printf 'frozen\n' > "$FROOT/merged/frozen"
sync -f "$FROOT/merged"
mv "$FROOT/initial/upper" "$FROOT/layers/c1"

FAIL=/sys/kernel/debug/fail_function
SYMBOL=ovl_deltafs_build_checkpoint
test -d "$FAIL"
test -d "$FAIL/$SYMBOL" || printf '%s\n' "$SYMBOL" > "$FAIL/inject"
printf '0xFFFFFFFFFFFFFFF4\n' > "$FAIL/$SYMBOL/retval"
printf '100\n' > "$FAIL/probability"
printf '1\n' > "$FAIL/interval"
printf '0\n' > "$FAIL/verbose"

checkpoint_success=
for n in $(seq 1 512); do
  B="$FROOT/trials/checkpoint-$n"
  rm -rf "$B"
  mkdir -p "$B"/{upper,work}
  printf '0\n' > "$FAIL/times"
  printf '%s\n' "$n" > "$FAIL/space"
  printf '1\n' > "$FAIL/times"
  set +e
  output=$(tools/deltafs/deltafs_v2_ioctl_test checkpoint \
    "$FROOT/merged" 1 "$B/upper" "$B/work" 2>&1)
  status=$?
  set -e
  printf '0\n' > "$FAIL/times"
  if ((status == 0)); then
    checkpoint_success=$n
    break
  fi
  grep -Fq 'Cannot allocate memory' <<<"$output"
  tools/deltafs/deltafs_v2_ioctl_test probe-generation "$FROOT/merged" 1
  test "$(cat "$FROOT/merged/frozen")" = frozen
done
test -n "$checkpoint_success"
tools/deltafs/deltafs_v2_ioctl_test probe-generation "$FROOT/merged" 2

restore_success=
for n in $(seq 1 512); do
  B="$FROOT/trials/restore-$n"
  rm -rf "$B"
  mkdir -p "$B"/{upper,work}
  printf '0\n' > "$FAIL/times"
  printf '%s\n' "$n" > "$FAIL/space"
  printf '1\n' > "$FAIL/times"
  set +e
  output=$(tools/deltafs/deltafs_v2_ioctl_test restore \
    "$FROOT/merged" 2 1 "$B/upper" "$B/work" 2>&1)
  status=$?
  set -e
  printf '0\n' > "$FAIL/times"
  if ((status == 0)); then
    restore_success=$n
    break
  fi
  grep -Fq 'Cannot allocate memory' <<<"$output"
  tools/deltafs/deltafs_v2_ioctl_test probe-generation "$FROOT/merged" 2
  test "$(cat "$FROOT/merged/frozen")" = frozen
done
printf '0\n' > "$FAIL/times"
printf '!%s\n' "$SYMBOL" > "$FAIL/inject"
test -n "$restore_success"
tools/deltafs/deltafs_v2_ioctl_test probe-generation "$FROOT/merged" 3
test "$(cat "$FROOT/merged/base-only")" = base
test ! -e "$FROOT/merged/frozen"
printf 'checkpoint_failures=%s restore_failures=%s\n' \
  "$((checkpoint_success - 1))" "$((restore_success - 1))"
umount "$FROOT/merged"
modprobe -r overlay
```

期望每个注入请求都以 `ENOMEM` 失败且 probe 保持原 generation；两个循环最终各成功
一次，输出的 failure 数都大于 0。随后 unmount/module unload 必须成功，`dmesg` 不得
出现 UAF、refcount、lockdep、KASAN/KFENCE/UBSAN 或 warning。

## 7. 日志与失败诊断

任一命令失败后不要删除测试树，执行：

```bash
OUT=/mnt/deltafs-v2/disk1/phase1-failure-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
printf '0\n' > /sys/kernel/debug/fail_function/times 2>/dev/null || true
printf '!ovl_deltafs_build_checkpoint\n' > \
  /sys/kernel/debug/fail_function/inject 2>/dev/null || true
dmesg -T > "$OUT/dmesg-full.log"
findmnt -J > "$OUT/findmnt.json"
cat /proc/mounts > "$OUT/proc-mounts.txt"
cat /sys/kernel/debug/kmemleak > "$OUT/kmemleak.log" 2>/dev/null || true
cat /proc/config.gz > "$OUT/kernel-config.gz" 2>/dev/null || \
  cp "/boot/config-$(uname -r)" "$OUT/kernel-config"
uname -a > "$OUT/uname.txt"
grep -Ei 'deltafs|overlay|use-after-free|refcount|lockdep|BUG:|WARNING:' \
  "$OUT/dmesg-full.log" > "$OUT/dmesg-focus.log" || true
cp -a "$OUT" /mnt/host/
```

报告需包含失败命令、退出码、stdout/stderr、active/target chain、generation、
`keep_bottom`、prefix 数量和首次异常前后的 `dmesg`。第一阶段完成只表示以上静态
门禁和 QEMU smoke 通过；完整 1..128 lower/prefix 深度矩阵、cache、压力、反复卸载、
kmemleak 和 controller crash compensation 仍由第二阶段总 acceptance 验收。
