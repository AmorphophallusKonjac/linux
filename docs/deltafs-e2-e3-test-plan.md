# DeltaFS E2/E3 bench 索引

> 当前状态：E2 已使用 v2 checkpoint/restore UAPI；E3 的每个 sample 也会先执行一次
> v2 checkpoint，再测冻结 generation-1 upper 到 fresh generation-2 upper 的 copy-up。
> 现行功能前置门禁统一为 v2 layout、controller 和 acceptance，不再运行 P1--P7 v1
> 测试。

E2 和 E3 现在是两个互不依赖的 benchmark。它们有各自的 runner、二进制、分析器、
测试和输出目录，不再通过统一入口选择实验。

| bench | 研究问题 | 设计文档 | 计划实现目录 |
|---|---|---|---|
| E2 | DeltaFS checkpoint/restore ioctl 延迟 | [deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) | `tools/deltafs/bench/e2/` |
| E3 | reflink 对 copy-up 和物理 I/O 的影响 | [deltafs-e3-test-plan.md](deltafs-e3-test-plan.md) | `tools/deltafs/bench/e3/` |

E2 和 E3 均有独立实现。E2 按 v2 checkpoint/restore request、`keep_bottom` 和 prefix
语义只测 ioctl latency，不用 `deltafsctl` 整条命令耗时冒充该指标。E3 使用固定
`smoke|run` preset，自动管理 synthetic event、fresh sample mount、generation-1 upper
冻结、v2 checkpoint、FIEMAP、block-stat、no-op control 和工件。E3 的 checkpoint 在
计数区间外，只是建立被测 v2 layer stack，不是 E2 latency 样本；两者当前权威契约及
QEMU 交接见各自详细设计文档。
