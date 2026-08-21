# deltafs 开发迭代脚本

完整流程说明见 `docs/deltafs-dev-workflow.md`。速查：

| 脚本 | 在哪跑 | 什么时候跑 |
|---|---|---|
| `minimal-modules-test.sh` | host | 静态检查 host/guest 的 15 个最小模块及依赖闭包；`release` 会自动运行 |
| `host-build.sh release` | host | 每个新 release 串一次（首次、改 .config/localversion/内核核心后） |
| `host-build.sh module` | host | 日常改 `fs/overlayfs`、`include/uapi/linux/deltafs.h` 后 |
| `guest-init.sh` | guest（root） | 每次开机后；显示对齐状态 |
| `guest-align.sh` | guest（root） | host 侧 `release` 构建之后；需要一次 reboot |
| `guest-reload.sh` | guest（root） | host 侧 `module` 构建之后；热替换 overlay.ko，不重启 |

guest 侧统一通过 9p 共享执行（`-virtfs ... mount_tag=host`，挂载在 `/mnt/host`）：

```bash
bash /mnt/host/tools/deltafs/dev/guest-<xxx>.sh
```

所有脚本幂等，失败修完问题后直接重跑即可。

核心不变量：

- `CONFIG_LOCALVERSION` 冻结为 `-deltafs`，`CONFIG_LOCALVERSION_AUTO` 保持关闭；
- 所有 make 只在 host 跑，guest 只做文件操作和 `make install`；
- overlay.ko 的 vermagic 必须与运行内核 `uname -r` 一致（同一棵树构建天然满足）。
