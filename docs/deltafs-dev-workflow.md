# deltafs 开发迭代工作流（模块热替换）

目标：日常改 `fs/overlayfs/`、`include/uapi/linux/deltafs.h`、`tools/deltafs/` 时，
**不重编全量内核、不 `modules_install`、不 `make install`、不重启**，一分钟一轮。

依据：`CONFIG_OVERLAY_FS=m`，内核侧改动全部落在 overlay 模块内；启动所需驱动
（`VIRTIO_PCI/VIRTIO_BLK/EXT4_FS/VIRTIO_NET`）全部内建；guest 里真正会用到的模块
只有 9p 三件套 + xfs + overlay 共 5 个 `.ko`。

配套脚本：`tools/deltafs/dev/`（host 侧 `host-build.sh`；guest 侧 `guest-init.sh` /
`guest-align.sh` / `guest-reload.sh`，guest 从 9p 共享直接运行）。

## 1. 两条循环

### 1.1 日常迭代（高频，不重启）

```bash
# host
./tools/deltafs/dev/host-build.sh module
#   = make -j$(nproc) M=fs/overlayfs modules
#   + make -C tools/deltafs v2-tools（UAPI 头变更自动触发 headers_install）

# guest（root）
bash /mnt/host/tools/deltafs/dev/guest-reload.sh
#   = umount 全部 overlay 挂载 → modprobe -r overlay → insmod 新 overlay.ko → dmesg
```

改了 `include/uapi/linux/deltafs.h` 时同样走这条循环（工具由 `module` 命令顺带重编）。

### 1.2 release 对齐（低频，一次重启）

只在 release 字符串（`include/config/kernel.release`，当前 `6.8.0-deltafs+`）变化时
需要：首次对齐、改 `.config` / `CONFIG_LOCALVERSION` / 内核核心代码。

```bash
# host：bzImage + 5 个最小模块 + 工具（不编全量 6258 个模块）
./tools/deltafs/dev/host-build.sh release

# guest（root）：装最小模块集 + make install + 引导重启
bash /mnt/host/tools/deltafs/dev/guest-align.sh
reboot

# guest（root）：开机后验证
uname -r                                # 期望 6.8.0-deltafs+
bash /mnt/host/tools/deltafs/dev/guest-init.sh   # 状态应显示“已对齐”
```

之后回到 1.1。

## 2. QEMU 启动参数

沿用 grub 盘启动即可；必须带 9p 共享（guest 脚本和产物都从它走）：

```bash
qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
  -drive if=virtio,format=qcow2,file="$ROOTFS" \
  -drive if=virtio,format=raw,file="$DATA1" \
  -drive if=virtio,format=raw,file="$DATA2" \
  -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,mount_tag=host,security_model=none
```

（可选提速：改用 `-kernel arch/x86/boot/bzImage -append 'root=/dev/vda1 rw console=ttyS0 ...'`
直启，内核变更就只是重启 QEMU，`guest-align.sh` 里 make install 一步可以跳过——
启动驱动全部内建，不需要 initrd。）

首次 9p 未挂载时在 guest 里：

```bash
mkdir -p /mnt/host
modprobe 9pnet_virtio; modprobe 9p
mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
```

之后每次开机 `guest-init.sh` 会自动挂载并报告对齐状态。

## 3. 挂载与测试

对齐 + reload 之后按既有文档执行，例如 v2 phase1
（`docs/deltafs_v2_phase1_test.md` §4 起）：数据盘、checkpoint/restore、
`tools/deltafs/deltafs_v2_*_test`。区别只是：**改模块后重跑测试前，先在 guest 执行
`guest-reload.sh`**。测试脚本自身应保证结束时清理 overlay 挂载，否则 reload 会列出
残留挂载并中止。

日志收集：`dmesg`（kmemleak 报告、WARN/panic 栈）；必要时
`cat /sys/kernel/debug/...`。复现失败时把 `guest-init.sh` 输出、reload 输出、
`dmesg | tail -50` 一起留存。

## 4. 不变量（防止再次错位）

1. **冻结 `CONFIG_LOCALVERSION="-deltafs"`**：只在上里程碑时改，且三个 fragment
   （`config/{debug,kasan,release}/config.v1`）一起改；每次变更 = 一轮 1.2。
2. **保持 `CONFIG_LOCALVERSION_AUTO` 关闭**：否则每个 commit/dirty 状态都会改
   release 串，模块天天 `Invalid module format`。
3. **所有 make 只在 host 跑**：guest 只做文件复制和 `make install`。guest 里跑
   kbuild 在缺 git 等情况下会改写共享树的 `include/config/kernel.release`
   （丢掉 `+` 后缀），污染 host 后续构建。`guest-align.sh` 对此有前后校验。
4. **模块只从同一棵树构建**：vermagic 与符号 CRC 与运行内核天然一致；
   `+` 后缀来自 `scripts/setlocalversion`（git 仓库恒定追加），稳定无碍。

## 5. 故障排查

| 症状 | 原因 | 处置 |
|---|---|---|
| `insmod: Invalid module format` | vermagic 与 `uname -r` 不一致 | `guest-init.sh` 看两侧 release；不一致走 `guest-align.sh`；一致则 host 重跑 `host-build.sh module`（stale 产物） |
| `rmmod: overlay is in use` | 有 overlay 挂载或进程引用 | `guest-reload.sh` 会列出残留挂载；`lsof`/`ps` 找占用进程 |
| 重启后 `uname -r` 还是旧串 | grub 默认项没指向新内核 | 检查 `/etc/default/grub` 的 `GRUB_DEFAULT` 与 `/boot` 内容 |
| `mount -t 9p` 失败 | 9p 模块缺失或 QEMU 没带 `-virtfs` | 确认 `-virtfs` 参数；`modprobe 9pnet_virtio && modprobe 9p` |
| 某测试 `modprobe: FATAL: Module X not found` | 最小集没含 X | 把对应 `.ko` 路径加进 `host-build.sh` / `guest-align.sh` 的 `MINIMAL_MODULES` |
| `make install` 报 initramfs 错误 | 极少数发行版要求完整模块树 | 在 host 全量 `make modules_install INSTALL_MOD_PATH=...` 后同步，或改用 `-kernel` 直启 |
| 树的 `kernel.release` 莫名变样 | guest 里跑过 kbuild 且无 git | host 重跑 `host-build.sh release` 恢复，再走 1.2 |

## 6. 首次启用（从旧全量流程迁移）

```bash
# host
./tools/deltafs/dev/host-build.sh release

# guest（root）
bash /mnt/host/tools/deltafs/dev/guest-align.sh   # 若 9p 未挂载先看 §2 首次挂载
reboot
# …开机后
bash /mnt/host/tools/deltafs/dev/guest-init.sh    # 确认“已对齐”
bash /mnt/host/tools/deltafs/dev/guest-reload.sh  # 确认能加载
```

一次性成本 = 一次 bzImage 重编 + 一次重启；之后全部走 1.1。
