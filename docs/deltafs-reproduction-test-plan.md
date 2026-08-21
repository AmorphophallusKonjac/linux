# DeltaFS 评估实验复现方案

## 1. 文档目标与复现边界

本文依据 Zotero 中的论文 **DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback**（Dong et al., arXiv:2605.22781v2, 2026）整理。目标是在 **DeltaFS 文件系统代码已经完成** 的前提下，复现论文中可以归因于 DeltaFS 的评估结论，并为后续与 DeltaCR/DeltaBox 集成预留接口。

本方案把实验分成三个层次：

1. **主线：DeltaFS 独立实验**
   - 热切换 checkpoint/rollback 的正确性。
   - OverlayFS 层切换延迟。
   - ext4、XFS、XFS+reflink、F2FS 下的 copy-up 写放大。
   - ext4、XFS、XFS+reflink、F2FS 下的物理 I/O。
2. **增强：当前支持边界内的文件系统验证**
   - 任意历史点回滚、深层栈、cache revalidation、故障注入和 teardown。
3. **后续：需要 DeltaCR 或完整 DeltaBox 的耦合实验**
   - SWE-bench MCTS 每事件 checkpoint/restore。
   - 30 轮 MCTS 端到端开销。
   - RL fan-out 和 GPU occupation。

E1 和 E2 已拆成两个独立 benchmark。E1 的固定 switch-latency preset、当前
128-lower ABI 的 depth 口径和 QEMU 交接见
[deltafs-e1-test-plan.md](deltafs-e1-test-plan.md)；E2 的 event、FIEMAP/sector counter
采样和 QEMU 交接见 [deltafs-e2-test-plan.md](deltafs-e2-test-plan.md)。

仅完成文件系统代码时，可以复现论文 Fig. 9 的核心结果和 Table 4 的 `Overlay ioctl switch` 分量；不能把只测 DeltaFS 的结果表述为 Table 2、Fig. 6 或 Fig. 7 的完整 DeltaBox 复现。

## 2. 从论文抽取的实验事实

### 2.1 系统和实现

| 项目 | 论文设置 |
|---|---|
| 客体系统 | DeltaFS，基于 Linux 6.8 OverlayFS |
| 实现规模 | 约 565 行 C，分布于 4 个文件 |
| checkpoint | 将当前 upper 重命名/降级为只读 lower，安装新 upper，并用 ioctl 原子切换 layer array |
| rollback | ioctl 将层栈切到目标 checkpoint 的配置 |
| 缓存处理 | 使旧 dentry 重新验证，避免返回 stale upper |
| 旧层栈回收 | 论文设计描述为延后两个 checkpoint；实现段描述为 RCU grace period 后回收 |
| 打开文件处理 | 每文件系统 `checkpoint_gen`；写入时发现代际不匹配则重新解析并 copy-up |
| 并发 copy-up | 通过 backing mount/mount namespace identity 判断并解决 EEXIST 竞争 |
| backing FS | 原型使用 XFS reflink；OverlayFS copy-up 通过 `vfs_clone_file_range` 共享 extent |
| 部署 | Firecracker microVM 内自定义 Linux 6.8 内核；同样机制也适用于容器/其他 VM |

### 2.2 论文硬件和虚拟机配置

| 项目 | 论文设置 |
|---|---|
| 服务器 | 4 路，96 个物理核 / 192 个硬件线程 |
| 内存 | 760 GiB |
| 实验盘 | 400 GB NVMe SSD，保存实验数据和快照 |
| DeltaBox VM | 4 vCPU，8 GB RAM |
| VM 文件系统 | XFS rootfs block device + 只读 XFS data image |
| data image 内容 | 目标代码仓库和 Conda 环境 |
| 回滚层 | 在上述存储上叠加 OverlayFS/DeltaFS |
| RL 额外硬件 | 单节点 4 GPU，每卡 96 GB |

### 2.3 工作负载

论文从 SWE-bench Verified 选取四类 MCTS 轨迹：

| 类别 | 仓库/特征 | Table 2 轨迹数 |
|---|---|---:|
| Django | fat process | 4 |
| SymPy | read-heavy exploration | 4 |
| Scientific | Astropy、Matplotlib、scikit-learn、Xarray；NumPy-heavy、process-dominated | 10 |
| Tools/Small | pylint、requests、pytest；lightweight | 6 |

Table 2 中每条轨迹回放两次。论文没有披露具体 SWE-bench instance ID、完整编辑轨迹或随机种子。

DeltaFS 写放大实验使用真实 SWE-Search agent 编辑，按被编辑文件大小分成：

- 1-8 KiB
- 8-16 KiB
- 16-32 KiB
- 32-64 KiB
- 64-128 KiB
- 128-256 KiB

论文将 12-65 KiB 标为典型 agent 编辑文件范围。Fig. 9 报告每个大小桶的中位数，但没有披露各桶样本数、具体文件、patch 内容或 cache 状态。

### 2.4 DeltaFS 直接相关指标和论文结果

#### Table 4：Overlay ioctl switch 延迟

在 standard-path SWE-bench MCTS replay 中：

| 操作 | 论文均值 |
|---|---:|
| checkpoint 的 Overlay ioctl switch | 0.07 ms |
| fast restore 的 Overlay ioctl switch | 0.19 ms |
| slow restore 的 Overlay ioctl switch | 0.25 ms |

这些是 DeltaBox 组件拆分数据，不等于完整 checkpoint/restore 延迟。论文同时报告完整 checkpoint 本地工作均值 10.83 ms、fast restore 1.86 ms、slow restore 9.29 ms，其中主要差额来自 DeltaCR。

#### Fig. 9(a)：copy-up duplicated data

- ext4 without reflink 与 XFS without reflink 曲线基本重合。
- 两者随文件大小近似线性增长，因为修改会复制整个文件。
- XFS with reflink 只为实际改脏的 4 KiB block 分配新空间，曲线低且相对平坦。
- 论文只给出图形趋势，没有提供 Fig. 9(a) 的原始数值表。

#### Fig. 9(b)：physical I/O

- 指标是 loopback device 写扇区，包括 journal 和 metadata。
- 小文件 1-8 KiB：论文正文给出的代表值为约 132 KiB 降至 26 KiB；主要收益来自 XFS 较轻的 metadata bookkeeping，reflink 增益较小。
- 大文件 128-256 KiB：论文正文给出的代表值为约 315 KiB 降至 141 KiB；主要收益来自 reflink 共享数据块。
- 结果报告每桶中位数。

#### Table 2 和 Fig. 6 中的 DeltaFS 贡献

- Table 2 的 DeltaBox checkpoint/restore 加权均值为 10.83/1.86 ms，但这是 DeltaFS+DeltaCR 的耦合结果。
- Fig. 6 中 DeltaBox 的 30 轮 MCTS 总时间为理想 `LLM+action` 的 1.01-1.02 倍；E2B(diff) 为 1.30-1.93 倍。
- 论文称 DeltaFS restore 的 layer switch 与 template fork 重叠，因此完整 fast restore 不能通过简单相加组件均值得到。

Table 2 的完整均值目标如下，单位均为 ms：

| Workload | replay+cp ck | FC-Diff+dm ck | CRIU+cp ck | E2B(diff) ck | DeltaBox ck |
|---|---:|---:|---:|---:|---:|
| Django | 568.1 | 740.7 | 791.7 | 487.7 | 12.12 |
| SymPy | 171.4 | 476.9 | 503.4 | 536.3 | 11.96 |
| Scientific | 265.9 | 603.0 | 435.7 | 552.5 | 10.86 |
| Tools/Small | 86.4 | 511.7 | 361.4 | 515.4 | 9.16 |
| Event-weighted average | 347.0 | 622.3 | 590.0 | 524.4 | 10.83 |

| Workload | replay+cp rs | FC-Diff+dm rs | CRIU+cp rs | E2B(diff) rs | DeltaBox rs |
|---|---:|---:|---:|---:|---:|
| Django | 28437 | 3784 | 1116 | 790.2 | 2.23 |
| SymPy | 31591 | 3279 | 635.7 | 900.7 | 2.21 |
| Scientific | 28299 | 3256 | 655.1 | 952.1 | 1.82 |
| Tools/Small | 20883 | 2920 | 427.1 | 933.3 | 1.46 |
| Event-weighted average | 27694 | 3429 | 811.4 | 899.7 | 1.86 |

其中 `replay+cp` 的 checkpoint 是每条轨迹一次 pristine-repo copy 的 trace-weighted proxy；restore 包含从冷状态重放到目标节点，但论文对已记录 completion 采用零延迟重放。`FC-Diff+dm` 的 checkpoint 包含每条轨迹一次初始 root VM checkpoint。因此，即使将来完成全系统复现，也必须复刻这些特殊计量规则，不能把五套系统的普通 API wall time 直接并排。

## 3. 论文未披露、复现前必须冻结的参数

这些项目必须写入实验 manifest；不能默默采用默认值：

1. 服务器 CPU/NUMA 型号、NVMe 型号和固件。
2. Linux 6.8 的精确 commit、内核 `.config`、DeltaFS commit。
3. `mkfs.ext4`、`mkfs.xfs` 和 mount 的完整参数。
4. XFS 是否使用 `reflink=0/1`、block size、sector size、CRC、bigtime 等 feature。
5. DeltaFS ioctl ABI、用户态 layer config 格式和最大层数。
6. 每次 checkpoint 是否把 upper 重命名、创建新 workdir，以及哪些步骤计入延迟。
7. SWE-bench instance ID、仓库 commit、Conda/依赖镜像 digest。
8. 编辑轨迹、每个文件大小桶样本数、编辑偏移和实际修改字节数。
9. 是否执行 `fsync`/`syncfs`，物理 I/O 稳定判据和 cache 处理方式。
10. 重复次数、warm-up 次数、CPU affinity、SMT/频率策略、NUMA 绑定。

首选做法是向作者获取 artifact/trace。拿不到时，按第 8 节的“独立冻结轨迹”执行，并把结果标记为 **method reproduction**，而非 **exact artifact reproduction**。

## 4. 复现目标与假设

| 编号 | 假设 | 主要指标 | 对应论文 |
|---|---|---|---|
| H1 | DeltaFS 能在不 umount 的情况下 checkpoint/rollback，并保持历史状态正确 | 文件内容与 metadata oracle 一致 | §4.1 |
| H2 | layer switch 为亚毫秒级，且 rollback 到任意历史点不复制目录内容 | ioctl latency | Table 4 |
| H3 | 无 reflink 时 copy-up bytes 随文件大小增长；有 reflink 时主要随实际 dirty block 数增长 | bytes/edit、log-log slope | Fig. 9(a) |
| H4 | XFS metadata 降低小文件 physical I/O，reflink 进一步降低大文件 physical I/O | loop sectors written/edit | Fig. 9(b) |
论文中的跨切换普通 fd、mmap lazy switch 和并发 data-path 协议不属于当前 DeltaFS
v2 支持边界，不能列为当前 E0 门禁，也不能用偶然成功的结果声明支持。

## 5. 实验环境

### 5.1 两档环境

**论文对齐档**：尽量使用 96 physical cores/760 GiB RAM/400 GB NVMe，Firecracker guest 为 4 vCPU/8 GB、Linux 6.8。

**机制验证档**：任何支持 XFS reflink 的裸机或 KVM VM，至少 4 vCPU、8 GB RAM、100 GB 独占 NVMe 空间。WSL、Docker Desktop overlay2 或网络盘不适合作为物理 I/O 结论的最终环境。

所有最终结果必须注明在哪一档运行。若不是论文同型号硬件，绝对微秒数只做参考，主要判断机制趋势。

### 5.2 CPU、NUMA 和系统噪声控制

1. 把 guest 固定为 4 vCPU；记录 vCPU 到 host CPU 的映射。
2. benchmark 主线程固定到一个物理核，避免 SMT sibling 上运行其他任务。
3. 将 guest memory 绑定到与 vCPU 同一 NUMA node。
4. 固定 CPU governor 为 `performance`；记录 turbo 是否开启，不在不同 run 间改变。
5. 停止自动更新、周期性 trim、文件索引、监控采集等磁盘干扰。
6. 每个 run 同时记录 load average、CPU frequency、context switches、major/minor faults。
7. 时间源使用 `clock_gettime(CLOCK_MONOTONIC_RAW)`；不要用 shell `time` 测 0.1 ms 量级 ioctl。

### 5.3 四种 E2/E3 backing filesystem 配置

在同一块 NVMe 上创建四个相同大小的 loopback image，每个 image 独占一个 loop device：

| ID | 格式 | 关键参数 | 用途 |
|---|---|---|---|
| `ext4` | ext4 | 固定 4 KiB block | ext4 基线 |
| `xfs_noreflink` | XFS | `mkfs.xfs -m reflink=0` | 分离 XFS metadata 效果 |
| `xfs_reflink` | XFS | `mkfs.xfs -m reflink=1` | DeltaBox 目标配置 |
| `f2fs` | F2FS | `mkfs.f2fs` | log-structured filesystem 基线 |

要求：

- 四个 image 大小一致，文件系统创建参数写入 manifest。
- 每轮实验都从新格式化或从同一个干净 image snapshot 开始。
- upperdir 与 workdir 必须位于同一 backing filesystem。
- 四种配置的 DeltaFS 层拓扑、数据集和操作顺序完全一致。
- 测物理 I/O 时一次只挂载和运行一个配置。
- 不把宿主文件系统的写计入客体 loop device 指标。

### 5.4 建议目录布局

```text
/mnt/deltafs-bench/
  ext4/
    base/
    layers/0000/
    work/0000/
    view/
  xfs_noreflink/
    ...
  xfs_reflink/
    ...
  f2fs/
    ...
```

实际 ioctl 和 mount 参数取决于现有实现。开始测试前实现一个极薄的 `deltafsctl` 适配器，至少提供：

```text
deltafsctl mount --lower <path> --upper <path> --work <path> --target <path>
deltafsctl checkpoint --target <path> --new-upper <path> --new-work <path>
deltafsctl restore --target <path> --checkpoint-id <id>
deltafsctl inspect --target <path> --json
```

所有性能脚本只调用该适配器，避免把项目内部 ioctl 结构复制到多份脚本中。

## 6. 统一测量规范

### 6.1 延迟口径

同时记录两个层次，防止与论文口径混淆：

1. `ioctl_latency_ns`：进入 ioctl 前到 ioctl 返回后的 `CLOCK_MONOTONIC_RAW` 差值，对齐 Table 4 的 `Overlay ioctl switch`。
2. `operation_latency_ns`：包含 rename 旧 upper、创建新 upper/workdir、生成 layer config 和 ioctl 的用户可见总时间。

restore 也分别记录 ioctl 和完整 wrapper。每条样本记录 layer depth、目标 checkpoint depth、回退距离、cache 状态和返回码。

### 6.2 copy-up duplicated data 口径

论文把它定义为“重新物化到 upper 的文件数据”，不包括 journal 和 metadata。推荐主测量方法：

1. 每次编辑从一个空的新 upper 开始。
2. 编辑并 `fsync` 后，对新 upper 中目标文件执行 FIEMAP。
3. 将 **非 shared 的 data extent** 按文件系统 block 边界求和。
4. XFS reflink 中带 `FIEMAP_EXTENT_SHARED` 的 extent 不算 duplicated；被此次写入打破共享的 extent 计入。
5. ext4/XFS-no-reflink 中所有实际分配的数据 extent 计入。

辅证方法：

- 记录 `filefrag -v`/`xfs_io -c 'fiemap -v'` 输出。
- 在 DeltaFS copy-up/clone 路径增加 per-mount debug counter 或 tracepoint，区分 clone bytes、fallback copied bytes 和 dirty CoW blocks。
- 不使用普通 `du` 作为唯一证据；reflink 文件的 `st_blocks` 不能可靠区分共享物理块。

主输出为每次编辑的绝对 `copyup_bytes`；可另报：

```text
copyup_amplification = copyup_bytes / logical_bytes_changed
```

### 6.3 physical I/O 口径

对目标 loop device 读取 `/sys/block/loopN/stat`：

```text
physical_io_bytes = (sectors_written_after - sectors_written_before) * 512
```

Linux block stat 的 sector 单位按 512 bytes 解释；同时记录内核文档版本。单次样本流程：

1. 等待设备静默：连续 3 次、每次间隔 100 ms，written sectors 不再增长。
2. 读取 before counter。
3. 执行一次编辑。
4. 对目标文件 `fsync`，对挂载点 `syncfs`。
5. 再次等待 written sectors 稳定。
6. 读取 after counter。
7. 记录差值以及静默等待时间。

为估计 journal/background noise，每个配置每 20 个样本插入一次空操作 control，使用相同 `syncfs` 和等待流程。主结果保留原始值以对应论文“including journal and metadata”，同时在附录报告扣除同批次空操作中位数后的 sensitivity result。

### 6.4 cache 规范

cache state is not an E2 write-amplification dimension. The single schedule is
cache-neutral: it does not issue per-edit `POSIX_FADV_DONTNEED`, write
`/proc/sys/vm/drop_caches`, or produce a separate cold-cache sensitivity run.

### 6.5 结果记录格式

每个原始样本一行 JSONL 或 Parquet，至少包含：

```json
{
  "run_id": "2026-...",
  "git_commit": "...",
  "kernel_release": "6.8...",
  "fs_config": "xfs_reflink",
  "experiment": "write_amp",
  "trace_id": "...",
  "event_id": 17,
  "repo": "django/django",
  "instance_id": "...",
  "file_size_before": 32768,
  "logical_bytes_changed": 412,
  "size_bin": "32-64KiB",
  "copyup_bytes": 8192,
  "physical_io_bytes": 65536,
  "layer_depth": 12,
  "checkpoint_id": "...",
  "status": "ok"
}
```

不要只保存聚合后的 CSV。原始 stdout/stderr、dmesg、mountinfo、FIEMAP 和 block-stat 前后值也要按 `run_id/event_id` 关联保存。

## 7. 实验 E0：正确性与一致性门禁

性能测试前必须通过当前 E0；任何 correctness failure 都使后续性能结果无效。E0 是
滚动门禁，直接覆盖更新，不按 ABI 或开发阶段创建平行编号。

### 7.1 现行覆盖

Host 静态部分包括 v2 UABI layout、format-2 controller、ownership checkpoint callsite
和 fast workdir/active-view validation。QEMU acceptance 覆盖 native ABI 负向矩阵、
checkpoint derived chain、restore prefix/suffix、1/128/129 lower 边界、generation、
cache revalidation、故障注入、retired-state teardown、反复 mount/unmount、module unload、
sanitizer、lockdep 和 kmemleak 诊断。

公开入口为：

```bash
make -C tools/deltafs check-e0-layout
make -C tools/deltafs test-e0-controller
make -C tools/deltafs check-e0-checkpoints CHECKPOINT_MODE=auto
make -C tools/deltafs check-e0-fast-path
make -C tools/deltafs test-e0-acceptance \
  BACKING_ROOT=/path/on/filesystem-a \
  EXTRA_BACKING_ROOT=/path/on/filesystem-b
```

完整 QEMU 成功终止行为 `All DeltaFS E0 acceptance checks passed`。退出码 4 或 `SKIP`
不是完整 E0 证据。具体构建、启动、数据盘和取证命令见
[E0 测试交接](deltafs-e0-test-plan.md)和统一测试用户手册。

### 7.2 明确排除的场景

ioctl 前 workload 必须静止并释放所有非 control fd 引用。E0 不运行或认可以下场景：

- checkpoint 前打开的普通/目录 fd 跨切换继续访问；
- cwd、进程 root、writable mmap 或 io_uring 跨切换；
- checkpoint/restore 与 lookup、copy-up、写 syscall 或 writeback 并发。

这些能力需要先修改 DeltaFS 支持边界和 data-path 协议，再直接覆盖更新 E0。

## 8. 真实 SWE-Search 编辑轨迹准备（非独立实验编号）

### 8.1 路径 A：作者 artifact/trace

向作者索取：

- Table 2 的 24 个 SWE-bench Verified instance ID。
- 每个 MCTS node 的 parent、checkpoint、restore target。
- action command、patch、目标文件和时间顺序。
- Fig. 9 使用的 edit event 清单及每桶样本数。
- 环境镜像和 Conda environment digest。
- 原始 benchmark/analyze 脚本。

获得后保持原顺序回放，不重新随机采样。这是 exact artifact reproduction 的首选路径。

### 8.2 路径 B：独立冻结轨迹

拿不到作者 artifact 时：

1. 从 SWE-bench Verified 固定 24 个 instance，维持论文 4/4/10/6 类别比例。
2. 记录 instance ID、repo base commit、测试 patch 和环境 image digest。
3. 用固定版本 SWE-Search/Qwen3-Coder-30B 生成轨迹，或使用已存在的确定性 MCTS 轨迹。
4. 将每个文件系统变更正规化为可重放 event：前置 tree hash、命令/patch、后置 tree hash。
5. 从事件中抽取修改前大小在 1-256 KiB 的 regular file 编辑。
6. 按六个大小桶分层；每桶至少 100 个有效事件。若真实轨迹不足，扩大 instance 集，不用复制同一事件凑数。
7. 同一事件在 E2/E3 的四个 FS 配置上使用相同顺序回放；E1 保持三配置切换延迟矩阵。

如果预算有限，最低可用规模为每桶 30 个独立事件，但必须报告置信区间，并标记为 pilot。

### 8.3 合成控制轨迹

真实 patch 的修改范围不同，不利于解释机制。增加一个不替代主实验的 synthetic control：

- 每个大小桶生成 100 个不可压缩 regular file。
- 固定随机种子。
- 每次只在一个随机 4 KiB 对齐 block 中修改 1 KiB，然后 fsync。
- 另做 dirty blocks = 1/2/4/8 的 sweep。

预期：无 reflink的 duplicated bytes 随 file size 增长；XFS+reflink 随 dirty block 数而非 file size 增长。

## 9. 实验 E1：层切换延迟

### 9.1 目标

复现 Table 4 的 DeltaFS ioctl switch component，并验证延迟是否随 request lower 数
增长。E1 不测 controller wrapper、dirty workload、cold cache 或随机分支；这些因素
需要单独实验，不能混入 ioctl latency。

### 9.2 测试矩阵

E1 对三种 backing filesystem 分别运行同一个固定 preset：

| 操作 | 固定 depth |
|---|---|
| checkpoint | source 1、2、4、8、16、32、64、127；request 为 source+1 |
| restore | source 128；target/request 1、2、4、8、16、32、64、128 |

每格 20 次 warm-up、200 次 measured、5 个 independent run。source depth 128 的
checkpoint 只做一次 `E2BIG` 用户态 preflight，不进入 latency 分布。

### 9.3 步骤

公开命令只接收空的 backing 目录和结果目录：

    sudo python3 tools/deltafs/bench/e1/run.py smoke BACKING_DIR OUT_DIR
    sudo python3 tools/deltafs/bench/e1/run.py run   BACKING_DIR OUT_DIR

runner 为每个 sample 创建独立 mount、自动构造 topology/request/generation、只计时一次
ioctl，然后执行 marker、lower immutable、fresh upper 和 generation oracle。所有固定
参数、raw schema、失败处理及完整 QEMU 命令以
[deltafs-e1-test-plan.md](deltafs-e1-test-plan.md) 为唯一权威定义。

### 9.4 统计

每个 filesystem/operation/request depth 报告 mean、median、p25/p75、p95、p99、
standard deviation 和两级 cluster-bootstrap CI95。只纳入
`status=ok && warmup=false`，不得静默删除 outlier；invalid 和 errno 单独汇总。

### 9.5 对齐判据

- 论文参考值：checkpoint 0.07 ms、fast restore 0.19 ms、slow restore 0.25 ms。
- 这些数字只作组件参考，不设置绝对延迟通过门槛。
- 计划样本数必须完整，oracle/invalid/failed 为 0，restore depth 128 成功，且
  checkpoint@128 只得到计划内 `E2BIG`。
- 结果只表述为 ioctl switch latency，不外推为完整 checkpoint/restore latency。

## 10. 实验 E2：copy-up 写放大和物理 I/O

E2 已实现为独立固定 preset benchmark。公开入口仅为：

```text
sudo python3 tools/deltafs/bench/e2/run.py smoke BACKING_DIR DEVICE_STAT OUT_DIR
sudo python3 tools/deltafs/bench/e2/run.py run BACKING_DIR DEVICE_STAT OUT_DIR
sudo python3 tools/deltafs/bench/e2/run.py depth-smoke BACKING_DIR DEVICE_STAT OUT_DIR
sudo python3 tools/deltafs/bench/e2/run.py depth-run BACKING_DIR DEVICE_STAT OUT_DIR
python3 tools/deltafs/bench/e2/analyze.py RESULTS_ROOT
```

runner 自动生成 immutable synthetic event、逐样本创建 fresh OverlayFS mount、采集
FIEMAP/sector counter/hash/no-op control，并保留失败现场。full run 的单次调用在一个
测试对象上串行执行 5 个 independent workload；分析器要求四个 filesystem 结果完整且
event hash 一致。接口、18 个合法 size/dirty-block cell、raw schema、
统计口径、path-depth 实验及完整 QEMU 命令以
[deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) 为唯一权威定义。

### 10.1 主矩阵

对每一个 trace event，在四个配置上执行：

1. 恢复相同的 pre-edit base tree。
2. 创建空的新 upper/workdir。
3. mount 或 checkpoint 到该 upper。
4. 在 checkpoint 和 precheck 后执行 `syncfs`，等待 loop device 静默并读取 block stat。
5. 执行原始 edit。
6. `fsync(target)` + `syncfs(mount)`，等待静默。
7. 读取 block stat。
8. 从新 upper FIEMAP 计算 non-shared data extent。
9. 验证 post-edit tree hash。
10. 保存原始证据并清理到下一个相同起点。

同一个 event 的四种 FS 结果必须通过 `event_id` 配对。配置运行顺序使用详细设计固定的
Latin square，且一次只运行一个配置，避免温度/设备后台行为与配置绑定或污染计数器。

### 10.2 主图

输出两幅 log-y 写放大图：

- x：`logical_bytes_changed`，即 4、8、16、32 KiB 逻辑写请求。
- y(a)：`copyup_bytes / logical_bytes_changed`。
- y(b)：`physical_io_bytes / logical_bytes_changed`。
- 每幅图按 ext4、XFS-no-reflink、XFS+reflink、F2FS 分为四个 panel。
- panel 内按 `file_size_before` 绘制六条序列，不能把不同文件大小隐藏在同一个
  请求大小聚合值中。
- cache state is absent from the event and artifact schemas; each exact
  filesystem/file-size/request-size cell is analyzed directly.
- 点表示 cell 中位数；固定 synthetic schedule 内的 event 不重复伪装为第二层独立
  随机样本。

同时输出 cache-neutral 的 amplification cell TSV，并保留原始字节统计和每桶 n、
p25、p50、p75、p95，方便审计放大率的分子。

### 10.3 机制分解

用两个预注册的 XFS 机制成对差分解释来源；F2FS 作为独立第四 panel 报告：

```text
XFS metadata benefit = median(ext4 physical I/O - XFS-no-reflink physical I/O)
reflink benefit      = median(XFS-no-reflink physical I/O - XFS-reflink physical I/O)
```

差分应先逐 event 计算，再聚合；不要直接相减两个独立 median。

对 copy-up 数据拟合：

```text
log2(copyup_bytes) = alpha + beta * log2(file_size_before)
```

分别报告四种 FS 的 `beta` 和 bootstrap CI。预期非 reflink 文件系统的 `beta` 接近 1，而 reflink 的 `beta` 显著更小。由于真实 edit 的 dirty range 不恒定，这一回归是趋势证据，不应强制 `beta=0`。

### 10.4 对齐判据

满足以下条件可称为复现 Fig. 9 的核心结论：

1. ext4 与 XFS-no-reflink 的 copy-up 曲线在各桶接近，且都随 file size 明显上升；F2FS 单独报告，不预注册相对大小方向。
2. XFS+reflink 的 copy-up 曲线低于两条 no-reflink 曲线，尤其在 64-256 KiB 桶差距扩大。
3. 1-8 KiB physical I/O 中 XFS 明显低于 ext4；论文参考为约 132 KiB -> 26 KiB。
4. 128-256 KiB physical I/O 中 XFS+reflink 明显低于 no-reflink；论文参考为约 315 KiB -> 141 KiB。
5. 所有对比使用相同 edit event，post-edit 内容完全一致。

若 exact trace 不可得，不要求逐点数值一致；应同时报告方向是否一致、效应量和 CI，而不是根据目测把图中点手工当作 ground truth。

### 10.5 父目录深度实验

独立的 `path_depth` preset 固定 4-KiB aligned write，使用 4-KiB 和
192-KiB 文件，并交叉目录深度 0、1、2、4、8、16。同一个 `case_id` 的六个
深度变体共享文件内容、payload 和 offset，父目录只存在于 checkpoint 后的冻结
generation-1 layer。每个成功样本必须证明 fresh upper 中恰好 materialize 了声明
数量的父目录。

主结果是同一 filesystem/case 相对 depth 0 的 corrected physical-I/O 配对增量及
bytes-per-directory slope；文件 FIEMAP `copyup_bytes` 是不应随深度变化的负对照。
分析单独报告 artifact correctness 和 `depth_hypothesis_supported`，不得因趋势不符合
预期而删除或重跑样本。完整 preset 数量、分析产物和 QEMU 命令以 E2 详细设计为准。

## 11. 实验 E3：时间维度的写延迟放大

E3 是独立的 temporal write-latency benchmark，详细契约见
[deltafs-e3-test-plan.md](deltafs-e3-test-plan.md)。它不属于 E2 的空间/物理 I/O
矩阵，也不把 E1 的单次 checkpoint/restore ioctl latency 混入主指标。

E3 在连续 generation 中执行 `single`、`burst4`、`multi16` 三类 workload，比较
直接 lower base 文件系统的 `lower_direct`、`upper_resident`、`first_touch` 和
`steady_after_copyup`，仅覆盖当前支持的
close/reopen 访问方式。主结果是用户可见 `open + pwrite + fsync + close` 延迟、按序列
配对的 `first_touch_ratio`，以及相对 `lower_direct` 的 `ratio_to_lower_direct` /
`overhead_ns` 和 generation/history-depth 趋势。checkpoint 只用于建立
下一代，计时区间外；checkpoint 自身仍由 E1 负责 ioctl latency。

E3 的原始序列、generation 计数、CPU/clock validity、SHA-256 oracle 和 QEMU 交接必须
按详细设计执行。原先计划中的 arbitrary rollback/depth sweep 不再占用 E3 编号：正确性
部分归入 E0，纯 ioctl 的 target-depth 变化归入 E1；需要时作为后续辅助实验另行命名。

## 12. SWE-bench 文件系统回放（未来工作）

在 DeltaCR 未完成时，把 Table 2 的 coupled C/R 拆成 DeltaFS-only replay：

1. 使用 4/4/10/6 的 24 条冻结轨迹，每条回放两次。
2. 每个 parent node 调用 DeltaFS checkpoint。
3. 每个 tree backtrack 调用 DeltaFS restore。
4. process state 不恢复；由回放 harness 驱动确定性 action，因此只验证 FS backend。
5. 按 Django/SymPy/Scientific/Tools-Small 报告 checkpoint 和 restore mean/p50/p95/p99。
6. 汇总时用原始 event pooling 得到 event-weighted average，对齐论文 Table 2 注脚。

结果标题必须写 **DeltaFS-only trajectory replay**，不能直接复制 Table 2 的 `DeltaBox` 列名。

建议增加以下纯文件系统 baseline：

- `shutil.copytree` checkpoint/restore。
- stock OverlayFS umount/remount（仅在能安全关闭 fd 的简化 harness 中）。
- dm-snapshot filesystem layer。

这些 baseline 有助于说明 DeltaFS 的贡献，但不等同于论文的 replay+cp、FC-Diff+dm、CRIU+cp、E2B(diff)，因为论文 baseline 同时恢复 process state。

## 13. 完整 DeltaBox 集成后的后续实验

DeltaCR、StateManager、GSD 和 NPD 可用后，再执行：

### 13.1 Table 2 coupled per-event latency

- 24 条轨迹，每条两次。
- baseline：replay+cp、FC-Diff+dm、CRIU+cp、self-hosted E2B(diff)、DeltaBox。
- checkpoint 与 restore 分开计时。
- standard checkpoint 的 API blocking 与 async CRIU completion 分开记录。
- 加权方式严格按 event pooling，不对四类均值做简单平均。

### 13.2 Fig. 6 30-iteration MCTS

- Qwen3-Coder-30B。
- 四类各运行 30 iteration 轨迹。
- 分母为各系统自己的 `LLM RTT + action work`。
- 直接调用 E2B pause/resume，不含 control-plane overhead。
- 目标趋势：DeltaBox 1.01-1.02x，E2B(diff) 1.30-1.93x。

### 13.3 Fig. 7 RL fan-out

- N = 1/4/16/64。
- source 触碰并保持 64 MiB，child 读取并校验。
- DeltaBox `fork_n`、CubeSandbox clone、E2B createSnapshot+create。
- N=64 的 E2B 按论文做 4 个顺序的 16-concurrency batch。
- Qwen2.5-7B、vLLM 256->512 tokens、LoRA rank 16、4 GPU。

这些实验主要衡量 process fork/CRIU 和整体 sandbox，不应作为当前 DeltaFS 代码的验收门槛。

## 14. 统计与可重复性

### 14.1 重复和随机化

- 微秒级 latency：100 warm-up + 至少 1,000 measured operations/组合。
- trace write amplification：每桶目标至少 100 个独立 event；最低 30。
- 每套完整实验至少 5 个独立 run，跨 run 重新初始化 filesystem image。
- 固定并公开 seed；在 run 内随机化配置顺序。
- 同时保留 per-event paired result。

### 14.2 汇总规则

- Table 4 对齐：报告 mean 为主，补充 p50/p95/p99/CI。
- Fig. 9 对齐：按论文使用 per-bin median；补充 IQR 和 bootstrap CI。
- Table 2 对齐：按 archetype pooling event，再给 event-weighted overall mean。
- 不删除 outlier；只可在预先定义的系统故障条件下标为 invalid，并公开原因和数量。

### 14.3 环境 manifest

每个 run 自动采集：

```text
git rev-parse HEAD
uname -a
zcat /proc/config.gz 或内核 config 路径
lscpu -e
numactl --hardware
lsblk -o NAME,MODEL,SERIAL,SIZE,ROTA,LOG-SEC,PHY-SEC
nvme list / smart-log
mount
findmnt -J
xfs_info / ext4 feature dump
losetup --list --json
cat /proc/cmdline
cpupower frequency-info
```

还要保存 DeltaFS module parameters、`deltafsctl inspect --json` 和 VM image SHA-256。

## 15. 建议的 benchmark 仓库结构

```text
bench/
  README.md
  config/
    paper-aligned.yaml
    local.yaml
  adapters/
    deltafsctl.c
  setup/
    build_kernel.sh
    create_images.sh
    mount_backend.sh
  traces/
    manifest.json
    events.jsonl
  correctness/
    test_basic.py
    test_open_fd.c
    test_mmap.c
    test_concurrent_copyup.c
  perf/
    switch_latency.c
    write_amp.py
    temporal_write_latency.py
    block_stats.py
    replay_swe.py
  analysis/
    validate_raw.py
    summarize_latency.py
    plot_fig9.py
  results/
    raw/
    manifests/
    figures/
    tables/
```

`create_images.sh` 等环境脚本可按项目习惯实现，但实际创建/格式化设备前必须校验 loop device 对应的是专用 image，不能对不明块设备运行 `mkfs`。

## 16. 执行顺序和里程碑

### M0：接口和环境冻结

- 固定 kernel/DeltaFS commit。
- 完成 `deltafsctl` 四个接口。
- 生成 E1 所需三种 backing image，以及 E2/E3 所需的第四个 F2FS image。
- 自动生成 manifest。

退出条件：E1 的三种 FS 与 E2/E3 的四种 FS 都能完成各自的 mount/checkpoint/restore/inspect。

### M1：正确性

- 完成 E0 ABI、checkpoint/restore、cache、深层栈、故障注入和 teardown。
- KASAN/LOCKDEP 压测。

退出条件：第 7.4 节全部通过。

### M2：微基准

- 执行 E1 层切换延迟。
- 执行 E3 temporal write-latency sweep。

退出条件：原始数据和统计脚本能从干净环境一键重跑，latency 口径完整。

### M3：写放大

- 准备真实 trace 和 synthetic control。
- 执行 E2，复刻 Fig. 9。

退出条件：四配置成对事件完全一致，图表和数据校验通过。

### M4：文件系统轨迹回放

- 执行 24 轨迹 x 2 replay。
- 报告 DeltaFS-only 分组结果。

退出条件：所有 rollback oracle 一致，按论文权重生成汇总表。

### M5：完整系统（后续）

- DeltaCR/StateManager 集成。
- Table 2、Fig. 6、Fig. 7。

## 17. 最终报告应包含的表和图

1. 环境对照表：论文 vs 本次实验。
2. 正确性矩阵和压力测试统计。
3. checkpoint/restore ioctl 与 wrapper latency 表。
4. E1 ioctl latency-vs-target-depth。
5. E3 latency-vs-generation、latency-vs-history-depth 和 latency amplification。
6. copy-up amplification vs logical write-request size。
7. physical-write amplification vs logical write-request size。
8. 每桶样本量/IQR/CI 表。
9. synthetic dirty-block sweep。
10. DeltaFS-only SWE-bench replay 分组表。
11. 与论文偏差分析：硬件、trace、内核、mount 参数、统计口径。

## 18. 复现结论的措辞规则

可以使用：

- “复现了 DeltaFS 在 XFS reflink 上降低大文件 copy-up 和物理 I/O 的趋势。”
- “在本机上测得 layer-switch ioctl 的均值/尾延迟如下，与论文 Table 4 的组件值比较……”
- “完成了 DeltaFS-only SWE-bench trajectory replay。”

避免使用：

- 只测 DeltaFS 后声称“复现 DeltaBox 1.86 ms restore”。
- 没有恢复 process state 却声称“复现 Table 2”。
- 用手工读图值代替原始 artifact，声称逐点精确一致。
- 把 XFS 的 metadata 收益全部归因于 reflink。

## 19. 最小验收清单

- [ ] Linux 6.8 和 DeltaFS commit、config 已冻结。
- [ ] E2/E3 的 ext4、XFS-no-reflink、XFS-reflink、F2FS 四配置可重复创建。
- [ ] E0 支持范围内的 correctness、故障注入和 teardown 全部通过。
- [ ] ioctl 和 wrapper latency 分开记录。
- [ ] physical I/O 取自专用 loop device 且包含 sync/静默协议。
- [ ] copy-up bytes 使用 FIEMAP/shared extent 或等价内核计数，不仅是 `du`。
- [ ] 真实 edit event 在四配置上成对一致。
- [ ] 六个大小桶样本数、median、IQR、CI 完整。
- [ ] Fig. 9 趋势和效应来源被正确解释。
- [ ] DeltaFS-only 与 coupled DeltaBox 结果明确区分。
- [ ] 原始数据、manifest、脚本、图表均可从固定 commit 重建。

## 20. 预计工期（不含文件系统开发）

| 阶段 | 人日 |
|---|---:|
| 环境、四种 E2/E3 image、适配器 | 1-2 |
| 正确性与并发压力测试 | 2-3 |
| 层切换/深度微基准 | 1-2 |
| 轨迹整理与 Fig. 9 写放大实验 | 3-5 |
| SWE-bench FS-only replay | 2-4 |
| 分析、复跑、报告 | 2-3 |
| 合计 | 11-19 |

若能获得作者 artifact 和镜像，轨迹整理时间可显著缩短；若没有 exact trace，独立生成并验证 SWE-Search 轨迹通常是最大的不确定项。

---

**来源**：Dong, Yunpeng, et al. “DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback.” arXiv:2605.22781v2, 8 Jun. 2026。重点依据 §4.1、§5、§6.1、§6.2.1、§6.3.1、§6.3.2、Table 2、Table 4、Fig. 6、Fig. 9。
