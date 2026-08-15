# DeltaFS v1 实现汇报

> 历史文档：v1 UAPI 测试 helper 和 P5--P7 harness 已从当前工具树删除。本文中的旧
> 测试命令只记录当时的实现状态，不适用于当前 v2 工作树。

> 用途：向导师汇报 DeltaFS v1 的实现进展、技术方案和验收安排。
> 代码基线：Linux 6.8.0，分支 `deltafs/6.8`，当前提交
> `d99dd3bfce6fba8b404595c1bba5ae9aadea33b0`（2026-08-11）。

## 一、结论先行

DeltaFS v1 的代码实现已经完成。当前版本在 Linux OverlayFS 上增加了运行时
checkpoint/restore 能力：在不执行 `umount/mount` 的情况下，冻结当前 writable
upper、安装新的 upper/work，并切换到指定的 lower chain；每次成功切换使 runtime
generation 严格加一，失败时保持原 active view 不变。

本次完成的交付物包括：

- 内核侧 OverlayFS 扩展、generation-aware cache 和原子 view commit；
- 版本化 UAPI（两个 ioctl）；
- C 语言 controller `deltafsctl` 及 manifest/transaction 处理；
- P5--P7 测试 helper、故障注入和最终验收 harness（P1–P4 阶段性回归已被 P7
  覆盖并清理）；
- 设计文档与 QEMU/KVM 测试交接说明。

需要明确区分验证来源：本开发环境只做静态构建，不加载自定义内核模块；用户已于
2026-08-11 确认在目标 QEMU/KVM debug guest 中运行
`p7_acceptance_test.sh` 通过，包含 in-kernel 功能、内存安全和 100 次切换验收；
该结果属于本次扩展前的 64-lower 基线。当前 128-lower、164-switch 版本必须重跑。
原始 P7 结果目录尚未导入当前源码工作区，后续汇报仍应附带原始日志，而不是用本
文档替代运行证据。

## 二、项目目标与 v1 范围

DeltaFS 不是新的磁盘文件系统，而是 OverlayFS 的运行时重配置扩展。目标是为
有状态 AI agent sandbox 提供低开销的 checkpoint/rollback：checkpoint 主要是
目录 rename 和元数据切换，restore 只重建 layer view，不复制目录树和文件数据。
由于论文原型代码尚未公开，本实现属于基于论文语义和 Linux 6.8 源码完成的
clean-room 复现，不声称与原作者的私有 ABI 或内部数据结构逐行一致。

v1 采用“工作负载静止（quiesced）”模型，支持单 OverlayFS、单 backing
superblock 和最多 128 个 lower。以下场景明确不属于 v1 保证范围：跨切换继续使用
普通文件/目录 fd、merged mount 内的 cwd/root、writable `MAP_SHARED`、并发
switch/copy-up、异步 I/O、在线回收 retired layer，以及 controller 崩溃后的自动
事务恢复。

## 三、核心实现

### 3.1 内核数据模型

`struct ovl_fs` 增加初始值为 1 的 `delta_generation`、切换互斥锁、retired
state 链表和 backing superblock 身份；`struct ovl_inode` 记录其创建时的
generation。每个 target/retired state 独占 layer array、private mount、trap、
workdir、root binding 和配置字符串，避免只交换裸数组造成悬空引用。

### 3.2 generation 缓存协议

- 正 dentry 在访问 backing 状态前先比较 inode generation 与 filesystem
  generation；RCU walk 遇到不一致时退出到 ref-walk，随后重新 lookup。
- inode cache key 同时包含 real inode 和 generation，允许同一 backing inode 在
  不同 view 中对应不同 overlay inode。
- negative dentry 不跨 syscall 长期缓存；root inode 不能重建，因此在 commit
  中原地更新并递增目录 version，使 root readdir cache 失效。

### 3.3 target state 构建与原子提交

ioctl 先复制固定大小的 UAPI 结构，再将目录 fd 转成稳定的 `struct path` 引用，
完成 ABI、权限、feature、同 superblock、重复/重叠路径、空 upper/work、层数和
checkpoint chain 校验。新 view 在 active view 外完整构建；所有 private mount、
trap、workdir 和 root lower stack 成功准备后，才在固定锁序
`delta_lock -> root i_rwsem -> root ovl_inode.lock` 下移动所有权并发布。

generation 的最后一步使用 release store；因此观察到新 generation 的 reader 必然
观察到完整的新 layer/root binding。旧 view 保留在 retired list，直到 OverlayFS
卸载时统一释放，保证 stale dentry/inode 不访问已释放的 layer。

### 3.4 用户态 controller

`deltafsctl` 负责 controller lock、严格解析 `state.json`、写入并 fsync
`transaction.json`、创建 fresh branch、checkpoint 时 rename frozen layer、调用
ioctl，以及成功/失败后的 metadata 提交或补偿。controller 不猜测内核状态：若
ioctl 成功后进程崩溃，transaction 会保留并阻止后续操作，要求人工恢复。

## 四、用户可见 UAPI

头文件：[`include/uapi/linux/deltafs.h`](../include/uapi/linux/deltafs.h)。

| 项目 | v1 定义 |
| --- | --- |
| ABI | `DELTAFS_ABI_VERSION = 1`，固定请求结构 584 bytes |
| 命令 | `DELTAFS_IOC_CHECKPOINT`、`DELTAFS_IOC_RESTORE` |
| 控制 fd | 已挂载 OverlayFS 的 merged root directory fd |
| 参数 fd | upper/work/lower 均为 `O_PATH|O_DIRECTORY|O_CLOEXEC` |
| 权限 | 目标 user namespace 中需要 `CAP_SYS_ADMIN` |
| generation | `expected_generation` 精确匹配；成功后加 1，失配返回 `-ESTALE` |
| lower 上限 | 1--128；超过上限返回 `-E2BIG` |
| 失败语义 | commit 开始前任意错误均不改变 active view |

v1 推荐的 OverlayFS mount 约束为：

```text
index=off,nfs_export=off,metacopy=off,xino=off,
uuid=off,redirect_dir=nofollow,volatile=off,numdatalayer=0
```

所有输入目录必须位于同一个 backing `struct super_block`；若要复现实验中的低
写放大，应使用启用 reflink 的 XFS，让 OverlayFS 现有 copy-up 路径优先复用
`vfs_clone_file_range()`。当前交付聚焦正确性，尚未给出可与论文直接比较的延迟
数据。

## 五、阶段性完成情况

| 阶段 | 主要工作 | 结果 |
| --- | --- | --- |
| P1 | 定义 UAPI、root ioctl dispatcher、ABI 负向矩阵 | 已完成 |
| P2 | passive state、generation 初始化、mount/unmount 生命周期 | 已完成 |
| P3 | inode/dentry generation cache、负 dentry 策略、root readdir version | 已完成 |
| P4 | target state builder、private mount/trap/workdir 构建和完整 unwind | 已完成 |
| P5 | 原子 view commit、checkpoint/restore、多次切换和 retired state | 已完成 |
| P6 | `deltafsctl`、manifest、transaction、分支和崩溃边界 | 已完成 |
| P7 | 深层 layer、动态故障注入、ABI matrix、164 次切换和 module unload harness | 旧 64 层基线通过；当前 128 层版本待重跑 |

代码主要集中在 `fs/overlayfs/deltafs.c`，并接入
`ovl_entry.h`、`params.c`、`super.c`、`inode.c`、`namei.c`、`util.c` 和
`readdir.c`；用户态实现和测试位于 `tools/deltafs/`。

## 六、当前验证状态

### 6.1 本环境已完成的静态验证

以下命令在仓库根目录执行并通过：

```bash
make -j"$(nproc)" bzImage
make -j"$(nproc)" M=fs/overlayfs modules
make C=2 CHECK=sparse M=fs/overlayfs
make -C tools/deltafs clean all
make -C tools/deltafs check-p7-checkpoints CHECKPOINT_MODE=enabled
make -C tools/deltafs test-p6-controller
make -C tools/deltafs test-p7-schedule
for f in tools/deltafs/*.sh; do bash -n "$f"; done
shellcheck tools/deltafs/p7_checkpoint_callsite_test.sh
```

关键结果：

- `arch/x86/boot/bzImage` 构建完成；
- `fs/overlayfs/overlay.ko` 成功链接；
- `sparse` 完成 OverlayFS/DeltaFS 全部目标检查，未输出诊断；
- 全部 P5/P6/P7 native helper 和 `deltafsctl` 成功编译；
- P7 静态检查确认 debug `deltafs.o` 保留全部 18 个
  `ovl_deltafs_build_checkpoint()` 调用点；关闭
  `CONFIG_FUNCTION_ERROR_INJECTION` 的 production 构建应以
  `CHECKPOINT_MODE=disabled` 确认调用点为 0；本次已在独立临时源码/输出树完成
  production 模块编译链接，且对象中没有 checkpoint 符号；
- host-safe P6 controller transaction tests 全部通过。
- `shellcheck` 和全部 shell 脚本的 `bash -n` 检查通过；
- 重构后 `bzImage`、debug `overlay.ko`、production `overlay.ko` 和全部 userspace
  helper 均编译、链接成功。

另行运行的 `checkpatch.pl` 报告了若干内核既有风格项和 DeltaFS 新代码的格式
建议；编译、链接和 `sparse` 检查不受影响，格式项后续可单独清理。

### 6.2 QEMU/KVM 验收状态

当前开发环境不能启动目标 Linux 6.8 内核，也不加载生成的 `overlay.ko`。用户已
确认目标 QEMU/KVM debug guest 中的旧 64-lower P7 总验收通过，包括真实
checkpoint/restore、cache 失效、retired state 生命周期、
KASAN/KFENCE/UBSAN/lockdep/kmemleak 和第 17 节八项判定。由于
`section-17.tsv`、`dmesg-window.log`、`kmemleak.log` 及结果目录未导入当前工作区，
这里不记录无法核实的具体 checkpoint 数或日志路径；该确认对应编译隔离重构前的
基线；当前 128-lower 固定 UAPI 和深层矩阵需按第 7.5 节重跑，交付时应补充新的
原始目录。

2026-08-13 用户在缺少故障注入、kmemleak 和 sanitizer 全能力的 QEMU/KVM guest
中运行当前 128 层 P7：P5 cache、P5 checkpoint、P6 controller、native ABI matrix、
128-lower chain 以及第 129 次 checkpoint 的无变更拒绝均执行到预期结果。随后旧
harness 的 36 次历史 restore 调度最终落到 `s01`（合法深度 2），却固定断言深度
128，因此以 `active lower depth is 2, expected 128` 失败。该问题属于 P7 harness，
不是内核或 controller 丢层；调度现已改为循环 8/32/64/128 层并最终停在 `s127`，
同时新增 host-safe `test-p7-schedule`。修复后的完整 P7 仍须重新运行，不能把上述
部分结果视为第 17 节最终通过证据。

## 七、QEMU/KVM 测试交接

下面步骤可直接交给测试人员执行。所有会 mount、`modprobe`、debugfs fault
injection 或卸载模块的命令，只在目标 guest 中运行。

### 7.1 宿主机构建

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make -C tools/deltafs p7-tools
make -C tools/deltafs check-p7-checkpoints CHECKPOINT_MODE=enabled
```

debug guest 至少应启用：

```text
CONFIG_OVERLAY_FS=m
CONFIG_MODULE_UNLOAD=y
CONFIG_DEBUG_KERNEL=y
CONFIG_DEBUG_FS=y
CONFIG_DEBUG_KMEMLEAK=y
CONFIG_FUNCTION_ERROR_INJECTION=y
CONFIG_KASAN=y
CONFIG_KFENCE=y
CONFIG_UBSAN=y
CONFIG_PROVE_LOCKING=y
CONFIG_PROVE_RCU=y
```

production 构建关闭下列配置，使 ownership checkpoint 及对应错误分支完全消失：

```text
# CONFIG_FUNCTION_ERROR_INJECTION is not set
# CONFIG_FAULT_INJECTION is not set
```

production 对象的静态门命令为：

```bash
make -C tools/deltafs check-p7-checkpoints \
  KERNEL_DELTAFS_OBJ=/absolute/production/build/fs/overlayfs/deltafs.o \
  CHECKPOINT_MODE=disabled
# 期望：PASS: ... removes all 18 source ... call sites
```

P7 深层注入只能使用前述 debug 配置；production guest 可重跑 P5 和 P6 非注入
功能。`p7_acceptance_test.sh` 现在对能力型调试选项做探测：缺少故障注入
（`CONFIG_FUNCTION_ERROR_INJECTION` 或 `/sys/kernel/debug/fail_function`）、
kmemleak 或 sanitizer/lockdep/PROVE_RCU 时，跳过依赖该能力的阶段（深层故障注入
循环改用一次非注入 restore 顶替 switch 128、kmemleak 扫描跳过、sanitizer dmesg
覆盖减弱），非注入部分仍运行，并以 SKIP（退出码 4）结束；能力齐全时才以 0 退出。
基础前置（QEMU/KVM guest、无既有 overlay mount、模块化 overlay、overlay/module-unload
/debugfs 配置、可写 `/dev/kmsg` 等）不满足仍是失败。

准备一个 rootfs 和两个独立数据盘；`DATA1`、`DATA2` 必须使用不同 backing
superblock（P7 用 `DATA2` 验证 `-EXDEV`）。若复现 reflink 写放大，建议在盘上使用
`mkfs.xfs -m reflink=1 -b size=4096`。

### 7.2 启动 VM

将下列变量替换为实际绝对路径：

```bash
KERNEL=/absolute/path/to/arch/x86/boot/bzImage
ROOTFS=/absolute/path/to/rootfs.qcow2
DATA1=/absolute/path/to/p7-disk1.raw
DATA2=/absolute/path/to/p7-disk2.raw

qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
  -kernel "$KERNEL" \
  -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
  -drive if=virtio,format=qcow2,file="$ROOTFS" \
  -drive if=virtio,format=raw,file="$DATA1" \
  -drive if=virtio,format=raw,file="$DATA2" \
  -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,mount_tag=host,security_model=none
```

若 rootfs 使用的根分区不是 `/dev/vda1`，只修改 `root=`；保留串口 console 以便
收集日志。

### 7.3 Guest 准备

```bash
mkdir -p /mnt/host /mnt/deltafs-test/p7/disk1 /mnt/deltafs-test/p7/disk2
mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
mount /dev/vdb /mnt/deltafs-test/p7/disk1
mount /dev/vdc /mnt/deltafs-test/p7/disk2
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug

install -D -m 0644 /mnt/host/fs/overlayfs/overlay.ko \
  "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
depmod -a
cd /mnt/host
make -C tools/deltafs p7-tools
```

确认 `uname -r` 对应本次 kernel、`findmnt -t overlay` 没有测试外的挂载，并以
root 身份执行后续命令。128-lower native/controller 矩阵需要至少 160 个可用文件
描述符；若 guest 的软限制更低，先执行 `ulimit -n 160`（或在 QEMU rootfs 的
limits 配置中提高硬限制）。

为各阶段脚本准备目录（普通测试使用第一块数据盘，跨 superblock 用例使用第二块）：

```bash
mkdir -p /mnt/deltafs-test/p7/disk1/{p2,p3,p4,p5,p6}
mkdir -p /mnt/deltafs-test/p7/disk2/p4-extra
```

### 7.4 P5--P6 阶段测试

> P1–P4 是 P5 原子 commit 引入前的阶段性回归：合法 build 刻意以 `-EOPNOTSUPP`
> 结束且 generation 不变。最终 HEAD 对相同合法请求必须成功提交，P7 不重跑这些
> 互斥的旧终态断言；其 ABI/路径负向覆盖与 builder unwind 已分别由 P7 native
> matrix 与 128 层动态故障注入接管，对应 `p1`–`p4` 测试文件已作为过时阶段性
> 证据清理。本节仅保留 P5/P6 阶段脚本（P7 总入口也会重跑它们）。

脚本会自动拒绝非 QEMU/KVM guest、已有 OverlayFS mount、非模块化 overlay 或缺少
debug 配置。每个脚本成功时的关键输出如下：

```bash
# P5：单次提交和多次 checkpoint/restore（同一挂载）
sudo tools/deltafs/p5_commit_test.sh \
  --backing-root /mnt/deltafs-test/p7/disk1/p5
sudo tools/deltafs/p5_checkpoint_test.sh \
  --backing-root /mnt/deltafs-test/p7/disk1/p5
# 期望：All P5 QEMU first-commit tests passed
#       All P5 checkpoint/multi-commit QEMU tests passed

# P6：controller 分支、补偿和崩溃边界
make -C tools/deltafs test-p6-controller   # host-safe parser/transaction 单测
sudo tools/deltafs/p6_controller_test.sh \
  --backing-root /mnt/deltafs-test/p7/disk1/p6
# 期望：All P6 checkpoint/restore controller tests passed
```

说明：P1–P4 曾是 P5 原子 commit 前的阶段性回归门，其“合法 build 后返回
`-EOPNOTSUPP`”断言对应各阶段 commit；P5 引入真正 commit 后，最终 HEAD 对相同
请求必须成功提交，这些互斥旧断言不再保留，对应 `p1`–`p4` 测试文件已清理。
最终版本的 ABI 负向矩阵和 builder unwind 分别由 P7 的 `p7_ioctl_test` 与深层
动态故障注入负责。

### 7.5 P7 最终验收（推荐唯一总入口）

```bash
make -C tools/deltafs p7-tools
tools/deltafs/p7_acceptance_test.sh \
  --backing-root /mnt/deltafs-test/p7/disk1 \
  --extra-backing-root /mnt/deltafs-test/p7/disk2
```

P7 会串联 P5/P6 场景，并额外覆盖：128-lower restore、129-lower `-E2BIG`、ABI
failure-atomicity、动态 `-ENOMEM` 故障注入、8/32/64/128 层历史 restore、总计
164 次成功切换、反复
umount/module unload、sanitizer 和 kmemleak。通过标准是同时看到：

```text
All P7 DeltaFS v1 acceptance checks passed
section-17.tsv 中 1--8 条均为 PASS
deep fault injection exhausted after N injected checkpoints（N >= 128）
```

2026-08-11 用户确认的是旧 64-lower、100-switch 基线。上述 128-lower 三项尚待在
debug guest 重新运行本节命令；production 构建关闭
`CONFIG_FUNCTION_ERROR_INJECTION` 时不会生成注入 helper。

### 7.6 日志和失败诊断

P7 会在 `--backing-root` 下创建 `deltafs-p7-results-*`，至少保留：
`summary.log`、`section-17.tsv`、每阶段日志、`dmesg-window.log`、
`kmemleak.log`、kernel config 和 `deep-fault-checkpoints.txt`。失败时在 guest
执行：

```bash
R=$(find /mnt/deltafs-test/p7/disk1 -maxdepth 1 \
  -type d -name 'deltafs-p7-results-*' | sort | tail -1)
cp -a "$R" /mnt/host/
dmesg -T > /mnt/host/deltafs-p7-dmesg-full.log
findmnt -t overlay > /mnt/host/deltafs-p7-overlay-mounts.log
cat /proc/config.gz > /mnt/host/deltafs-p7-config.gz 2>/dev/null || \
  cp "/boot/config-$(uname -r)" /mnt/host/deltafs-p7-config
```

若脚本异常退出，还要保留终端提示的 `.deltafs-p7-run.*` 和
`.deltafs-p7-extra.*` 目录；不要手工删除 transaction 或 frozen layer，以免丢失
故障现场。

## 八、已知限制与后续工作

1. 旧 fd、旧 cwd 和 writable mmap 的跨切换语义尚未纳入 v1；调用 ioctl 前必须由
   controller 保证 workload 静止并释放引用。
2. retired state 在卸载时回收，v1 没有在线 GC；长时间运行需控制 checkpoint
   次数和 layer 深度。
3. controller 没有 GET_STATE 或掉电自动恢复接口；存在 transaction 时采取
   fail-stop，后续由管理员依据 manifest 处理。
4. 性能复现（不同 layer depth、文件规模、XFS reflink 与 ext4 的对比）属于 P8，
   可在已通过的 P7 基线上单独测量 mean/median/p95/p99。

## 九、可直接口头汇报的摘要

“DeltaFS v1 已在 Linux 6.8 OverlayFS 上完成编码。实现通过固定 UAPI、generation
缓存协议和 immutable target view，把 checkpoint/restore 变成一次可验证的运行时
层切换；新状态在锁外构建，提交阶段只做不可失败的所有权移动，因此成功时
generation 加一、失败时 active view 不变。用户态 `deltafsctl` 负责目录 rename、
manifest、fsync 和失败补偿，并支持从任意历史 checkpoint 创建新分支。宿主机已完
成内核模块、userspace 工具、P7 调用点和 controller 单元测试的静态验证；最终的
内核功能与内存安全证据将在 QEMU/KVM debug guest 中运行 P7 总验收后提交。”

详细设计和复现背景见：

- [`deltafs_v1_design.md`](deltafs_v1_design.md)
- [`deltafs_reproduction_report.md`](deltafs_reproduction_report.md)
