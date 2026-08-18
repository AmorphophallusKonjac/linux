# DeltaFS v2 E2 switch bench 设计方案

> 当前实现使用 DeltaFS v2 checkpoint/restore UAPI，工件 schema 为 2。

> E2 只测 DeltaFS checkpoint/restore ioctl 的延迟和正确性。
> backing filesystem 和磁盘镜像由 QEMU 环境提供；bench 不执行 `mkfs`。
>
> 适用版本：DeltaFS v2，UAPI lower 上限为 128。

## 1. 简化目标和边界

E2 回答两个问题：

1. checkpoint/restore 的单次 ioctl 需要多长时间；
2. ioctl 延迟是否随目标 lower 数增长。

计时区间严格为：

    t0 = clock_gettime(CLOCK_MONOTONIC_RAW)
    ioctl(merged_fd, DELTAFS_IOC_CHECKPOINT or DELTAFS_IOC_RESTORE, &request)
    t1 = clock_gettime(CLOCK_MONOTONIC_RAW)

目录创建、rename、打开 fd、`syncfs`、mount/umount、hash、generation probe、日志和
结果落盘均在计时区间外。结果只能称为 DeltaFS switch ioctl latency，可与论文
Table 4 的 `0.07/0.19/0.25 ms` 作组件级参考；不能称为完整 agent checkpoint、
restore 或 DeltaBox Table 2 复现。

相较原合并方案，E2 删除以下公开维度：

- controller wrapper 分量和 metadata transaction；
- 外部 manifest、reset hook 和 lowers-file；
- workload、cache、CPU、seed、depth、warm-up、samples、batch-size 等命令行参数；
- random branch、Latin square、no-op control 和 cold-cache sensitivity；
- 多 sample 共用 mount 的 epoch/batch 生命周期。

E2 使用一个固定的空 upper workload。每层只放一个确定性 marker，用于验证层顺序，
不做目录数据复制。需要研究 controller 端到端开销或 cache/workload sensitivity 时，
应另建 benchmark，不能继续扩展 E2 的公开接口。

## 2. 用户接口

E2 只有两个运行命令和一个分析命令：

    sudo python3 tools/deltafs/bench/e2/run.py smoke BACKING_DIR OUT_DIR
    sudo python3 tools/deltafs/bench/e2/run.py run   BACKING_DIR OUT_DIR
    python3 tools/deltafs/bench/e2/analyze.py OUT_DIR

用户只提供：

- `BACKING_DIR`：目标 ext4/XFS backing filesystem 中的空专用目录；
- `OUT_DIR`：不存在或为空的结果目录，不能位于 `BACKING_DIR` 内。

`smoke` 和 `run` 是源代码中版本化的固定 preset。runner 自动完成以下工作：

- 用 `findmnt`/`statfs` 识别 filesystem 和 mount 参数；
- 选择当前进程 affinity 中第一个 CPU 并固定进程；
- 生成 seed、拓扑、marker、generation 和 request；
- 创建、挂载、验证和销毁每个 sample 的 sandbox；
- 保存 manifest、raw、stdout/stderr、dmesg 窗口和失败现场；
- 检查有效样本数并生成运行摘要。

runner 拒绝非 root、非空 `BACKING_DIR`、位于 backing 内的 `OUT_DIR`、已有 overlay
mount、跨 superblock 路径、不可用的 `CLOCK_MONOTONIC_RAW` 或不匹配的 mount feature。
它不接收环境变量形式的隐藏实验参数。

## 3. 独立代码布局

    tools/deltafs/bench/e2/
      Makefile
      switch_once.c
      e2_common.h
      e2_common.c
      run.py
      analyze.py
      tests/
        request_layout_test.c
        test_runner.py
        test_analyze.py

`switch_once` 是 runner 的私有子进程，不是用户接口。runner 为每个 sample 生成一个
`spec.json`，私有进程读取该文件、预先打开 fd 和构造 request，只把 ioctl 放在计时
区间内，并原子写出一个 `result.json`。用户不手工编写或复用 spec。
schema-2 spec 明确记录 `source_depth`、`keep_bottom` 和 `lower_prefix`；checkpoint 的
`keep_bottom=0` 且 prefix 为空，当前 ancestor restore 矩阵只设置 `keep_bottom`。

E2 不依赖 E3 的 event、FIEMAP、block-stat 或分析代码。两者可以共享 UAPI 头文件，
但不共享 runner、manifest schema 或 Makefile target。

## 4. 固定实验契约

### 4.1 lower depth

一个 view 定义为：

    view = (active_upper, active_work, lowers[0..D-1], generation)

`lowers[0]` 是最上层只读层，`lowers[D-1]` 是 base，`D` 是 lower 数，不包括
writable upper。分析变量保留名称 `request_depth`，在 schema 2 中定义为 ioctl 成功后的
target lower 深度，而不是 request 中的 fd 数：

- checkpoint：`request_depth = source_depth + 1`；
- restore：`request_depth = target_depth`。

v2 checkpoint 不携带 lower fd，内核从 current state 派生完整 lower chain。source
depth 127 的 checkpoint 是合法的 128-lower latency 样本；source depth 128 的
checkpoint 必须在用户态 preflight 得到 `E2BIG`，只作为 negative gate，不进入延迟
分布。v2 restore 使用 `keep_bottom=target_depth` 且 lower prefix 为空，因此当前固定
ancestor rollback 矩阵中 restore request 始终只有 fresh upper/work 两个 fd。

### 4.2 preset

固定 preset 如下：

| preset | operation/depth | warm-up | measured | independent run |
|---|---|---:|---:|---:|
| `smoke` | checkpoint source 1/127；restore target 1/128 | 每格 2 | 每格 5 | 1 |
| `run` | checkpoint source 1/2/4/8/16/32/64/127 | 每格 20 | 每格 200 | 5 |
| `run` | restore target 1/2/4/8/16/32/64/128 | 每格 20 | 每格 200 | 5 |

restore 的 source depth 固定为 128；target 是同一祖先链上的 checkpoint。即使
target depth 也是 128，restore 仍会丢弃当前 active upper 并安装 fresh upper/work。
raw 中记录 `rollback_distance = 128 - target_depth`、`keep_bottom`、`prefix_depth` 和
`request_fd_count`，主分析按 target lower depth（字段名仍为 `request_depth`）分组，
不能把它解释为 request fd 数，也不用 distance 声称复杂度。

每个 independent run 还执行一次 checkpoint@source-depth=128 preflight，必须得到
`E2BIG` 且不得调用 ioctl。seed 固定为 `14857`，只用于 marker 内容和 sample 顺序；
用户不能覆盖。将来改变 preset、seed 或 mount options 必须递增 manifest schema。

### 4.3 mount 和 topology

每个 sample 独占一个 mount 生命周期和如下目录：

    BACKING_DIR/.e2-work/<sample-id>/
      base/
      layers/l001 ... layers/l127/
      active/upper/
      active/work/
      next/upper/
      next/work/
      merged/

runner 只创建本 sample 需要的 layer。base 和 layer 各含一个只读 marker，marker 内容由
schema、seed、run、operation 和 depth 确定。active upper 为空。所有目录必须位于
同一 backing superblock，fresh upper/work 必须为空且互不重叠。

固定 mount options 为：

    index=off,nfs_export=off,metacopy=off,xino=off,uuid=off,redirect_dir=nofollow

checkpoint sample 直接以 source chain mount；计时前将空 active upper rename 为新的
最上层 lower，并创建 fresh upper/work。checkpoint request 只传 fresh upper/work。
restore sample 直接以 128 层 source chain mount；计时请求传 fresh upper/work 和
`keep_bottom=target_depth`，不重复传 retained lower suffix。

一次 ioctl 后立即在计时外验证、umount 并删除成功 sample 的工作目录。失败或 invalid
sample 不删除现场。这样每个 measured request 的 expected generation 恒为 1，成功后
应为 2，不需要 epoch、batch、setup restore、controller state.json 或 GC 策略。

## 5. 私有 driver

### 5.1 request builder

    int e2_build_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *req,
                                    int upper_fd, int work_fd,
                                    uint64_t expected_generation);
    int e2_build_restore_request(struct deltafs_ioc_restore_v2 *req,
                                 int upper_fd, int work_fd,
                                 const int *lower_fds,
                                 unsigned int nr_lower_prefix,
                                 unsigned int keep_bottom,
                                 uint64_t expected_generation);

builder 必须：

- 按操作构造 64-byte checkpoint 或 584-byte restore request；
- 清零整个结构，再设置 size、version、flags 和 expected generation；
- 保持 `reserved[]` 为 0；
- restore 将 `fds[nr_fds..129]` 全部设为 -1；
- restore 检查 `prefix_depth + keep_bottom` 为 1..128；
- 拒绝 generation 0，并且不引入用户指针。

单元测试覆盖两个 request 的固定布局、restore 的 0/1/128/129 target lower 边界、
reserved 字段，以及 checkpoint source depth 127/128 preflight。checkpoint@128 在
ioctl 前返回 `E2BIG`。

### 5.2 单次执行

`switch_once` 的固定顺序为：

1. 读取 runner 生成的 spec，验证 schema 和所有绝对路径；
2. 固定到 spec 已记录的自动选择 CPU；
3. 以 `O_RDONLY|O_DIRECTORY|O_CLOEXEC` 打开 merged 控制 fd，以
   `O_PATH|O_DIRECTORY|O_CLOEXEC` 打开 upper/work；restore 只额外打开 lower prefix；
4. 构造 request，预触碰 request/result buffer；
5. 读取 `getrusage` 和当前 CPU；
6. 读取 RAW clock，调用一次 ioctl，再读取 RAW clock；
7. 立即保存 return value 和 errno，再读 CPU 和 `getrusage`；
8. 关闭控制 fd，原子写 `result.json`。

计时区间内禁止 malloc、printf、fsync、hash、readdir、路径查找和日志函数。ioctl 前后
CPU 不同、major fault 非零或时钟倒退时，样本标为 `invalid`，但 raw 行仍保留。

## 6. runner 状态机和 oracle

固定状态机为：

    VALIDATE_ENV -> PREPARE_SAMPLE -> MOUNT_SOURCE -> VERIFY_SOURCE
      -> PREPARE_REQUEST -> RUN_ONCE -> PROBE_GENERATION -> VERIFY_TARGET
      -> WRITE_RAW -> UMOUNT -> REMOVE_OR_PRESERVE -> NEXT_SAMPLE -> SUMMARY

任一步失败都必须先写 raw/diagnostic，再停止当前 independent run。reset 简化为本 sample
的 `umount + rm -rf sample-dir`，不调用用户 hook，不格式化设备，也不跨 sample 复用
mount。

成功 ioctl 后使用新打开的 merged root fd 做 generation probe：

1. 构造合法布局的 v2 restore request（`keep_bottom=1`、`nr_fds=2`、两个 fd 为 -1）；
2. 以 generation 1 调用，必须返回 `ESTALE`；
3. 以 generation 2 调用，必须越过 generation 检查并返回 `EBADF`。

正确性 oracle 同时检查：

- merged 中可见 marker 集合和内容与目标 chain 完全一致；
- lower 顺序、inode、size、mtime 和 SHA-256 在 sample 前后不变；
- fresh upper 没有 marker，work 只允许 OverlayFS 内部条目；
- 对 merged 新写一个固定 probe 后，文件只出现在 fresh upper；
- 成功 request 的 generation probe 为 2；
- checkpoint@128 preflight 没有执行 ioctl，也没有改变 mount 内容。

oracle 或 generation probe 失败时 `status=invalid` 并保留整个 sample 目录。任何
`BUG/WARNING/KASAN/KFENCE/UBSAN/lockdep/RCU` 新 dmesg 关键字使 independent run 失败。

## 7. 输出和分析

### 7.1 自动 manifest

runner 在 `OUT_DIR/manifest.json` 自动记录：

    schema, deltafs_abi_version, preset, seed, git_commit, kernel_release,
    kernel_config_sha256, fs_type, fs_uuid, backing_source, backing_mount_options,
    deltafs_mount_options, cpu, clocksource, started_at,
    depth_matrix, warmup_count, measured_count, independent_runs

用户不提供 manifest。runner 同时保存 `raw.jsonl`、`summary.json`、`stdout.log`、
`stderr.log`、`dmesg-before.log`、`dmesg-after.log` 和失败 sample 的 `spec.json`。

### 7.2 raw JSONL

每次 measured/warm-up/negative 尝试一行：

    {
      "schema": 2,
      "run": 2,
      "sample": 17,
      "warmup": false,
      "operation": "restore",
      "source_depth": 128,
      "target_depth": 8,
      "request_depth": 8,
      "rollback_distance": 120,
      "keep_bottom": 8,
      "prefix_depth": 0,
      "request_fd_count": 2,
      "expected_generation": 1,
      "generation_after": 2,
      "cpu_before": 1,
      "cpu_after": 1,
      "major_faults": 0,
      "ioctl_ret": 0,
      "errno": 0,
      "ioctl_latency_ns": 183420,
      "status": "ok",
      "invalid_reason": null
    }

checkpoint 行的 `keep_bottom=0`、`prefix_depth=0`、`request_fd_count=2`；当前 restore
矩阵的对应值为 `target_depth`、0、2。`status` 只有 `ok`、`expected_reject`、
`invalid`、`failed`。分析器只纳入
`status=ok && warmup=false`；任何其他行都保留并单独汇总。

### 7.3 固定分析

`analyze.py OUT_DIR` 校验 manifest/raw 一致性和计划样本数，按 filesystem、operation、
request depth 输出 `n`、mean、median、p25/p75、p95、p99 和 stddev。CI95 使用两级
cluster bootstrap：先抽 independent run，再抽 run 内 sample，固定 10,000 次和 seed
14857。不 winsorize，不静默删除 outlier。

固定产物为：

    analysis/summary.tsv
    analysis/latency-vs-depth.tsv
    analysis/errno-counts.tsv
    analysis/invalid.jsonl
    analysis/latency-vs-depth.png

PNG 图使用 log2 request-depth 横轴和按 mean 数据范围自动取整的线性延迟纵轴，只展示
mean，不绘制置信区间。Checkpoint 使用蓝色实线和圆形 marker，restore 使用橙色虚线
和方形 marker；图题不重复实验编号。TSV 中继续保留 run-cluster bootstrap CI95 字段。

通过条件为：计划样本数全部满足、invalid/failed 为 0、所有 oracle 成功、depth 128
restore 成功、checkpoint@128 只得到一次计划内 `E2BIG`。论文数字仅作参考，不设绝对
延迟硬门槛，也不因曲线结果调整样本或删除数据。

## 8. 静态验证

实现后在仓库根目录执行：

    make -C tools/deltafs clean all
    make -C tools/deltafs e2-bench
    make -C tools/deltafs/bench/e2 check
    python3 -m unittest discover tools/deltafs/bench/e2/tests
    make C=2 CHECK=sparse M=fs/overlayfs

`check` 必须启用 `-Wall -Wextra -Werror`，并运行 request layout C 单元测试。当前开发
环境只做这些编译、链接、sparse 和 parser/analyzer 单元测试；禁止加载或运行 DeltaFS
module，也不能把宿主机数据当作功能或 latency 结果。

`e2-bench` 构建独立的 `switch_once`，不调用 `deltafsctl`，也不把 controller wall
time 混入 ioctl latency。`check` 只运行 host-safe 的布局和 parser/analyzer 单元测试。

2026-08-15 当前工作树已完成上述静态门禁：v2 userspace 工具和 E2 helper 以
`-Wall -Wextra -Werror` 编译链接通过；两个 v2 request layout、边界和 C preflight
测试通过；17 个 Python runner/analyzer 测试通过；GCC `-fanalyzer` 和严格 checkpatch
通过；v2 layout/controller/callsite 门禁通过；`fs/overlayfs`（含 `deltafs.c`）通过
sparse 并生成 `overlay.ko`。未在宿主机加载 module、mount OverlayFS 或产生 latency
数据；E2 smoke/run 的功能与性能结论仍必须由下述 QEMU/KVM 步骤产生。

## 9. QEMU/KVM 测试交接

以下命令由用户在能启动本工作树 kernel 的 QEMU/KVM 环境执行。

### 9.1 宿主机构建

在仓库根目录执行：

    make -j"$(nproc)" bzImage modules
    make -C tools/deltafs clean all
    make -C tools/deltafs v2-tools
    make -C tools/deltafs e2-bench

    if grep -q '^CONFIG_FUNCTION_ERROR_INJECTION=y' .config; then
      make -C tools/deltafs check-v2-checkpoints CHECKPOINT_MODE=enabled
    else
      make -C tools/deltafs check-v2-checkpoints CHECKPOINT_MODE=disabled
    fi

期望生成 `arch/x86/boot/bzImage`、`fs/overlayfs/overlay.ko`、v2 controller/layout/ioctl
工具以及 `tools/deltafs/bench/e2/switch_once`。

### 9.2 启动 guest

准备 rootfs 和两个已经格式化的专用 backing image。以下示例不会执行 `mkfs`：

    KERNEL=/absolute/path/to/arch/x86/boot/bzImage
    ROOTFS=/absolute/path/to/rootfs.qcow2
    DATA1=/absolute/path/to/deltafs-data1.raw
    DATA2=/absolute/path/to/deltafs-data2.raw

    qemu-system-x86_64 \
      -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
      -kernel "$KERNEL" \
      -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
      -drive if=virtio,format=qcow2,file="$ROOTFS" \
      -drive if=virtio,format=raw,file="$DATA1" \
      -drive if=virtio,format=raw,file="$DATA2" \
      -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,mount_tag=host,security_model=none

按实际 root partition 调整 `root=`。guest 内执行：

    mkdir -p /mnt/host /mnt/deltafs-test/{disk1,disk2,results}
    mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
    mount /dev/vdb /mnt/deltafs-test/disk1
    mount /dev/vdc /mnt/deltafs-test/disk2
    install -D -m 0644 /mnt/host/fs/overlayfs/overlay.ko \
      "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
    depmod -a
    mountpoint -q /sys/kernel/debug || \
      mount -t debugfs debugfs /sys/kernel/debug
    ulimit -n 192
    cd /mnt/host
    make -C tools/deltafs v2-tools
    make -C tools/deltafs e2-bench

确认 `uname -r` 对应本次 kernel、`findmnt -rn -t overlay` 没有输出，并以 root 执行
后续命令。不要对未知或已挂载块设备运行 `mkfs`。

### 9.3 现有 v2 功能门禁

历史 P1--P7 helper 已从 v2 工作树删除，不能把缺少这些旧入口记为失败。创建两个位于
不同 backing filesystem 的专用目录后，依次运行现行 v2 门禁：

    mkdir -p /mnt/deltafs-test/disk1/v2 /mnt/deltafs-test/disk2/v2
    make -C tools/deltafs check-v2-layout
    make -C tools/deltafs test-v2-controller
    make -C tools/deltafs test-v2-acceptance \
      BACKING_ROOT=/mnt/deltafs-test/disk1/v2 \
      EXTRA_BACKING_ROOT=/mnt/deltafs-test/disk2/v2

期望分别看到 layout PASS、controller unit tests PASS，以及：

    All DeltaFS v2 acceptance checks passed

acceptance 在缺少 fault injection、sanitizer 或其他 debug capability 时可以退出码 4 并
显示 capability-dependent `SKIP`；这不是完整 debug 证据，不能代替完整 v2 验收。

### 9.4 E2 smoke、主实验和分析

为当前 backing filesystem 准备空目录：

    rm -rf /mnt/deltafs-test/disk1/e2-backing \
      /mnt/deltafs-test/results/e2-smoke \
      /mnt/deltafs-test/results/e2-run
    mkdir -p /mnt/deltafs-test/disk1/e2-backing

先运行 smoke：

    python3 tools/deltafs/bench/e2/run.py smoke \
      /mnt/deltafs-test/disk1/e2-backing \
      /mnt/deltafs-test/results/e2-smoke

期望最终一行：

    PASS: E2 smoke completed; ok=28 invalid=0 failed=0 expected_reject=1

`28` 来自 4 个 cell 各 2 次 warm-up 和 5 次 measured；negative gate 另计。每条成功
raw 必须满足 `generation_after=2`，restore depth 128 成功，checkpoint@128 为
`expected_reject/E2BIG`。

smoke 通过后，重新创建空 backing 目录并运行固定主实验：

    rm -rf /mnt/deltafs-test/disk1/e2-backing
    mkdir -p /mnt/deltafs-test/disk1/e2-backing
    python3 tools/deltafs/bench/e2/run.py run \
      /mnt/deltafs-test/disk1/e2-backing \
      /mnt/deltafs-test/results/e2-run

    python3 tools/deltafs/bench/e2/analyze.py \
      /mnt/deltafs-test/results/e2-run

期望 run 最终一行为：

    PASS: E2 run completed; ok=17600 invalid=0 failed=0 expected_reject=5

每个 operation/depth 有 1,000 个 measured 样本；分析器最终输出
`PASS: E2 analysis completed; groups=16`，且分析目录包含第 7.3 节的五个固定文件。对
`ext4_noreflink`、`xfs_noreflink` 和 `xfs_reflink` 分别使用独立 image 重复上述命令，
不要在同一个已挂载 image 上重新格式化。

### 9.5 日志和失败诊断

失败时不要删除 `BACKING_DIR/.e2-work` 或 `OUT_DIR`。执行：

    R=/mnt/deltafs-test/results/e2-run
    dmesg -T > "$R/dmesg-failure-full.log"
    findmnt -J > "$R/findmnt-failure.json"
    cat /proc/mounts > "$R/proc-mounts-failure.txt"
    cat /proc/config.gz > "$R/kernel-config.gz" 2>/dev/null || \
      cp "/boot/config-$(uname -r)" "$R/kernel-config"
    uname -a > "$R/uname.txt"
    cp -a "$R" /mnt/host/

同时保存 runner 的退出码、stdout/stderr、失败 raw 行、对应 `spec.json` 和 sample 目录。
若残留 mount 阻止打包，先记录 `findmnt -J --target MERGED`，再只对该失败 sample 执行
`umount MERGED`。generation/oracle 不一致、计划外 errno、reset 失败或任何 kernel
sanitizer/lockdep/RCU 报告都使该 independent run 失败。

## 10. fast workdir / static validation 回归

本优化只改变 ioctl 构建路径的固定开销，不改变 v2 UAPI。宿主机先执行静态检查和
构建：

    make -C tools/deltafs check-v2-fast-path
    make -C tools/deltafs clean all

在内核源码树中按现有配置构建模块（未设置 `$KERNEL_BUILD` 时使用当前构建目录）：

    cd /home/wangmingyu/repos/agentfs/fs/deltafs
    make -C "${KERNEL_BUILD:-.}" M="$PWD/fs/overlayfs" modules

QEMU 中用该 kernel 和测试 disk image 启动（`ROOTFS`、`DATA1`、`DATA2` 换成实际
镜像，且不要对已挂载盘执行 `mkfs`）：

    KERNEL=/home/wangmingyu/repos/agentfs/fs/deltafs/arch/x86/boot/bzImage
    qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
      -kernel "$KERNEL" \
      -append 'root=/dev/vda1 rw console=ttyS0 nokaslr' \
      -drive if=virtio,format=qcow2,file="$ROOTFS" \
      -drive if=virtio,format=raw,file="$DATA1" \
      -drive if=virtio,format=raw,file="$DATA2" \
      -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,\
      mount_tag=host,security_model=none

Guest 中挂载 9p、数据盘和 debugfs 后，按 v2 phase-1/acceptance 的 mount 与
controller 初始化命令执行完整功能矩阵：

    mkdir -p /mnt/host /mnt/deltafs-test/{disk1,disk2}
    mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
    mount /dev/vdb /mnt/deltafs-test/disk1
    mount /dev/vdc /mnt/deltafs-test/disk2
    mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug
    cd /mnt/host

    make -C tools/deltafs all
    make -C tools/deltafs check-v2-fast-path
    tools/deltafs/deltafs_v2_phase1_test.sh \
      --backing-root /mnt/deltafs-test/disk1/phase1-run
    tools/deltafs/deltafs_v2_acceptance_test.sh \
      --backing-root /mnt/deltafs-test/disk1/acceptance-run \
      --extra-backing-root /mnt/deltafs-test/disk2/acceptance-run
    tools/deltafs/deltafs_v2_checkpoint_callsite_test.sh \
      fs/overlayfs/deltafs.o fs/overlayfs/deltafs.c auto

预期 phase-1、acceptance 和 callsite 脚本均以 `PASS` 结束；再运行 E2 smoke/run
并比较优化前后的 `checkpoint`/`restore` 截距。失败时保存：

    dmesg -T > /mnt/host/deltafs-fast-dmesg.log
    findmnt -J > /mnt/host/deltafs-fast-findmnt.json
    cat /proc/mounts > /mnt/host/deltafs-fast-mounts.txt

若 fast 前提（不同 superblock、idmapped mount、只读 remount 或非空/重叠目录）不满足，
期望得到明确的 `-EXDEV`、`-EOPNOTSUPP`、`-EROFS`、`-ENOTEMPTY` 或 `-EINVAL`，且
generation 和 active view 不发生变化。
