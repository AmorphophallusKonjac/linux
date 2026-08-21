# DeltaFS E1/E2/E3 bench 索引

完整用户流程见中文 [E0-E3 测试用户手册](deltafs-testing-guide-zh.md) 和英文
[E0-E3 Testing Guide](deltafs-testing-guide-en.md)。性能测试前必须通过
[E0 正确性门禁](deltafs-e0-test-plan.md)。

> 当前状态：E1 已使用 v2 checkpoint/restore UAPI；E2 的每个 sample 也会先执行一次
> v2 checkpoint，再测冻结 generation-1 upper 到 fresh generation-2 upper 的 copy-up。
> 现行功能前置门禁统一为 E0；E0 内部验证当前 v2 ABI，不再运行 P1--P7 v1 测试。

E1、E2 和 E3 是三个互不依赖的 benchmark。它们有各自的 runner、二进制、分析器、
测试和输出目录，不通过统一入口选择实验。

| bench | 研究问题 | 设计文档 | 计划实现目录 |
|---|---|---|---|
| E1 | DeltaFS checkpoint/restore ioctl 延迟 | [deltafs-e1-test-plan.md](deltafs-e1-test-plan.md) | `tools/deltafs/bench/e1/` |
| E2 | reflink 对 copy-up/物理 I/O 的影响，以及深层父目录的物理写放大 | [deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) | `tools/deltafs/bench/e2/` |
| E3 | 连续 generation 中 copy-up 的用户可见写延迟、延迟放大，以及相对直接 lower base 的 DeltaFS overhead | [deltafs-e3-test-plan.md](deltafs-e3-test-plan.md) | `tools/deltafs/bench/e3/` |

E1、E2 和 E3 均有独立实现。E1 按 v2 checkpoint/restore request、`keep_bottom` 和 prefix
语义只测 ioctl latency，不用 `deltafsctl` 整条命令耗时冒充该指标。E2 使用固定
`smoke|run|depth-smoke|depth-run` preset，自动管理 synthetic event、fresh sample mount、generation-1 upper
冻结、v2 checkpoint、FIEMAP、block-stat、no-op control 和工件。E2 的 checkpoint 在
计数区间外，只是建立被测 v2 layer stack，不是 E1 latency 样本。E3 使用连续 generation
和独立 workload，测用户可见写延迟；每个事件还在同一 backing filesystem 的独立 `base/`
树上执行 `lower_direct` 控制，用于计算 DeltaFS 相对直接文件系统路径的额外延迟。E3
不复用 E1 的 ioctl 计时窗口或 E2 的字节/I/O schema。三者当前权威契约及 QEMU 交接见
各自详细设计文档。

E2/E3 的 backing 配置固定为 `ext4`、`xfs_noreflink`、`xfs_reflink` 和 `f2fs`；只有
XFS 配置使用 `noreflink`/`reflink` 后缀。
