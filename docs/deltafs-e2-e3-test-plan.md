# DeltaFS E2/E3 bench 索引

> 当前状态：E2 仍实现 v1 switch request，不能在 v2 上构建或运行；其详细文档仅保留
> 为移植输入，不是当前测试交接。E3 不调用 switch UAPI，可继续在 v2 上使用。现行
> 功能前置门禁统一为 v2 layout、controller 和 acceptance，不再运行 P1--P7 v1 测试。

E2 和 E3 现在是两个互不依赖的 benchmark。它们有各自的 runner、二进制、分析器、
测试和输出目录，不再通过统一入口选择实验。

| bench | 研究问题 | 设计文档 | 计划实现目录 |
|---|---|---|---|
| E2 | DeltaFS checkpoint/restore ioctl 延迟 | [deltafs-e2-test-plan.md](deltafs-e2-test-plan.md) | `tools/deltafs/bench/e2/` |
| E3 | reflink 对 copy-up 和物理 I/O 的影响 | [deltafs-e3-test-plan.md](deltafs-e3-test-plan.md) | `tools/deltafs/bench/e3/` |

E2 和 E3 均有独立实现。E2 必须先按 v2 checkpoint/restore request、`keep_bottom` 和
prefix 语义完成移植，届时不能用 `deltafsctl` 整条命令耗时冒充 ioctl latency。E3 使用
固定 `smoke|run` preset，自动管理 synthetic event、fresh sample mount、FIEMAP、
block-stat、no-op control 和工件；其当前权威契约及 QEMU 交接见详细设计文档。
