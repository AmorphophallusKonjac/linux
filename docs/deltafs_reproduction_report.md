# DeltaFS 初步复现研究报告（Linux 6.8 clean-room 方案）

> 状态：初步设计，2026-07-17
>
> 范围：仅讨论 DeltaFS 的文件系统状态管理。本文不讨论任何进程/内存快照、恢复或模板机制。
>
> 代码基线：本仓库 Linux 6.8.0，分支 `deltafs/6.8`。

## 0. 结论先行

DeltaFS 不是一个新的磁盘文件系统，而是对 Linux OverlayFS 的运行时重配置扩展。论文披露的核心实现可以归纳为三件事：

1. checkpoint 时在用户态用 `rename(2)` 保存当前 upper，内核不卸载 OverlayFS，直接把旧 upper 降为最上层只读 lower，并安装 fresh upper；restore 则切换到目标层栈。
2. 每个文件系统和 inode 都带 generation。缓存对象属于旧 generation 时，在下一次写入前重新解析并 copy-up 到当前 upper，从而保护已冻结层。
3. backing filesystem 使用 XFS reflink。OverlayFS 原本就会优先调用 `vfs_clone_file_range()`，因此同一 XFS superblock 内的 copy-up 可以共享 extent，实际写入时才按块 CoW。

本仓库的 Linux 6.8 代码提供了大部分基础设施，但不能通过“给 `struct ovl_fs` 加一个数组指针并交换”完成复现。至少还要处理：

- `ovl_entry` 中指向旧 `ovl_layer[]` 的内部指针；
- `ovl_inode.__upperdentry` 与对应 `vfsmount` 的一致配对；
- root inode、正/负 dentry、inode alias 和目录缓存；
- 已打开普通文件和目录中缓存的真实 `struct file`；
- 旧 layer array、private mount clone、trap、dentry/inode 的引用生命周期；
- writable `MAP_SHARED`。Linux 6.8 的 `backing_file_mmap()` 会把 VMA 直接改绑到真实 XFS file，之后的写 fault 不再经过 OverlayFS，这是完整复现的独立高风险门槛。

因此建议采用两级目标：

- **MVP**：先在“所有写入者已静止、无 writable shared mmap、关闭复杂 OverlayFS 特性”的约束下完成热切层、任意历史点恢复、缓存重校验和 XFS reflink 验证。
- **论文对齐版本**：再完成旧 fd 的 generation-based lazy switch、并发 copy-up、RCU/refcount 生命周期、目录 fd 和 writable mmap；全部通过后才称为 DeltaFS 复现，而不是概念验证。

作者公开项目页明确说明 kernel patch、用户态 controller 和 benchmark 尚未公开。因此本方案是依据论文与 Linux 6.8 源码所做的 clean-room 初步复现，并非原作者代码的逐行还原。

## 1. 研究材料与证据等级

### 1.1 Zotero 中的论文

通过本地 Zotero 检索并通读了全文：

- Yunpeng Dong, Jingkai He, Shiqi Liu, Yuze Hou, Dong Du, Zhonghu Xu, Si Yu, Baochuan Yang, Yubin Xia, and Haibo Chen. *DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback*. arXiv:2605.22781v2, 2026-06-08 [1]。
- Zotero item key：`K3HZMWBN`。
- PDF attachment key：`3SNFP752`，Zotero 索引完整度为 15/15 页。
- Zotero 导出的 BibTeX key：`dongDeltaBoxScalingStateful2026`。
- DOI：`10.48550/arXiv.2605.22781`。

作者官方仓库 `dongyunpeng-sjtu/deltabox` 的 `main` 分支在检查时只有论文、图片和项目网页。其 README 的 “Code Release” 明确表示 kernel patch、controller 和 benchmark scripts 暂未公开 [2]。该仓库网页中的部分作者列表和性能数字对应较早版本，本文以 Zotero 中的 arXiv v2 正文为准，仅用官方仓库确认代码可用性。

### 1.2 证据标签

为避免把推断写成论文事实，本文使用以下标签：

- **[P] 论文事实**：论文正文明确描述。
- **[K] 代码事实**：当前 Linux 6.8 工作树可直接验证。
- **[D] 复现设计**：为补齐论文未公开细节而提出的工程方案，需通过实现和测试验证。

论文没有公开 ioctl ABI、四个修改文件的名称、字段布局、具体锁名、完整 mmap hook 或 controller 数据格式。本文不会将这些内容伪装为原实现。

## 2. 当前仓库基线审计

### 2.1 版本和工作树

- 顶层 `Makefile` 为 `VERSION = 6`、`PATCHLEVEL = 8`、`SUBLEVEL = 0`，即 Linux 6.8.0。
- 当前分支为 `deltafs/6.8`。
- 当前 HEAD 为 `772478ef4934902388c30e75bbbe4ab2fd99170c`。将 HEAD 下 `fs/overlayfs/` 的 16 个 Git blob SHA 与 `torvalds/linux` tag `v6.8` 逐一比较，16/16 完全一致；因此 HEAD 的 OverlayFS 可确认是上游 v6.8 原版，而不只是版本号相同。
- 工作树已有三处未提交修改：
  - `fs/overlayfs/namei.c`
  - `fs/overlayfs/ovl_entry.h`
  - `fs/overlayfs/util.c`
- 三处差异全部是中文说明注释，共新增 77 行，没有功能性 DeltaFS 代码。本报告保留这些用户修改，不覆盖它们。
- 仓库历史中曾有 `docs/deltafs_design_reproduction.md`，当前 HEAD 以 `del: outdate doc` 删除了它。本报告使用新文件名，不恢复旧稿。

旧稿有可复用的背景说明，但存在需要修正的设计假设，例如把未公开的多个 ioctl 当成事实、使用 `512 × PATH_MAX` 的约 2 MiB ioctl 结构、遗漏 mount/dentry 配对和 mmap 绕过路径等。附录 A 给出简要审计。

### 2.2 已有构建配置

当前 `.config` 已包含：

- `CONFIG_OVERLAY_FS=m`
- `CONFIG_XFS_FS=m`
- `CONFIG_BLK_DEV_LOOP=y`
- `CONFIG_OVERLAY_FS_REDIRECT_ALWAYS_FOLLOW=y`
- `CONFIG_OVERLAY_FS_XINO_AUTO=y`
- UBSAN 和 KFENCE 的部分配置

当前运行内核是 WSL2 6.6，而源码/模块目标为 Linux 6.8，不能直接把这里构建出的模块加载到当前 WSL2 内核。编译可以在当前环境完成；功能、并发和崩溃测试应在可启动自定义 6.8 kernel 的 KVM VM、Firecracker guest 或裸机上进行。

建议维护两套配置：

- **debug config**：打开 `CONFIG_OVERLAY_FS_DEBUG`、KASAN、UBSAN、LOCKDEP/PROVE_LOCKING、RCU 调试和 XFS 调试；用于正确性与压力测试。
- **performance config**：关闭 sanitizer/lockdep，固定 CPU governor 和存储配置；用于延迟与 I/O 对比。

## 3. 论文中 DeltaFS 的明确设计

### 3.1 机制事实表

| 主题 | 论文明确披露的内容 | 位置 |
|---|---|---|
| 动态 OverlayFS | layer stack 可在不 umount 的情况下运行时重配置 | §1、§4.1 |
| checkpoint | 冻结当前 writable upper，把它插为最上层只读 lower，安装 fresh upper | Fig. 4、§4.1 |
| 用户态动作 | controller 在 ioctl 前用 `rename(2)` 重命名当前 upper，关键路径主要是元数据操作 | §4.1 |
| 内核 ioctl | 解析新配置；每个路径建立 private mount clone；构建 layer array；原子换指针 | §4.1 |
| 发布协议 | release-consistent store；实现节称在 spinlock 下原子交换 | §4.1、§5 |
| generation | 每文件系统维护 `checkpoint_gen`，inode 缓存上次解析 generation | §4.1.1 |
| cache | 切层时使旧 dentry/inode cache 失效；实现节具体说 cached dentry 被标为 revalidation | §4.1、§5 |
| lazy switch | generation 不同时，在写路径重新解析，必要时 copy-up，在 per-inode mutex 下更新 generation/backing dentry | §4.1.1 |
| copy-up race | loser 观察到 `EEXIST`，比较已存在 upper 的 backing mount 与当前 upper；当前代则复用，旧代则丢弃 | §4.1.1 |
| restore | 切到目标 checkpoint 保存的精确 layer configuration | §3.3 |
| array 回收 | 设计节称旧数组延迟两个 checkpoint；实现节称 RCU grace period 后释放 | §4.1、§5 |
| XFS reflink | OverlayFS copy-up 通过 `vfs_clone_file_range()` 克隆 extent；rename 降层保留 extent map | §3.1、§4.1 |
| 实现规模 | “checkpoint ioctl”增量约 565 行 C、跨四个文件；不是整个 DeltaFS 的已知总 LOC | §5 |

论文内部对旧数组生命周期同时用了“两次 checkpoint 后释放”和“RCU grace period 后释放”两种表述。没有代码时不能判断它们是组合策略还是文稿层次不同；复现必须用引用计数与测试证明实际安全性，不能只照抄其中一句。

### 3.2 论文中的 DeltaFS 单项结果

Table 4 给出的 Overlay ioctl switch 均值为：

| 操作 | 均值 |
|---|---:|
| checkpoint switch | 0.07 ms |
| restore switch（完整系统 fast 情形） | 0.19 ms |
| restore switch（完整系统 slow 情形） | 0.25 ms |

这里的 fast/slow 标签来自完整系统的恢复路径，不代表 DeltaFS 有两套 restore 算法。本文只把 0.07/0.19/0.25 ms 当作同硬件同工作负载下的参考值，不把完整系统的其他耗时归因给 DeltaFS。

写放大实验还给出：

- XFS+reflink 只物化实际被修改的 4 KiB 块；ext4 和未启用 reflink 的 XFS 会退化为整文件 copy-up。
- 1–8 KiB 编辑所在的小文件场景，论文报告 physical I/O 从约 132 KiB 降至 26 KiB，主要来自 XFS 元数据效率。
- 128–256 KiB 文件场景从约 315 KiB 降至 141 KiB，reflink 共享贡献更大。

论文没有给出 ioctl 单项的样本数、p95/p99 或方差。因此复现时应报告完整分布，而不能只比较均值。

## 4. 必须先固定的状态语义

### 4.1 checkpoint 保存什么

一个 checkpoint 保存的是“刚刚被冻结的只读层及其祖先链”，不是一个未来仍可写的 upper。

```text
初始：
    RW upper-A
    RO base

checkpoint A 后继续运行：
    RW upper-B              <- 新的 active branch upper
    RO layer-A              <- checkpoint A 的增量
    RO base

checkpoint B 后继续运行：
    RW upper-C
    RO layer-B              <- checkpoint B 的增量
    RO layer-A
    RO base

restore A 后开新分支：
    RW upper-A-prime        <- 必须重新创建
    RO layer-A
    RO base
```

restore 不能把 `layer-A` 重新当成 RW upper，否则下一条分支会污染 checkpoint A。用户态 metadata 中应把 snapshot 的 immutable lower chain 与 active upper/workdir 分开记录。

### 4.2 状态转换

```mermaid
flowchart LR
    Q["外部协调器静止所有写入者"] --> R["rename active upper -> frozen layer"]
    R --> N["创建 fresh upper；准备目标 lower chain"]
    N --> B["内核校验并构建 immutable layer view"]
    B --> S["spinlock 下发布 view；checkpoint_gen++"]
    S --> V["cached dentry 标记重校验"]
    V --> C["旧 view 经 RCU + 引用计数延迟回收"]
    C --> U["恢复写入者；旧 inode 首次写时 lazy refresh"]
```

### 4.3 “O(1) restore”的严格解释

论文称 arbitrary rollback 为 O(1)，合理解释是：切换不遍历目录树、不复制文件内容，成本与文件数和数据量无关。

但论文同时说 ioctl 会按路径解析配置、为每个路径 clone mount 并构建数组。如果 restore 每次重建包含 `L` 层的数组，内核工作至少是 O(L)。因此报告和 benchmark 应使用以下准确措辞：

- **对文件数/目录项数/总字节数近似 O(1)**；
- **对层深可能是 O(L)**，除非后续实现预构建并缓存 view handle；
- 必须测量 `L = 1, 8, 32, 128, 256, 499`，不能只测浅层。

## 5. Linux 6.8 原版代码对应关系

下表行号以当前工作树为准；后续开发时优先以符号名定位。

| 论文机制 | Linux 6.8 代码锚点 | 原版语义 | 复现改动方向 |
|---|---|---|---|
| 全局 layer stack | `fs/overlayfs/ovl_entry.h:58-93`, `struct ovl_fs` | `numlayer/layers/fs/workdir/config` 都按 mount 生命周期静态保存 | 引入 active immutable view、generation、发布锁和退休队列；不能只换裸数组 |
| 单 layer | `ovl_entry.h:33-45`, `struct ovl_layer` | 保存 private `vfsmount`、trap、fs/fsid/idx | view 必须拥有这些对象及引用；降级 upper 需要只读 mount 身份 |
| inode backing | `ovl_entry.h:162-176`, `struct ovl_inode` | 缓存 `__upperdentry`、`oe`，已有 per-inode `mutex lock` | 增加 generation；把 upper 从裸 dentry 扩成带 mount/view identity 的 backing path |
| lower backing | `ovl_entry.h:47-55`, `struct ovl_path/ovl_entry` | `ovl_path.layer` 是指向 `ofs->layers[]` 内部元素的裸指针 | layer array 退休前，所有 entry 都必须刷新或持有 view 引用；RCU grace period 本身不够覆盖长期 open inode |
| upper clone | `fs/overlayfs/super.c:487-545`, `ovl_get_upper()` | `clone_private_mount()` 建 upper clone，层 0 固定为 upper | 抽取成可为新 view 调用的构建 helper |
| lower clone | `super.c:990-1116`, `ovl_get_layers()` | 每个 lower clone private mount，并设置 `MNT_READONLY|MNT_NOATIME` | 复用验证/clone 逻辑；提交前完整构建，失败不能碰 active view |
| root lower stack | `super.c:1118-1172`, `ovl_get_lowerstack()` | root `ovl_entry` 保存 dentry 和指向全局 layer 元素的指针 | switch 必须同时原子更新/重建 root binding |
| mount 初始化 | `super.c:1300-1403`, `ovl_fill_super()` | 一次性分配 layers、upper、workdir、lower stack | 初始化 DeltaFS 锁、generation 和 initial view；普通 mount 行为保持不变 |
| remount | `fs/overlayfs/params.c:541-566`, `ovl_parse_param()`；`:663-682`, `ovl_reconfigure()` | 新 mount API 明确返回 “No changes allowed in reconfigure”；remount 只处理只读同步 | 不能把现有 remount/fsconfig 当作热切层入口；论文所述 custom ioctl 是合理落点 |
| teardown | `params.c:740-773`, `ovl_free_fs()` | 假定只有一组 layers；逐层 iput、unmount、kfree | 改为释放 active/retired views，等 RCU 和引用归零后再 unmount |
| lookup | `fs/overlayfs/namei.c:1081-1395`, `ovl_lookup()` | upper 直接取 `ofs->layers[0]`，lower 从父 `ovl_entry` 复制 | lookup 开始时固定一个 view 快照；构造的新 inode/entry 记录该 view generation |
| 层内解析 | `namei.c:222-415`, `ovl_lookup_positive_unlocked()/ovl_lookup_layer()` | 已有逐层路径解析、whiteout/opaque/redirect/metacopy 处理 | 可抽出用于 generation mismatch 的重新解析；不能另写一套忽略 OverlayFS 语义的 lookup |
| dentry revalidate | `fs/overlayfs/super.c:91-152` | 当前只把 revalidate 传递给具有相应回调的真实 dentry | DeltaFS mount 上还要比较 generation；RCU walk mismatch 返回 `-ECHILD`，ref-walk 再使旧 dentry 失效 |
| inode 初始化 | `fs/overlayfs/inode.c:887-902`, `ovl_inode_init()` | 把 `upperdentry/oe` 一次写入 inode | 记录 generation 和 view/backing mount identity |
| upper 更新 | `fs/overlayfs/util.c:581-596`, `ovl_inode_update()` | `WARN_ON` 已有 upper，只支持从无 upper 到有 upper | lazy switch 必须支持在 per-inode lock 下替换旧代 upper/oe，并正确退休旧引用 |
| copy-up 串行化 | `util.c:677-746`, `ovl_already_copied_up*()/ovl_copy_up_start()` | 同一个 overlay inode 由现有 `oi->lock` 串行 copy-up | fast path 先比 generation；不能把“存在 upper”误判成“当前 generation 已 copy-up” |
| copy-up 提交 | `fs/overlayfs/copy_up.c:780-1000` | tmpfile/workdir 路径安装 upper，随后 `ovl_inode_update()` | 对 `EEXIST` 比较实际 backing mount/view identity；同代复用，旧代重试 |
| regular open | `fs/overlayfs/file.c:145-175`, `ovl_open()` | `file->private_data` 缓存真实 backing file | 旧 fd 写前先 lazy refresh；刷新后可复用 `ovl_real_fdget_meta()` 的 inode mismatch 自愈逻辑 |
| regular fd 自愈基础 | `file.c:95-130`, `ovl_real_fdget_meta()` | 当前真实 file inode 与重新解析路径不同时，临时 reopen 新 realfile | 是旧 fd lazy switch 的重要现成基础，但前提是 inode/path 已被刷新到当前 generation |
| 写路径 | `file.c:288-327`, `ovl_write_iter()`；`:359-386`, `ovl_splice_write()`；`:426-457`, `ovl_fallocate()` | 通过 realfile 写；没有 checkpoint generation 检查 | 所有修改型入口在取得 real fd 前调用统一 lazy refresh helper |
| 目录 fd | `fs/overlayfs/readdir.c:54-61`, `struct ovl_dir_file`；`:946-979`, `ovl_dir_open()` | 缓存 `realfile/upperfile/cache/is_upper`，没有 regular fd 的 inode mismatch reopen | switch 后必须丢弃目录 cache 并 reopen，或 MVP 明确禁止跨 switch 的活动目录 fd |
| mmap | `file.c:414-424`, `ovl_mmap()`；`fs/backing-file.c:300-324`, `backing_file_mmap()` | VMA 的 `vm_file` 被替换为真实 backing file，fault/page_mkwrite 直接进入 XFS | 仅在 `write_iter` 加 generation 检查无效；需单独 VMA 方案或 MVP 拒绝 writable shared mmap |
| reflink copy-up | `copy_up.c:246-274`, `ovl_copy_up_file()` | 先调用 `vfs_clone_file_range()`；失败再 splice 全量复制 | 无需重写数据复制；保证所有层在同一 reflink XFS superblock |
| VFS clone 限制 | `fs/remap_range.c:376-413` | source/destination `i_sb` 不同直接 `-EXDEV` | base 与 upper 若在不同 XFS image，首次 copy-up 不能 reflink，这是部署硬约束 |
| XFS reflink | `fs/xfs/xfs_file.c:1161-1196`, `xfs_file_remap_range()` | 要求 `xfs_has_reflink()`，随后调用 XFS reflink block remap | 格式化时显式 `reflink=1`，用 `xfs_info` 和 extent/I/O 指标核验 |
| layer 上限 | `fs/overlayfs/params.h:20`；`params.c:303-307` | lower directory 上限 `OVL_MAX_STACK = 500` | 一条链最多为 base + 约 499 个 checkpoint 增量；必须设计 GC/压平策略并测深层退化 |

## 6. 从对应关系得出的关键缺口

### 6.1 `ofs->layers` 不是唯一动态状态

`struct ovl_fs` 还拥有 `numlayer`、`numfs`、`numdatalayer`、`fs`、workdir/workbasedir、trap、whiteout cache、配置路径和 feature 状态。每个缓存 inode 的 `ovl_entry` 又保存指向 layer array 内部的指针。只交换 `ofs->layers` 会立即造成：

- `numlayer` 与数组长度不一致；
- cached `ovl_path.layer` 指向释放后的旧内存；
- root lower stack 仍描述旧 view；
- teardown 重复释放或遗漏 mount/trap；
- show_options、fsid/xino/export 路径读到混合状态。

因此 [D] 建议把“一次层配置”封装成不可变 `struct ovl_delta_view`，而不是继续增加一组可独立更新的裸字段。

### 6.2 `__upperdentry` 必须与 mount identity 配对

原版 `ovl_path_upper()` 取 cached `__upperdentry`，再与“当前全局” `ovl_upper_mnt(ofs)` 拼成 `struct path`。层切换后，cached dentry 可能属于旧 upper，而全局 mount 已经是 fresh upper；两者组合不是一个合法的 backing path。

[D] 建议把 inode 的 upper backing 改为至少包含：

```c
struct ovl_delta_backing {
    struct dentry *dentry;
    struct vfsmount *mnt;
    u64 generation;
};
```

实际布局需考虑 RCU、引用计数和 cache line，不应直接照抄此示意。关键是不允许再从不同 generation 拼接 dentry 与 mount。

### 6.3 dentry/inode cache 不能靠一次全局 drop 解决

论文说 cached dentry 被标记 revalidation，而不是调用全局 `drop_caches`。这是必要的，因为 open fd、cwd、mmap 和被引用 inode 不能保证被 shrink 回收。

- 正 dentry 可以通过 inode/view generation 判断是否过期。
- 负 dentry 没有 inode，而当前 `d_fsdata` 被 OverlayFS 当位标志使用。MVP 可让 DeltaFS mount 的负 dentry 不长期缓存；完整版本应给 dentry 增加独立 generation 元数据。
- root dentry 永不依赖普通 lookup 重建，必须在 commit 中显式更新。
- directory readdir cache 需要按 generation 失效。

### 6.4 普通旧 fd 有可复用基础，但不是自动正确

`ovl_real_fdget_meta()` 已会在 backing inode 变化后临时 reopen realfile。因此合理路径是：

1. `ovl_write_iter()` 等入口检测 inode generation；
2. mismatch 时在 `oi->lock` 下按当前 view 重新解析并 copy-up；
3. 原子更新 inode backing path/generation；
4. `ovl_real_fdget_meta()` 发现缓存 realfile inode 不同并 reopen 当前 realfile；
5. 冻结层内容保持不变。

需要覆盖的不只有 `write(2)`：还包括 splice、fallocate、truncate/setattr、fileattr、ACL/xattr、rename/link/unlink 的父目录、O_DIRECT、异步 I/O、fsync/flush 以及目录 fd。

### 6.5 writable `MAP_SHARED` 是独立研究门槛

Linux 6.8 `ovl_mmap()` 调用 `backing_file_mmap()`，后者执行 `vma_set_file(vma, realfile)`。此后 page fault 和 `page_mkwrite` 直接落到 XFS，不再执行 OverlayFS 的 `write_iter`。

因此以下旧思路不成立：

> “给 inode 加 generation，然后在普通 write path 检查，就自然覆盖 mmap。”

MVP 应明确拒绝或检测 writable `MAP_SHARED`，不能静默宣称支持。论文对齐版本可研究两条路线：

1. **Overlay VMA wrapper**：保留/包装底层 `vm_ops`，在第一次写 fault 时检查 generation，安全切换 backing file 后再转发；必须正确处理 fork、mremap、mprotect、close、huge page 和底层 `vm_private_data`。
2. **切层时 eager VMA rebinding**：在所有写入者静止时枚举并迁移相关 VMA；需要正确持有 `mmap_lock`、处理脏页和 page cache，风险更高。

选择前先做最小实验验证 VMA/file/mapping 的实际关系。验收标准不是“不崩溃”，而是 checkpoint 后经旧 mapping 写入并 `msync()`，冻结层 inode 的 hash、mtime、extent 和内容仍完全不变。

### 6.6 XFS reflink 有同 superblock 前提

`vfs_clone_file_range()` 在 source/destination `i_sb` 不同时直接返回 `-EXDEV`。因此“base 位于一个只读 XFS image、upper 位于另一个 XFS rootfs”不能让首次 base→upper copy-up 使用 reflink。后续同一 rootfs 内的历史 layer→fresh upper 仍可 reflink，但首次复制会是整文件。

为了复现论文的低写放大曲线，最小环境应把 base、所有 frozen layers、active upper 和 workdir 放在**同一个**启用 reflink 的 XFS filesystem 上；只读性通过 mount clone/权限体现，而不是放到另一个 block image。

### 6.7 复杂 feature 会放大状态空间

第一阶段建议强制：

```text
index=off,nfs_export=off,metacopy=off,xino=off,redirect_dir=nofollow
```

本仓库配置了 `CONFIG_OVERLAY_FS_REDIRECT_ALWAYS_FOLLOW=y`，所以只写 `redirect_dir=off` 可能仍转换为 follow；应显式使用 `nofollow`，或在 debug kernel 中关闭该 Kconfig。

这些限制会保留 stock OverlayFS 的部分非 POSIX 行为，例如 `index=off` 时 lower hardlink copy-up 可能断开。它们是 MVP 的已知边界，不应永久隐藏；后续按 feature 逐一放开并增加回归测试。

## 7. 推荐技术路线

### 7.1 内核总体结构：immutable view，而不是裸数组

[D] 建议引入“层视图”对象，概念上包含：

- generation；
- upper、lower layer array 和数量；
- 每层 private mount clone、trap、fs/fsid/idx；
- 当前 workdir/workbasedir 及其引用；
- root lower stack；
- RCU head、引用计数和退休状态。

`struct ovl_fs` 只保留一个 `active_view` RCU 指针、全局 generation、switch mutex/spinlock 和 retired-view 管理。普通 OverlayFS mount 仍可走原路径；只有显式启用 DeltaFS 的 mount 才使用动态 view 和额外 revalidation。

建议的生命周期规则是：

1. 构建阶段完全在 active view 之外分配、解析路径、clone mount 和验证。
2. 提交时只做指针/generation/root binding 的有界更新。
3. 读侧在 RCU 临界区内访问；若对象要跨临界区保存，则获取 view 引用。
4. retired view 同时满足 RCU grace period 已结束、长期引用归零后，才可 iput/dput/unmount/kfree。
5. per-inode lazy refresh 成功后释放旧 view/backing 引用。

这比“两个 checkpoint 后无条件释放”保守，但能覆盖长期 open fd。后续若实验证明两代延迟足够，可以优化；不能先以经验常数替代引用所有权。

### 7.2 同步模型

[P] 论文给出的同步原语是 layer pointer 在 spinlock 下交换、release-consistent publication、旧数组经 RCU 回收、inode 在 per-inode mutex 下刷新。

[D] 在 6.8 上建议分层使用：

- `switch_mutex`：串行化配置复制、路径验证、view 构建和一次完整 switch；这些步骤可能睡眠，不能放 spinlock。
- `layers_lock`：只保护 active view 指针、generation 和最小根状态的提交。
- RCU：保护短期无锁读和 old→new view 指针替换。
- `refcount_t`：保护 open inode/file/dir 等长期引用。
- 已有 `ovl_inode.lock`：串行同 inode 的 generation refresh/copy-up/backing 替换。
- VFS `inode_lock`、upper parent lock、rename lock：继续遵循原 OverlayFS lock ordering，不在持有 upper write count 时反向获取 lower inode lock。

必须用 lockdep 验证，而不是在设计文档中假定锁序正确。

### 7.3 ioctl ABI：单一、版本化的 SWITCH

论文只披露了“一个 custom ioctl”，没有披露命令号或参数。建议把 checkpoint 和 restore 都建模为同一个 `SWITCH`：两者的内核动作相同，区别只在用户态选择了哪条 lower chain。

概念 UAPI 如下，字段仅表示设计方向：

```c
struct deltafs_ioc_switch {
    __u32 size;
    __u32 version;
    __u64 flags;
    __u64 expected_gen;
    __s32 upper_fd;          /* O_PATH | O_DIRECTORY */
    __s32 work_fd;           /* O_PATH | O_DIRECTORY */
    __u32 nr_lower;
    __u32 reserved0;
    __aligned_u64 lower_fds; /* userspace pointer to __s32[nr_lower] */
    __u64 reserved[4];
};
```

推荐约束：

- ioctl 对 merged root directory fd 调用，只接受 root；入口可放在 `ovl_dir_operations.unlocked_ioctl`。
- 调用者需要目标 mount user namespace 内的 `CAP_SYS_ADMIN`。
- `size/version/reserved` 必须严格校验，未知 flag 返回 `-EINVAL`。
- 路径以 `O_PATH` directory fd 传入，避免大字符串结构、路径解析 TOCTOU 和 `_IOC_SIZE` 限制。
- kernel 对所有 fd `fget` 后验证是目录、同一受支持 filesystem、upper 可写、lower 可读、upper/work 不重叠、layer 数不超过 `OVL_MAX_STACK`。
- `expected_gen` 不等于当前 generation 时返回 `-ESTALE`，防止两个 controller 覆盖彼此状态。
- `SWITCH` 成功后 generation 恰好加一；用户态不需要第二个 checkpoint/restore ABI。

这与论文可能使用的 path-based ABI 不一定相同，但更适合可维护的 clean-room 实现。MVP 不应同时设计 `CHECKPOINT/RESTORE/GET/DROP` 四套未经证实的 ioctl。

### 7.4 内核 switch 流程

[D] 推荐实现顺序：

1. `copy_from_user()` 读取固定头，校验 ABI 版本和长度。
2. 复制 fd 数组并为每个目录取得稳定 `struct path` 引用。
3. 用 `ovl_get_upper()`、`ovl_get_layers()`、trap/overlap 检查的重构 helper 构建新 view。
4. 对 MVP 强制所有层和 workdir 位于同一 XFS `s_dev/i_sb`，且 feature 组合在白名单内。
5. 预分配新 root binding、配置显示字符串和所有提交后必需对象；此时任何失败都只释放 new view。
6. 获取 `switch_mutex`，重新检查 `expected_gen` 和 active view。
7. 在短 spinlock 临界区内用 `rcu_assign_pointer()` 发布 new view，递增 generation，并使 root 指向新 binding。
8. 标记 cached dentry 需要 generation revalidation；清除/版本化 readdir cache。
9. 释放 spinlock/mutex，把 old view 放入 RCU+refcount 退休队列。
10. ioctl 返回 0；controller 再提交用户态 manifest。

切换临界区不能执行 path lookup、mount clone、内存分配、用户内存访问或同步 I/O。

### 7.5 generation-based lazy refresh

建议提供一个统一内部入口，例如：

```c
int ovl_delta_ensure_current(struct dentry *dentry,
                             enum ovl_delta_access access);
```

语义：

1. acquire-load 当前 filesystem generation；与 inode generation 相同则返回 0。
2. mismatch 时获取 `ovl_inode.lock` 并二次检查。
3. 固定当前 view 引用；按 overlay dentry 相对路径，在该 view 中复用 `ovl_lookup_layer()` 语义重新解析 upper/lower、whiteout 和 opaque 状态。
4. 对修改型访问，若当前 view 只有 lower，则 copy-up 到当前 upper。
5. 构造包含正确 mount+dentry 的新 backing 和 `ovl_entry`；原子替换 generation/backing/entry。
6. 目录则同时丢弃旧 readdir cache；普通文件让 `ovl_real_fdget_meta()` reopen 当前 realfile。
7. 释放旧 backing/view 引用。

并发 copy-up 要比较**实际 `vfsmount` 或 view identity**，不能只比较 dentry 的 `d_sb`：所有层通常都在同一个 XFS superblock，`d_sb` 相等无法区分 generation。

MVP 可在 switch 时要求所有任务无在途 syscall，从而先解决 switch→后续写的竞态。论文对齐阶段再加入 lookup/write 与 switch 并发压力，不应一开始把所有并发路径混在一个补丁中。

### 7.6 用户态 controller

建议目录布局：

```text
/mnt/deltafs-xfs/sandboxes/<sandbox-id>/
    base/                         # 与 layers/upper 同一 XFS
    layers/
        <layer-uuid>/             # 冻结后永不原地写
    active/
        <branch-uuid>/
            upper/
            work/
    meta/
        snapshots.json
        transaction.json
    merged/
```

snapshot 记录至少包含：

```json
{
  "id": "cp-000123",
  "parent": "cp-000122",
  "frozen_layer": "layers/8b...",
  "lower_chain": ["layers/8b...", "layers/4e...", "base"],
  "kernel_generation": 123,
  "state": "committed"
}
```

不要在 snapshot 中把 `active/.../upper` 记录成可复用 checkpoint upper。active upper/work 是当前分支的临时资源。

checkpoint 流程：

1. 创建 `transaction.json`，状态为 `preparing`，fsync 文件和父目录。
2. 静止该 mount 的全部写入者，等待在途 syscall/copy-up 完成；同步必要的文件数据和目录元数据。
3. 把 active upper 原子 rename 为唯一 frozen layer 名称。
4. 为继续执行创建 fresh active upper/work，二者与 frozen layers 位于同一 XFS。
5. 以 `[just-frozen, parent-chain...]` 调用 `SWITCH(expected_gen)`。
6. 成功后原子写入 snapshot manifest 和 `committed` transaction；失败则在写入者仍静止时把目录 rename 回去或按旧 view 再切回。
7. 恢复写入者。

restore 流程：

1. 读取并校验目标 snapshot 的 immutable lower chain。
2. 创建新的 branch upper/work，绝不复用 frozen layer 为 RW。
3. 静止全部写入者。
4. 用 `[target-layer, ancestors..., base]` 调用同一个 `SWITCH(expected_gen)`。
5. 提交 branch metadata 后恢复写入者；旧 active upper 进入待 GC 集合。

controller 第一版建议用 C 实现 ioctl/目录 fd/rename/fsync 的核心，Python 只负责测试编排，避免 Python 异常或垃圾回收模糊系统调用边界。

### 7.7 XFS 布局与 reflink

最小可重复环境：

```bash
truncate -s 40G deltafs-xfs.img
mkfs.xfs -f -m reflink=1 -b size=4096 deltafs-xfs.img
mkdir -p /mnt/deltafs-xfs
mount -o loop deltafs-xfs.img /mnt/deltafs-xfs
xfs_info /mnt/deltafs-xfs
```

必须确认 `reflink=1`、block size 为 4096，并用 `stat -f`/`findmnt` 证明 base、layers、upper、workdir 的 backing superblock 相同。

初始 mount 可使用：

```bash
mount -t overlay deltafs \
  -o lowerdir=/mnt/deltafs-xfs/sandboxes/s1/base,\
upperdir=/mnt/deltafs-xfs/sandboxes/s1/active/b0/upper,\
workdir=/mnt/deltafs-xfs/sandboxes/s1/active/b0/work,\
index=off,nfs_export=off,metacopy=off,xino=off,redirect_dir=nofollow \
  /mnt/deltafs-xfs/sandboxes/s1/merged
```

命令仅是 MVP 配置示例；实际 controller 应使用 mount API/目录 fd 并检查所有返回值。

### 7.8 GC 与故障恢复

用户态 snapshot tree 决定 layer 可达性，但删除前还必须满足：

- 不属于任何 committed snapshot 的 ancestor；
- 不属于 active view；
- 内核没有 retired view、inode、file、VMA 或 private mount 引用；
- controller 没有针对它的 preparing transaction。

第一版宁可只标记、不自动删除。后续增加内核 debug counters 或只读状态接口，能观察 active/retired view 和引用数后再启用 GC。

断电恢复时重新 mount 会重建内核状态；controller 根据 `committed/preparing` manifest 和实际目录名完成 redo/undo。测试前必须明确 fsync 顺序，否则 `rename()` 的内存原子性不能等同于掉电持久性。

## 8. 预计代码改动面

论文只说约四个文件，没有公开文件名；下面是依据 6.8 代码得出的复现改动面，不声称与原补丁一致。

| 文件 | 预计工作 |
|---|---|
| `fs/overlayfs/ovl_entry.h` | view/backing/generation/RCU/refcount 数据结构；修正 upper path 表示 |
| `fs/overlayfs/overlayfs.h` | 内部 helper、lazy refresh、switch、view get/put 原型 |
| `fs/overlayfs/super.c` | initial view、private mount 构建复用、dentry revalidation、root switch、销毁 |
| `fs/overlayfs/params.c` | 抽取 layer 参数验证/释放逻辑；动态 view teardown；feature 白名单 |
| `fs/overlayfs/namei.c` | 固定 view 的 lookup、stale inode 重解析、负 dentry 策略 |
| `fs/overlayfs/inode.c` | inode generation 初始化；setattr/ACL/fileattr 等修改入口刷新 |
| `fs/overlayfs/util.c` | backing path 访问器、replace-capable inode update、view identity 比较 |
| `fs/overlayfs/copy_up.c` | stale upper 判断、同代 `EEXIST` 复用、跨代重试 |
| `fs/overlayfs/file.c` | regular fd 修改入口 lazy refresh；mmap 研究实现 |
| `fs/overlayfs/readdir.c` | root ioctl、目录 fd/readdir cache generation refresh |
| `fs/overlayfs/dir.c` / `xattrs.c` | 目录和 xattr 修改路径的 current-generation 保证 |
| `include/uapi/linux/overlayfs.h`（新） | 版本化 SWITCH ABI；名称可在实现评审时确定 |
| `tools/deltafs/`（新） | controller/CLI、manifest、故障恢复 |
| `tools/testing/selftests/filesystems/overlayfs/` | DeltaFS kselftests 和 mmap/open-fd 辅助程序 |

为了可审查，建议把补丁拆成数据结构、closed-world switch、cache/lazy fd、mmap、controller/tests 五组，不追求复刻论文的约 565 LOC。

## 9. 分阶段开发步骤与验收门

### P0：冻结语义与建立基线

工作：

- 写出 UAPI/manifest/错误模型短规范。
- 构建未修改的 6.8 debug kernel，运行现有 OverlayFS selftests。
- 在同一 XFS 上验证 stock `ovl_copy_up_file()` 确实走 reflink；在不同 superblock 上确认回退。
- 记录 static OverlayFS 的 open fd、mmap、whiteout、hardlink 行为，形成 baseline。

验收：未修改内核测试通过；能够用 trace/ftrace 看到 `vfs_clone_file_range()` 和 XFS remap 路径；所有后续差异有基准可比。

### P1：controller 骨架与 versioned SWITCH ABI

工作：

- 实现 root fd ioctl、目录 fd 传参、权限/版本/保留字段检查。
- 实现 controller 的 init/list/inspect 和 transaction manifest，但尚不允许真正切层。
- 为无权限、坏 fd、跨 filesystem、层数超限、expected generation 错误写负向测试。

验收：所有非法输入稳定返回指定 errno，无泄漏、无内核告警；ABI 结构可被 32/64 位 compat 审查。

### P2：closed-world hot switch MVP

工作：

- 引入 immutable view，复用 upper/lower private mount clone 和 trap 验证。
- 在“无 open fd、无 mmap、写入者全部静止”条件下实现 pointer swap、root binding 和 retired view。
- 完成 controller checkpoint/restore/branch。

验收：创建、修改、删除、rename、whiteout、xattr 的多层 checkpoint/任意 restore 正确；恢复后新分支不修改 frozen layer；循环切换无 KASAN/UBSAN/lockdep 报告。

### P3：cache generation 与 lookup 正确性

工作：

- 正 dentry/inode generation revalidate。
- 负 dentry 的 MVP 不缓存策略或显式 generation 元数据。
- root、cwd、readdir cache 和 inode alias 刷新。
- 逐步加入 redirect/metacopy/index 前，先证明限制能被 mount/ioctl 拒绝。

验收：预热正/负 dcache 后切换仍得到正确视图；`stat/open/readdir/getcwd` 不返回旧 generation；并发只读 lookup 压力下无 UAF。

### P4：旧 fd lazy switch 与 copy-up 竞态

工作：

- 给 regular fd 的所有修改入口接入统一 ensure-current helper。
- 更新 `__upperdentry` 为 mount+dentry backing；改造 `ovl_inode_update()` 的 set-once 假设。
- 处理同 dentry 并发 copy-up 的 `EEXIST` 和 mount identity。
- 支持或明确限制跨 switch 的目录 fd、open-but-unlinked file。

验收：checkpoint 前打开的 fd 在 checkpoint 后写入只改变 fresh upper；冻结层内容/mtime/extent 不变；两线程并发写同文件不会使用 stale upper；O_APPEND/pwrite/truncate/fallocate/splice/O_DIRECT 分别通过。

### P5：writable mmap 门槛

工作：

- 先加入可检测且 fail-closed 的限制，遇到 writable `MAP_SHARED` 返回 `-EBUSY` 或 `-EOPNOTSUPP`。
- 用独立原型比较 VMA wrapper 与 eager rebinding。
- 完成 chosen path 的 VMA 生命周期、write fault 和 page cache 处理。

验收：旧 mapping 写入、`msync()`、`mprotect()`、fork 后写、munmap/reopen、并发 switch 测试全部保持 frozen layer 不变；否则版本仍标为 MVP，不得称论文对齐。

### P6：生命周期、GC、崩溃与 feature 放开

工作：

- 压测 RCU/refcount/view retirement、模块卸载和反复 mount/unmount。
- 完成 manifest redo/undo、故障注入和安全 GC。
- 按 `redirect_dir`、hardlink/index、metacopy、xino、idmapped mount 顺序逐项评估；不需要的 feature 可永久显式拒绝。

验收：故障点重启后只出现 old 或 new committed view，不出现混合层栈；GC 不删除在用 layer；1000+ switch 和 fsstress 后无泄漏/UAF/deadlock。

### P7：性能复现

工作：

- 分离 controller rename/mkdir/fsync、内核 build-view、原子 commit 和 lazy refresh 耗时。
- 测试 layer depth、文件树规模、文件大小、编辑大小和 backing filesystem。
- 固定硬件、kernel config、CPU governor、缓存冷热状态，输出原始数据和脚本。

验收：先证明延迟与文件数/总字节数无关，再与论文的 0.07/0.19/0.25 ms 均值比较；若环境或语义不同，只报告差异，不调参隐藏。

## 10. 测试矩阵

### 10.1 功能与分支语义

| 类别 | 场景 | 必须断言 |
|---|---|---|
| 内容 | create/write/append/truncate 后 checkpoint→继续写→restore | 目标 checkpoint 内容精确恢复 |
| 删除 | unlink/rmdir/whiteout 后在不同历史点恢复 | whiteout/opaque 只影响所属 generation |
| 名称 | file/dir rename、交换、跨目录移动 | 目标 view 无新分支残留 |
| 元数据 | chmod/chown/utime/xattr/ACL | 内容和 metadata 同时恢复 |
| 类型 | symlink、FIFO、device（有权限时） | 类型与元数据正确 |
| 链接 | hardlink、多名称 copy-up | 结果符合声明的 index 配置；不静默改变语义 |
| 文件布局 | sparse file、预分配、洞、large file | restore 内容/size/extent 合法 |
| 分支 | A→B→C，restore A 后创建 A′ | A′ 与 B/C 互不污染，A frozen layer hash 不变 |
| 深度 | 1/8/32/128/256/499 lowers | 正确性不随深度丢层，超限明确失败 |

每次测试都直接读取 frozen layer 并记录 hash/mtime/filefrag，而不仅检查 merged view。否则“merged 看起来正确、历史层已被旧 fd 污染”会漏检。

### 10.2 cache 与 fd

- 切换前反复 `stat/open/ENOENT/readdir` 预热正负 dcache。
- 进程 cwd 位于深层目录时 checkpoint/restore。
- root 和子目录 fd 跨 switch 执行 `getdents64()`、`openat()`、`fsync()`。
- 普通 fd 跨 switch 执行 `read/write/pread/pwrite/O_APPEND`。
- `ftruncate/fallocate/copy_file_range/splice`。
- O_DIRECT、io_uring 同步和异步写；异步请求必须在 switch 前 drain，或由内核可靠拒绝 switch。
- open-but-unlinked、rename 后仍打开、多个 hardlink fd。
- 两个线程对同一 lower file 首次写，稳定触发 copy-up race。

### 10.3 mmap

- `MAP_SHARED|PROT_WRITE` 在 checkpoint 前建立，之后写并 `msync(MS_SYNC)`。
- 私有/共享、只读/可写四种组合分别测试。
- checkpoint 前产生 dirty PTE、checkpoint 后 writeback。
- `mprotect` 从只读升为可写。
- fork 后父子同时写同 mapping。
- truncate/hole-punch 与 mapping 并发。
- restore 后旧 VMA 不得指向被恢复目标之外的 generation。

### 10.4 并发与生命周期

- switch 与只读 path walk/readdir 并发。
- switch 前严格 quiesce 的 MVP，以及故意违反 quiesce 时 fail-closed 的测试。
- 反复创建/退休 view；打开 fd 长时间持有旧 view。
- `fsstress`、xfstests OverlayFS/XFS 相关子集、现有 `tools/testing/selftests/filesystems/overlayfs`。
- kmemleak/KASAN/KFENCE/UBSAN/lockdep/RCU stall 检查。
- module unload、umount、强制终止 controller、掉电/guest reboot 故障注入。

### 10.5 权限与恶意输入

- 非特权调用者、其他 mount namespace 的 fd、非目录 fd、已关闭 fd。
- symlink/rename 竞争、upper/work/lower 重叠、递归 overlay、跨 superblock。
- 重复 layer、乱序 layer、超过 500 lower、坏 ABI version/size/flag。
- feature 不受支持时明确返回 `-EOPNOTSUPP`，不能部分切换。

## 11. 性能与写放大复现实验

### 11.1 延迟拆分

至少分别记录：

- controller quiesce 时间（不计入纯 DeltaFS ioctl，但单独报告）；
- rename/mkdir/fsync；
- ioctl 总时间；
- path/fd 校验与 private mount clone；
- atomic publish 临界区；
- restore 后第一次旧 fd lazy refresh/copy-up；
- retired view 回收（异步）。

每项输出样本数、mean、median、p95、p99、min/max 和标准差。冷热 cache 分开；不得只输出最小值。

### 11.2 自变量

- lower depth：`1, 8, 32, 128, 256, 499`。
- merged 文件数：`10^2, 10^4, 10^5, 10^6`。
- 单文件大小：`4 KiB` 至 `1 GiB`。
- 单次编辑：`1 KiB, 4 KiB, 16 KiB, 64 KiB, 256 KiB`。
- backing：ext4、XFS reflink=0、XFS reflink=1。
- fd 状态：closed、open current、open stale、writable mmap。

核心结论应是：switch 不随文件树大小/数据量线性增长；如果随 layer depth 增长，明确给出斜率。

### 11.3 写放大指标

- OverlayFS 逻辑 copy-up bytes。
- block device sectors written（含 journal/metadata）。
- XFS extent sharing/refcount，可结合 `xfs_io`、`filefrag`、tracepoint。
- frozen/current layer 的 `du --apparent-size` 与物理占用。
- 修改前后大文件的 shared extent 数量。

基准用例：base 中放 256 MiB 文件，checkpoint 后只改 4 KiB，再 checkpoint。XFS+reflink 应避免 256 MiB 数据复制；不同 superblock 或 reflink=0 应清楚展示 fallback。

## 12. 风险与应对

| 风险 | 严重度 | 应对 |
|---|---:|---|
| cached `ovl_path.layer` 指向已释放 array | 致命 | immutable view + RCU + 长期 refcount；KASAN/RCU 压测 |
| dentry 与错误 generation 的 mount 拼接 | 致命 | upper backing 保存 mount/view identity，不再用全局 mount 拼接旧 dentry |
| writable mmap 直接写 frozen XFS inode | 致命 | MVP fail-closed；独立 P5 实现和内容不变性验收 |
| inode hash/alias 在 backing 替换后不一致 | 高 | 专门设计 rehash/alias 规则；用 hardlink、open fd、多 dentry 测试 |
| 负 dentry 返回旧 ENOENT | 高 | MVP 不缓存负项；完整版本存 dentry generation |
| workdir/index/trap 与新 upper 不匹配 | 高 | view 同时拥有 upper/work/trap；MVP index/metacopy off |
| RCU grace period 后仍有长期 open 引用 | 高 | RCU 只保护短读；file/inode/view 另持 refcount |
| rename 已完成而 ioctl 失败 | 高 | 全程 quiesce；transaction manifest；可逆 rename 或 old-view SWITCH |
| base/upper 不同 XFS superblock | 中 | init 阶段强校验；性能模式拒绝跨 sb |
| 500 层上限和 lookup 退化 | 中 | 深度 benchmark；可达性 GC；必要时离线 squash/新 base |
| 论文 565 LOC 诱导过度最小化 | 中 | 以可验证所有权和测试为目标，不以 LOC 为验收指标 |
| 当前 WSL2 无法运行目标模块 | 中 | 在自定义 6.8 VM/裸机测试；WSL 仅编译/静态分析 |

## 13. “复现完成”的判定标准

### 13.1 MVP 完成

- closed-world switch、任意历史 restore、分支隔离全部通过。
- feature 限制和 writable mmap 限制是显式、fail-closed 的。
- 正/负 cache 在声明范围内正确。
- XFS reflink 路径得到代码和 I/O 双重证据。
- debug kernel 压测无 KASAN/UBSAN/lockdep/RCU 报告。
- 文档明确称“DeltaFS MVP”，不声称完全论文对齐。

### 13.2 论文对齐版本完成

除 MVP 条件外，还必须：

- checkpoint 前已打开的 regular/dir fd 在切层后透明使用当前 generation；
- 并发 copy-up 不会复用 stale upper；
- writable `MAP_SHARED` 全矩阵通过，冻结层不可变；
- active/retired view 的全部 mount/dentry/inode/file/VMA 引用可证明安全回收；
- 深层、故障注入、GC 和长时间 fsstress 通过；
- 性能报告区分 pure ioctl、controller、lazy refresh 和 XFS I/O，并给出分布。

## 14. 推荐的下一步提交顺序

1. `docs/uapi-and-state-semantics.md`：冻结 `SWITCH`、snapshot 和 failure semantics。
2. baseline selftests + reflink tracing，不改行为。
3. immutable view 数据结构和 initial static-view 包装，行为仍与 stock 相同。
4. closed-world root switch + controller，完成 P2。
5. generation revalidate 和普通旧 fd，完成 P3/P4。
6. mmap 独立 RFC/原型；评审通过后合入。
7. GC/crash/feature/performance。

每一步应保持可启动、可回退、可单独审查。不要在第一版同时改所有 OverlayFS feature。

## 附录 A：已删除旧稿的关键修正

| 旧稿内容 | 修正 |
|---|---|
| 假设 `DELTAFS_CHECKPOINT/RESTORE/GET_STATE/DROP_LAYERS` | 论文只披露一个 custom ioctl；建议一个版本化 `SWITCH`，其他接口按实测需求再加 |
| `DELTAFS_MAX_LAYERS 512` | Linux 6.8 实际 `OVL_MAX_STACK` 为 500 个 lower |
| `char lower[512][4096]` | 结构约 2 MiB，不适合作为 ioctl 固定参数；改用 O_PATH fd 数组和小型 versioned header |
| snapshot 记录 active upper/work | snapshot 应记录 immutable frozen lower chain；restore 总是创建 fresh branch upper/work |
| 只交换 layer array + RCU | 还要处理 root/普通 `ovl_entry`、upper mount identity、trap/workdir/fsid、长期 open 引用 |
| generation write check 自然覆盖 mmap | 6.8 VMA 已改绑真实 file，普通 OverlayFS write hook 不会运行；需独立方案 |
| restore 为严格 O(1) | 对文件数/字节数近似 O(1)，构建 L 层 view 仍可能 O(L) |
| 创建 fresh workdir 是论文事实 | 论文只明确 fresh upper；fresh workdir 是本方案为安全隔离提出的设计选择 |

## 附录 B：开发环境检查清单

- [ ] 能启动本仓库构建的 Linux 6.8 debug kernel。
- [ ] `CONFIG_OVERLAY_FS`、`CONFIG_XFS_FS`、loop device 可用。
- [ ] debug build 打开 OverlayFS debug、KASAN/UBSAN/lockdep/RCU 检查。
- [ ] XFS `reflink=1`、block size=4096。
- [ ] base/layers/upper/work 位于相同 XFS superblock。
- [ ] mount feature 与 MVP 白名单一致。
- [ ] controller 具备所需 namespace 内 `CAP_SYS_ADMIN`。
- [ ] 每次 switch 前能可靠静止并 drain 所有写入者/异步 I/O。
- [ ] 测试同时核验 merged view 和 frozen physical layer。
- [ ] 结果记录 kernel commit、config、硬件、mount options、layer depth 和原始数据。

## 参考资料

1. Yunpeng Dong et al. *DeltaBox: Scaling Stateful AI Agents with Millisecond-Level Sandbox Checkpoint/Rollback*. arXiv:2605.22781v2, 2026. DOI: [10.48550/arXiv.2605.22781](https://doi.org/10.48550/arXiv.2605.22781). Zotero item `K3HZMWBN`, BibTeX key `dongDeltaBoxScalingStateful2026`。
2. DeltaBox 作者项目页与 Code Release 声明：[dongyunpeng-sjtu/deltabox](https://github.com/dongyunpeng-sjtu/deltabox)，检查分支 `main`，commit `7bf5dd3787b350c0b424d0d4499e9a02f42a0544`，2026-07-17。
3. 本仓库 Linux 6.8 源码：`fs/overlayfs/`、`fs/backing-file.c`、`fs/remap_range.c`、`fs/xfs/xfs_file.c`；OverlayFS 文档：`Documentation/filesystems/overlayfs.rst`；上游基线：[torvalds/linux v6.8 fs/overlayfs](https://github.com/torvalds/linux/tree/v6.8/fs/overlayfs)。
