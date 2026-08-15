# DeltaFS v2 开发计划

> 状态：工作包一代码与静态门禁完成，QEMU 证据待用户；工作包二、三待实现
>
> 设计依据：`docs/deltafs_v2_design.md`
>
> v2 暂不实现 layer-stack RCU 或 layer resource 复用。全部实现和 QEMU/KVM 验收
> 完成前，本计划保留在 `docs/plans/`。

## 1. 工作包一：UABI 与内核切换

1. 将 `include/uapi/linux/deltafs.h` 拆成 checkpoint/restore 两个 v2 request：
   checkpoint 只包含 fresh upper/work；restore 包含 `keep_bottom` 和
   `[upper, work, lower prefix...]`。
2. 固定 checkpoint 64 bytes、restore 584 bytes，并添加 host-side layout assertions。
3. 为 initial、active、build 和 retired layer 保存 backing source path，保证
   checkpoint 前 rename 后仍能派生 frozen upper。
4. checkpoint 从 current state 构造 `[new upper, old upper, old lowers...]`。
5. restore 从 fd prefix 和 current bottom suffix 构造 target stack。
6. 删除 ancestor/descendant 和 retired overlap 扫描；保留 fd、same-sb、feature、
   count、empty 和 in-use 等 builder 必需检查。
7. 复用 v1 target-state builder、`delta_lock` commit、generation cache 和
   retired-until-unmount 生命周期。
8. 更新 failure injection，使每个新增 source-path 和 command-specific owner 获取点
   都能验证 unwind。

完成条件：kernel、module 和 native ioctl helper 编译通过；成功切换 generation 恰好
加一，任意 commit 前错误不改变 active state 或 retired list。

实现记录（2026-08-14）：v2 UABI、source-path owner、checkpoint/current snapshot、
restore bottom-suffix snapshot、target builder、final revalidation、layout test 和 native
helper 已完成；kernel/module、`W=1`、sparse、userspace `-Werror`、strict checkpatch 和
production checkpoint callsite gate 已通过；`deltafs_v2_phase1_test.sh` 已将 guest
smoke、negative matrix、checkpoint/restore 和 fault unwind 编排为一个入口。运行态
语义按 `docs/deltafs_v2_phase1_test.md` 在 QEMU debug guest 中取证。

## 2. 工作包二：Controller 与测试

1. 将 state/transaction format 升级为 2，并拒绝 v1 state。
2. checkpoint 继续执行 durable transaction、fresh branch 创建和 old upper rename，
   但 ioctl 只打开并传递 fresh upper/work。
3. restore 计算 active/target 最长公共后缀，传递 `keep_bottom` 和 target prefix fd。
4. transaction 持久化 full target、prefix、keep_bottom 和 generation。
5. 更新同步失败补偿、post-ioctl fail-stop、generation helper 和 crash fixtures。
6. 不创建 v2 P1-P7 阶段脚本，只维护：
   `check-v2-layout`、`test-v2-controller`、native `deltafs_v2_ioctl_test` 和
   guest `deltafs_v2_acceptance_test.sh`。
7. acceptance 覆盖 UABI 负向矩阵、checkpoint/restore、cache、1..128 lower、
   129 `E2BIG`、故障注入、反复切换、unmount/module unload 和 kmemleak。

完成条件：host controller/layout tests 通过；QEMU/KVM debug guest 输出
`All DeltaFS v2 acceptance checks passed`。

## 3. 工作包三：最终验证与交接

开发机只执行静态验证：

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make M=fs/overlayfs W=1
make C=2 CHECK=sparse M=fs/overlayfs
make -C tools/deltafs clean v2-tools
make -C tools/deltafs check-v2-layout
make -C tools/deltafs test-v2-controller
for f in tools/deltafs/*.sh; do bash -n "$f"; done
```

同时运行 `scripts/checkpatch.pl --no-tree --strict`。不得在开发机加载 module、mount
DeltaFS 或运行 guest acceptance。

完整 kernel build、QEMU boot、mount、acceptance 命令、预期输出和日志采集维护在
`docs/deltafs_v2_design.md` 第 13 节。接口或测试入口变化时先同步该节。

全部静态门禁和用户提供的 QEMU 原始证据通过后，才将本计划移动到
`docs/plans/done/`。
