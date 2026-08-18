# DeltaFS 评估实验复现方案

## 1. 文档目标与复现边界

本文依据 Zotero 中的论文 **DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback**（Dong et al., arXiv:2605.22781v2, 2026）整理。目标是在 **DeltaFS 文件系统代码已经完成** 的前提下，复现论文中可以归因于 DeltaFS 的评估结论，并为后续与 DeltaCR/DeltaBox 集成预留接口。

本方案把实验分成三个层次：

1. **主线：DeltaFS 独立实验**
   - 热切换 checkpoint/rollback 的正确性。
   - OverlayFS 层切换延迟。
   - ext4、XFS、XFS+reflink 下的 copy-up 写放大。
   - ext4、XFS、XFS+reflink 下的物理 I/O。
2. **增强：论文未单列但复现必须具备的文件系统验证**
   - 任意历史点回滚、深层栈、打开文件、mmap、并发 copy-up、崩溃后一致性等。
3. **后续：需要 DeltaCR 或完整 DeltaBox 的耦合实验**
   - SWE-bench MCTS 每事件 checkpoint/restore。
   - 30 轮 MCTS 端到端开销。
   - RL fan-out 和 GPU occupation。

E2 和 E3 已拆成两个独立 benchmark。E2 的固定 switch-latency preset、当前
128-lower ABI 的 depth 口径和 QEMU 交接见
[deltafs-e2-test-plan.md](deltafs-e2-test-plan.md)；E3 的 event、FIEMAP/sector counter
采样和 QEMU 交接见 [deltafs-e3-test-plan.md](deltafs-e3-test-plan.md)。

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
| H5 | checkpoint 前打开的 fd 和 mmap 在 checkpoint 后首次写入时切到新 upper | correctness、首次写延迟 | §4.1.1 |
| H6 | 并发写同一 stale dentry 不会产生 stale upper、丢写或错误层引用 | stress failure count | §4.1.1 |

H5-H6 是设计关键点，论文没有单独给出评估图；它们应作为性能结果可信的前置门槛。

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

### 5.3 三种 backing filesystem 配置

在同一块 NVMe 上创建三个相同大小的 loopback image，每个 image 独占一个 loop device：

| ID | 格式 | 关键参数 | 用途 |
|---|---|---|---|
| `ext4_noreflink` | ext4 | 固定 4 KiB block | 无 reflink 基线 |
| `xfs_noreflink` | XFS | `mkfs.xfs -m reflink=0` | 分离 XFS metadata 效果 |
| `xfs_reflink` | XFS | `mkfs.xfs -m reflink=1` | DeltaBox 目标配置 |

要求：

- 三个 image 大小一致，文件系统创建参数写入 manifest。
- 每轮实验都从新格式化或从同一个干净 image snapshot 开始。
- upperdir 与 workdir 必须位于同一 backing filesystem。
- 三种配置的 DeltaFS 层拓扑、数据集和操作顺序完全一致。
- 测物理 I/O 时一次只挂载和运行一个配置。
- 不把宿主文件系统的写计入客体 loop device 指标。

### 5.4 建议目录布局

```text
/mnt/deltafs-bench/
  ext4_noreflink/
    base/
    layers/0000/
    work/0000/
    view/
  xfs_noreflink/
    ...
  xfs_reflink/
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

- 主实验采用 warm page cache，符合连续 agent 轨迹行为。
- 每个配置开始前执行相同的 warm-up 轨迹。
- 不在每个 edit 之间全局 `drop_caches`，因为它会引入额外 I/O 和非真实 agent 行为。
- 另做 cold-cache sensitivity run，每个大小桶至少 30 个样本，并单独作图。

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

性能测试前必须通过 E0；任何 correctness failure 都使后续性能结果无效。

### 7.1 状态 oracle

每个 checkpoint 保存 manifest，不复制文件数据作为被测机制的一部分。manifest 对 view 做递归遍历，记录：

- 相对路径与类型。
- regular file 的 SHA-256、size、allocated blocks。
- mode、uid、gid、mtime/ctime（按测试是否要求精确恢复分别比较）。
- symlink target。
- hardlink inode group。
- xattr、ACL。
- sparse extent map。

rollback 后重新生成 manifest，与目标 checkpoint 的 oracle 比较。

### 7.2 基础操作矩阵

对每类操作执行 `checkpoint -> mutate -> checkpoint -> mutate -> restore`：

- 新建、覆盖、append、pwrite、truncate、hole punch。
- unlink、rename、rename-over-existing、跨目录 rename。
- mkdir/rmdir、深目录。
- symlink、hardlink。
- chmod、chown、xattr、ACL。
- sparse file、1 B、4 KiB、1 MiB、256 MiB 文件。
- fsync、fdatasync、syncfs 后回滚。
- whiteout/opaque directory 语义。

覆盖回滚目标：parent、grandparent、非祖先分支、最新 checkpoint、root checkpoint。

### 7.3 打开文件与 mmap lazy switch

至少覆盖：

1. checkpoint 前 `open(O_RDWR)`，checkpoint 后通过旧 fd `pwrite`。
2. checkpoint 前 `mmap(MAP_SHARED)`，checkpoint 后写入并 `msync`。
3. checkpoint 前打开后 unlink，checkpoint 后继续写 fd。
4. checkpoint 前打开，checkpoint 后 rename pathname，再通过旧 fd 写。
5. 同一 inode 多 fd、多进程共享 fd。

检查旧只读层内容不变，新 upper 有正确 copy-up，rollback 后目标 generation 内容正确。

### 7.4 并发 copy-up 压测

对同一 stale inode，在 checkpoint 后用 2/4/8/16/32 个线程同时写不同 4 KiB 区域；每个并发度运行 1,000 轮。记录：

- EEXIST 次数和处理路径。
- 最终内容 hash。
- 是否出现 stale backing mount。
- kernel warning/oops/lockdep/KASAN 报告。

建议至少运行一次 KASAN+LOCKDEP debug kernel 的 1 小时压力测试，再用 release kernel 做性能实验。

### 7.5 通过条件

- 所有确定性测试 100% 通过。
- 并发压力测试无内容错误、死锁、内核告警和资源泄漏。
- `inspect` 返回的层顺序、checkpoint generation 和 oracle 一致。
- 失败注入后要么 checkpoint 原子成功，要么完整保留旧状态，不出现半切换。

## 8. 实验 E1：真实 SWE-Search 编辑轨迹准备

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
7. 同一事件在三个 FS 配置上使用相同顺序回放。

如果预算有限，最低可用规模为每桶 30 个独立事件，但必须报告置信区间，并标记为 pilot。

### 8.3 合成控制轨迹

真实 patch 的修改范围不同，不利于解释机制。增加一个不替代主实验的 synthetic control：

- 每个大小桶生成 100 个不可压缩 regular file。
- 固定随机种子。
- 每次只在一个随机 4 KiB 对齐 block 中修改 1 KiB，然后 fsync。
- 另做 dirty blocks = 1/2/4/8 的 sweep。

预期：无 reflink的 duplicated bytes 随 file size 增长；XFS+reflink 随 dirty block 数而非 file size 增长。

## 9. 实验 E2：层切换延迟

### 9.1 目标

复现 Table 4 的 DeltaFS ioctl switch component，并验证延迟是否随 request lower 数
增长。E2 不测 controller wrapper、dirty workload、cold cache 或随机分支；这些因素
需要单独实验，不能混入 ioctl latency。

### 9.2 测试矩阵

E2 对三种 backing filesystem 分别运行同一个固定 preset：

| 操作 | 固定 depth |
|---|---|
| checkpoint | source 1、2、4、8、16、32、64、127；request 为 source+1 |
| restore | source 128；target/request 1、2、4、8、16、32、64、128 |

每格 20 次 warm-up、200 次 measured、5 个 independent run。source depth 128 的
checkpoint 只做一次 `E2BIG` 用户态 preflight，不进入 latency 分布。

### 9.3 步骤

公开命令只接收空的 backing 目录和结果目录：

    sudo python3 tools/deltafs/bench/e2/run.py smoke BACKING_DIR OUT_DIR
    sudo python3 tools/deltafs/bench/e2/run.py run   BACKING_DIR OUT_DIR

runner 为每个 sample 创建独立 mount、自动构造 topology/request/generation、只计时一次
ioctl，然后执行 marker、lower immutable、fresh upper 和 generation oracle。所有固定
参数、raw schema、失败处理及完整 QEMU 命令以
[deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) 为唯一权威定义。

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

## 10. 实验 E3：copy-up 写放大和物理 I/O

E3 已实现为独立固定 preset benchmark。公开入口仅为：

```text
sudo python3 tools/deltafs/bench/e3/run.py smoke BACKING_DIR DEVICE_STAT OUT_DIR
sudo python3 tools/deltafs/bench/e3/run.py run RUN_INDEX BACKING_DIR DEVICE_STAT OUT_DIR
python3 tools/deltafs/bench/e3/analyze.py RESULTS_ROOT
```

runner 自动生成 immutable synthetic event、逐样本创建 fresh OverlayFS mount、采集
FIEMAP/sector counter/hash/no-op control，并保留失败现场。full run 用 `RUN_INDEX=1..5`
分片，按固定 Latin square 串行调度三种 filesystem；分析器要求 15 个 shard 完整且每个
run index 的三份 event hash 一致。接口、18 个合法 size/dirty-block cell、raw schema、
统计口径及完整 QEMU 命令以
[deltafs-e3-test-plan.md](deltafs-e3-test-plan.md) 为唯一权威定义。

### 10.1 主矩阵

对每一个 trace event，在三个配置上执行：

1. 恢复相同的 pre-edit base tree。
2. 创建空的新 upper/workdir。
3. mount 或 checkpoint 到该 upper。
4. 等待 loop device 静默并读取 block stat。
5. 执行原始 edit。
6. `fsync(target)` + `syncfs(mount)`，等待静默。
7. 读取 block stat。
8. 从新 upper FIEMAP 计算 non-shared data extent。
9. 验证 post-edit tree hash。
10. 保存原始证据并清理到下一个相同起点。

同一个 event 的三种 FS 结果必须通过 `event_id` 配对。配置运行顺序使用详细设计固定的
Latin square，且一次只运行一个配置，避免温度/设备后台行为与配置绑定或污染计数器。

### 10.2 主图

输出两幅 log-y 写放大图：

- x：`logical_bytes_changed`，即 4、8、16、32 KiB 逻辑写请求。
- y(a)：`copyup_bytes / logical_bytes_changed`。
- y(b)：`physical_io_bytes / logical_bytes_changed`。
- 每幅图按 ext4-no-reflink、XFS-no-reflink、XFS+reflink 分为三个 panel。
- panel 内按 `file_size_before` 绘制六条序列，不能把不同文件大小隐藏在同一个
  请求大小聚合值中。
- 现有 warm/cold 样本在完全相同的文件系统、文件大小和请求大小 cell 内合并，
  图中不显示缓存标签；缓存状态不是写放大维度。
- 点表示 cell 中位数；固定 synthetic schedule 内的 event 不重复伪装为第二层独立
  随机样本。

同时输出 cache-neutral 的 amplification cell TSV，并保留原始字节统计和每桶 n、
p25、p50、p75、p95，方便审计放大率的分子。

### 10.3 机制分解

用三个配置的成对差分解释来源：

```text
XFS metadata benefit = median(ext4 physical I/O - XFS-no-reflink physical I/O)
reflink benefit      = median(XFS-no-reflink physical I/O - XFS-reflink physical I/O)
```

差分应先逐 event 计算，再聚合；不要直接相减两个独立 median。

对 copy-up 数据拟合：

```text
log2(copyup_bytes) = alpha + beta * log2(file_size_before)
```

分别报告三种 FS 的 `beta` 和 bootstrap CI。预期 no-reflink 的 `beta` 接近 1，而 reflink 的 `beta` 显著更小。由于真实 edit 的 dirty range 不恒定，这一回归是趋势证据，不应强制 `beta=0`。

### 10.4 对齐判据

满足以下条件可称为复现 Fig. 9 的核心结论：

1. ext4-no-reflink 与 XFS-no-reflink 的 copy-up 曲线在各桶接近，且都随 file size 明显上升。
2. XFS+reflink 的 copy-up 曲线低于两条 no-reflink 曲线，尤其在 64-256 KiB 桶差距扩大。
3. 1-8 KiB physical I/O 中 XFS 明显低于 ext4；论文参考为约 132 KiB -> 26 KiB。
4. 128-256 KiB physical I/O 中 XFS+reflink 明显低于 no-reflink；论文参考为约 315 KiB -> 141 KiB。
5. 所有对比使用相同 edit event，post-edit 内容完全一致。

若 exact trace 不可得，不要求逐点数值一致；应同时报告方向是否一致、效应量和 CI，而不是根据目测把图中点手工当作 ground truth。

## 11. 实验 E4：任意历史点回滚与层深扩展

这是对 R3 “O(1) arbitrary rollback”的专门验证，论文没有单列图，但对机制主张非常重要。

1. 生成深度 1/2/4/8/16/32/64/128 的线性 checkpoint chain。
2. 每层创建、修改、删除不同文件，保存 oracle。
3. 从最新状态随机回滚到任意历史点，共 10,000 次。
4. 每次验证 tree manifest，并记录回滚距离、目标深度、ioctl latency。
5. 额外构建分支树，在 A 分支编辑后回 root，再生成 B 分支，验证 A/B 不互相可见。
6. 记录 mount 数、dentry/inode/slab、module allocation，验证旧 layer array 在 RCU grace period 后回收。

输出 latency-vs-distance 和 memory-vs-live-checkpoints。若 rollback latency 随距离增加，定位是 layer config 构建、cache invalidation，还是实际数据操作。

## 12. 实验 E5：SWE-bench 文件系统回放

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
- 生成三种 backing image。
- 自动生成 manifest。

退出条件：三种 FS 都能完成 mount/checkpoint/restore/inspect。

### M1：正确性

- 完成 E0 基础、open-fd、mmap、并发 copy-up、分支回滚。
- KASAN/LOCKDEP 压测。

退出条件：第 7.5 节全部通过。

### M2：微基准

- 执行 E2 层切换延迟。
- 执行 E4 arbitrary rollback/depth sweep。

退出条件：原始数据和统计脚本能从干净环境一键重跑，latency 口径完整。

### M3：写放大

- 准备真实 trace 和 synthetic control。
- 执行 E3，复刻 Fig. 9。

退出条件：三配置成对事件完全一致，图表和数据校验通过。

### M4：文件系统轨迹回放

- 执行 E5 的 24 轨迹 x 2 replay。
- 报告 DeltaFS-only 分组结果。

退出条件：所有 rollback oracle 一致，按论文权重生成汇总表。

### M5：完整系统（后续）

- DeltaCR/StateManager 集成。
- Table 2、Fig. 6、Fig. 7。

## 17. 最终报告应包含的表和图

1. 环境对照表：论文 vs 本次实验。
2. 正确性矩阵和压力测试统计。
3. checkpoint/restore ioctl 与 wrapper latency 表。
4. latency-vs-layer-depth、latency-vs-rollback-distance。
5. copy-up amplification vs logical write-request size。
6. physical-write amplification vs logical write-request size。
7. 每桶样本量/IQR/CI 表。
8. synthetic dirty-block sweep。
9. DeltaFS-only SWE-bench replay 分组表。
10. 与论文偏差分析：硬件、trace、内核、mount 参数、统计口径。

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
- [ ] ext4-no-reflink、XFS-no-reflink、XFS-reflink 三配置可重复创建。
- [ ] correctness/open-fd/mmap/concurrency 全部通过。
- [ ] ioctl 和 wrapper latency 分开记录。
- [ ] physical I/O 取自专用 loop device 且包含 sync/静默协议。
- [ ] copy-up bytes 使用 FIEMAP/shared extent 或等价内核计数，不仅是 `du`。
- [ ] 真实 edit event 在三配置上成对一致。
- [ ] 六个大小桶样本数、median、IQR、CI 完整。
- [ ] Fig. 9 趋势和效应来源被正确解释。
- [ ] DeltaFS-only 与 coupled DeltaBox 结果明确区分。
- [ ] 原始数据、manifest、脚本、图表均可从固定 commit 重建。

## 20. 预计工期（不含文件系统开发）

| 阶段 | 人日 |
|---|---:|
| 环境、三种 image、适配器 | 1-2 |
| 正确性与并发压力测试 | 2-3 |
| 层切换/深度微基准 | 1-2 |
| 轨迹整理与 Fig. 9 写放大实验 | 3-5 |
| SWE-bench FS-only replay | 2-4 |
| 分析、复跑、报告 | 2-3 |
| 合计 | 11-19 |

若能获得作者 artifact 和镜像，轨迹整理时间可显著缩短；若没有 exact trace，独立生成并验证 SWE-Search 轨迹通常是最大的不确定项。

---

**来源**：Dong, Yunpeng, et al. “DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback.” arXiv:2605.22781v2, 8 Jun. 2026。重点依据 §4.1、§5、§6.1、§6.2.1、§6.3.1、§6.3.2、Table 2、Table 4、Fig. 6、Fig. 9。
