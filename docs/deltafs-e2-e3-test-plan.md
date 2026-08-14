# DeltaFS v1 E2/E3 bench 索引

E2 和 E3 现在是两个互不依赖的 benchmark。它们有各自的 runner、二进制、分析器、
测试和输出目录，不再通过统一入口选择实验。

| bench | 研究问题 | 设计文档 | 计划实现目录 |
|---|---|---|---|
| E2 | DeltaFS checkpoint/restore ioctl 延迟 | [deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) | `tools/deltafs/bench/e2/` |
| E3 | reflink 对 copy-up 和物理 I/O 的影响 | [deltafs-e3-test-plan.md](deltafs-e3-test-plan.md) | `tools/deltafs/bench/e3/` |

E2 和 E3 均已按独立设计实现。E2 直接测量 switch ioctl，不用 `deltafsctl` 整条命令
耗时冒充 benchmark 结果；E3 使用与 E2 相同的固定 `smoke|run` preset 风格，自动管理
synthetic event、fresh sample mount、FIEMAP、block-stat、no-op control 和工件。E3 的
唯一权威契约及 QEMU 交接见其详细设计文档。
