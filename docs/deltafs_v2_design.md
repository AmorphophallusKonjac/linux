# DeltaFS v2 设计与可行性评估

> 状态：v2 第一至三阶段完成；静态门禁与 QEMU 总验收通过
>
> 基线：当前 DeltaFS v1，Linux 6.8.0 OverlayFS
>
> 结论：v2 只修改 UABI 和 layer 嵌套检查。layer stack 继续使用 v1 的 mutex
> 提交和 retired-state-until-unmount 生命周期；RCU 更新与在线回收推迟到后续版本。

## 1. 范围

v2 包含两项改动：

1. checkpoint 只接收 fresh upper fd 和 fresh work fd。当前 active upper 及
   lower chain 由内核从 current state 派生。
2. restore 接收一个 `keep_bottom` 值和一组 fd。`keep_bottom` 表示保留当前
   lower stack 最底部的层数；fd 组表示 fresh upper、fresh work 和目标 lower
   的新前缀。

需求中的 `idx` 在 UAPI 中命名为 `keep_bottom`，并定义为层数而不是数组下标。
当前 writable upper 不属于可保留的 bottom suffix。

v2 明确不包含：

- layer stack 的 RCU 发布；
- retired state 在线回收或 backing layer GC；
- 跨切换普通 fd、目录 fd、cwd、进程 root、mmap 或 io_uring；
- checkpoint/restore 与 lookup、copy-up、写 syscall 或 writeback 并发；
- open-fd lazy switch、并发写 barrier 或 mmap write fault 迁移；
- controller 崩溃后的自动事务恢复。

除本文明确改变的部分外，v1 支持边界继续有效。ioctl 前 workload 必须静止并释放
所有非 control fd 引用。v2 的 layer stack 替换不是并发 data-path 协议。

## 2. 可行性结论

### 2.1 UABI 简化可行

checkpoint 的完整目标可以从 current state 推导：

```text
current = [upper-A(RW), lower-N, ..., base]
input   = [upper-B fd, work-B fd]
target  = [upper-B(RW), upper-A(RO), lower-N, ..., base]
```

restore 的完整目标可以由 fd 前缀和 current lower 后缀拼接：

```text
current lowers       = [C, B, A, base]
target lowers        = [A, base]
keep_bottom          = 2
new lower fd prefix  = []
target state         = [fresh-upper(RW), A, base]
```

若目标为 `[X, A, base]`，则 `keep_bottom == 2`，只传 `X` 的 fd。controller 根据
manifest 中 active/target lower chain 的最长公共后缀计算该值。

### 2.2 非 RCU 更新可直接复用 v1 正确性模型

当前 `ovl_entry.__lowerstack[].layer` 长期指向 `ofs->layers[]` 元素。即使 stale
dentry 已因 generation mismatch 失效，旧 inode/entry 仍可能留在 cache 中。

v2 不改变这些指针的所有权模型：

- target state 在 active `ovl_fs` 之外完整构建；
- `delta_lock` 串行化 final validation 和 commit；
- root inode 在固定锁序下原地更新；
- old active state 加入 `delta_retired`；
- active generation 最后以 release-store 发布；
- 所有 retired state 到 unmount 才释放。

因此 v2 不需要改造 OverlayFS 全部 data path，也不会引入新 read-side lifetime
协议。代价是内核内存和 private mount 数量继续随切换次数增长。

### 2.3 checkpoint 派生需要稳定 source path

checkpoint 不再回传旧 upper fd。controller 仍会在 ioctl 前把 active upper rename 为
`layers/<checkpoint-id>`，所以内核需要在 current layer 中保留 backing mount 上的
稳定 `struct path`：

- private mount 用于 OverlayFS 实际访问，root 相对路径通常只显示 `/`；
- source path 用于 rename 后重新 clone layer、比较 backing identity 和生成
  `show_options` 路径；
- 两者是独立引用，teardown 分别执行 `kern_unmount*()` 和 `path_put()`。

该字段是 UABI 简化所必需，与 RCU 无关。

## 3. 状态语义

### 3.1 Layer 编号

每个 state 的 stack 按从上到下编号：

```text
layers[0]       writable upper
layers[1]       topmost read-only lower
...
layers[n - 1]   bottommost lower
```

`numlayer` 包含 upper，`numlower == numlayer - 1`。restore 的 `keep_bottom` 只在
`layers[1..numlayer-1]` 上计数。

### 3.2 Checkpoint

设 current state 为：

```text
[U, L0, L1, ..., Lm]
```

checkpoint 结果固定为：

```text
[U-new, U, L0, L1, ..., Lm]
```

其中：

- `U-new` 来自 request `upper_fd`；
- new work base 来自 `work_fd`；
- `U` 从 current layer 0 的 source path 创建新的 read-only private mount；
- `L0..Lm` 从 current state 依次派生；
- 用户态不能跳过、重排或替换 current chain；
- 已有 128 个 lower 时 checkpoint 返回 `-E2BIG`。

不能直接把 current writable private mount 改成 read-only，因为 retired current
state 仍持有该 mount，且 stale entry 的 layer 指针必须继续有效。target 中的 frozen
upper 必须使用单独 clone，并设置 `MNT_READONLY | MNT_NOATIME`。

### 3.3 Restore

restore fd 组布局固定为：

```text
fds[0]     fresh writable upper
fds[1]     fresh work base
fds[2..]   new lower prefix，按 top-to-bottom 排列
```

设：

```text
current_lower = [C0, C1, ..., Cn-1]
k             = keep_bottom
prefix        = fds[2..nr_fds-1]
suffix        = current_lower[n-k..n-1]
target_lower  = prefix + suffix
```

约束：

- `0 <= k <= current_numlower`；
- `nr_fds >= 2`；
- `nr_new_lower = nr_fds - 2`；
- `1 <= nr_new_lower + k <= DELTAFS_V2_MAX_LOWERS`；
- `k == 0` 表示 current lower 全部不保留；
- `nr_new_lower == 0` 合法，但此时 `k > 0`；
- current writable upper 永远不能通过 `keep_bottom` 保留；
- target slot 的 `idx` 按新 stack 位置重新填写，不能复制 old idx。

内核不解释 checkpoint ID 或 snapshot parent。full target chain 仍由 controller
manifest 持久化。

### 3.4 Generation

runtime generation 继续从 1 开始。每次成功 commit 恰好加 1；任意 build 或 final
validation 失败均不改变 generation。

UAPI 保留 `expected_generation`。它用于检测过期 manifest、上一次成功 ioctl 后的
controller 崩溃和两个错误请求的竞争，不因“单 controller”假设而删除。

## 4. v2 UAPI

### 4.1 定义

v2 直接替换实验性 v1 ABI，不提供双 ABI：

```c
/* include/uapi/linux/deltafs.h */
#define DELTAFS_ABI_VERSION              2
#define DELTAFS_V2_MAX_LOWERS            128
#define DELTAFS_V2_RESTORE_UPPER_FD      0
#define DELTAFS_V2_RESTORE_WORK_FD       1
#define DELTAFS_V2_RESTORE_LOWER_BASE    2
#define DELTAFS_V2_MAX_RESTORE_FDS       \
	(DELTAFS_V2_MAX_LOWERS + DELTAFS_V2_RESTORE_LOWER_BASE)
#define DELTAFS_IOC_MAGIC                0xdf

struct deltafs_ioc_checkpoint_v2 {
	__u32 size;
	__u32 version;
	__u64 flags;
	__u64 expected_generation;

	__s32 upper_fd;
	__s32 work_fd;
	__u64 reserved[4];
};

struct deltafs_ioc_restore_v2 {
	__u32 size;
	__u32 version;
	__u64 flags;
	__u64 expected_generation;

	__u32 keep_bottom;
	__u32 nr_fds;
	__s32 fds[DELTAFS_V2_MAX_RESTORE_FDS];
	__u64 reserved[4];
};

#define DELTAFS_IOC_CHECKPOINT \
	_IOW(DELTAFS_IOC_MAGIC, 0x01, struct deltafs_ioc_checkpoint_v2)
#define DELTAFS_IOC_RESTORE \
	_IOW(DELTAFS_IOC_MAGIC, 0x02, struct deltafs_ioc_restore_v2)
```

固定 native layout：

```text
sizeof(deltafs_ioc_checkpoint_v2) = 64
sizeof(deltafs_ioc_restore_v2)    = 584

checkpoint: upper_fd@24, work_fd@28, reserved@32
restore:    keep_bottom@24, nr_fds@28, fds@32, reserved@552
```

userspace layout test 必须使用 `_Static_assert` 固定 size 和主要 offset。

### 4.2 ABI 规则

- `size` 必须精确等于对应 request 大小；
- `version == 2`；
- `flags == 0`；
- `reserved[]` 全零；
- `expected_generation` 必须等于 current generation；
- fd 使用 `O_PATH | O_DIRECTORY | O_CLOEXEC` 打开；
- restore 的 `fds[nr_fds..MAX-1]` 必须全部为 `-1`；
- request 不含用户指针，只执行一次固定大小 `copy_from_user()`；
- ioctl 成功返回 0，不回写 request；
- 32 位 compat 程序不作为 v2 第一版验收目标。

v2 checkpoint 为 64 bytes，和 584-byte v1 switch request 的 encoded command 不同；
旧 checkpoint binary 得到未知 command 错误。v2 restore 和 v1 switch 都是 584 bytes，
restore command 数值相同；旧 restore request 必须在解释 offset 24 之后的字段前因
`version == 1` 返回 `-EINVAL`。

### 4.3 固定 fd 数组的理由

固定数组保持 v1 的三个优点：无二次用户指针读取、无 compat pointer 转换、错误时
容易完整释放。restore 最多传 2 个固定 fd 和 128 个 lower prefix fd，请求仍只有
584 bytes。

controller 和测试必须使用公开 fd index 宏，不复制数字 0、1、2。

## 5. Trusted controller 与非嵌套边界

v2 假定 ioctl 只由项目 controller 调用。controller 必须保证：

- upper、work 和所有 lower 目录不存在祖先/后代关系；
- layer 不位于 merged mount 内；
- snapshot manifest 无重复、无环、parent chain 完整；
- frozen layer 不被任何进程通过 backing path 修改；
- 同一 superblock 同时只有一个 controller 操作。

内核删除 v1 的 `is_subdir()` 两两扫描，也不扫描 active/retired state 检查输入路径与
旧 layer/workdir 的祖先、后代关系。

内核仍保留直接影响 builder 和内存安全的检查：

- fd 有效、是 `FMODE_PATH` 目录且未 unlink；
- fresh upper/work 可写、同一个 `vfsmount`、均为空且不是同一 root；
- 所有路径与固定 backing superblock 相同；
- 不接受 idmapped mount 或 OverlayFS backing path；
- fd prefix 中完全相同的 root 不重复，且不等于 fresh upper/work；
- feature 白名单和 target layer 数合法；
- new upper/work in-use lock 获取成功；
- restore `keep_bottom` 不越界。

父子目录嵌套属于 controller contract，不属于 kernel 支持边界。违反 contract 的
行为不作功能保证，但 kernel 仍不得发生数组越界、double free 或 UAF。

## 6. 内核数据模型

### 6.1 `struct ovl_layer` source path

概念上为每层增加：

```c
struct ovl_layer {
	/* existing fields */
	struct path delta_source;
	bool delta_source_valid;
};
```

所有可执行 DeltaFS ioctl 的 writable mount 都必须为 active layer 保存 source path。
初始 mount 在 clone private mount 时从 `fs_context` layer path 获取引用；v2 builder
从 UAPI path 或 current state path 获取引用。

所有权规则：

| 阶段 | private mount | source path | trap |
|---|---|---|---|
| Build | target state | target state | target state |
| Active | `ovl_fs` | `ovl_fs` layer slot | `ovl_fs` layer slot |
| Retired | retired state | retired layer slot | retired layer slot |

state teardown 对每个 valid source 执行一次 `path_put()`。mount error path 也必须覆盖
只取得 source、尚未 clone mount 的 partial slot。

### 6.2 `ovl_delta_state`

继续使用 v1 的完全所有权 state：

```c
struct ovl_delta_state {
	struct list_head node;
	u64 generation;

	unsigned int numlayer;
	struct ovl_layer *layers;

	struct dentry *workbasedir;
	struct dentry *workdir;
	struct dentry *whiteout;
	struct inode *workbasedir_trap;
	struct inode *workdir_trap;

	bool upperdir_locked;
	bool workdir_locked;
	bool no_shared_whiteout;

	char *upperdir_name;
	char *workdir_name;
	char **lowerdir_names;

	struct dentry *root_upperdentry;
	struct ovl_entry *root_oe;
	bool root_impure;
	bool root_xwhiteouts;
};
```

build、active、retired 使用同一 owner shape。commit 只移动 owner，不复制后双重释放。

### 6.3 `struct ovl_fs`

v2 继续保留：

```c
u64 delta_generation;
struct mutex delta_lock;
struct list_head delta_retired;
struct super_block *delta_backing_sb;
```

active `numlayer/layers/workdir/config/root` 字段维持 v1 布局，不引入新的 active-view
指针。`delta_lock` 保护 state snapshot、trap 复用、final validation 和 commit。

### 6.4 Retired state

old state 每次成功 commit 后加入 `delta_retired`，只在 unmount 释放。原因保持不变：

- stale `ovl_path.layer` 是指向 old layer array 的裸指针；
- stale upper/lower binding 仍可能在 inode/dentry cache 中存在；
- generation revalidation 只阻止继续使用 stale binding，不负责其 owner 回收。

v2 不尝试根据 dcache 状态回收 retired state。长期服务必须限制 switch 次数或定期
停机 remount。

## 7. Current-state snapshot 与 builder

### 7.1 Snapshot 原则

checkpoint 和 restore 都需要从 current state 派生一部分 layer。builder 不能在释放
`delta_lock` 后继续使用 active array 裸指针。

进入慢速 build 前，先在 `delta_lock` 下建立独立 input snapshot：

- 校验 expected generation 和 command-specific count；
- 对需要复用的 current source path 执行 `path_get()`；
- 对需要复用的 trap 执行 `igrab()`；
- 复制 `has_xwhiteouts` 等 per-layer 描述；
- 记录 current chain identity 供 final validation；
- 之后释放 `delta_lock`，只使用 snapshot owner 构建 target。

final commit 前重新获取 `delta_lock`。generation 仍等于 expected value时，current
state 必须仍是 snapshot 对应状态；否则返回 `-ESTALE` 并释放 target/snapshot。

### 7.2 通用 builder 流程

1. copy 和校验 UAPI；
2. 收集 user fd paths；
3. 在 `delta_lock` 下验证 feature/generation 并 pin current inputs；
4. 完成非嵌套 path、empty、same-sb 校验；
5. 预分配 target state、layers、root oe、names；
6. 构建 fresh upper/work；
7. 按 command 构建 target lower；
8. 构建 root binding 和 root flags 描述；
9. 最终加锁重检；
10. 无失败 commit；任意错误完整 unwind，active state 不变。

所有读取或修改 backing 对象的 VFS 操作继续使用 `ofs->creator_cred`。

### 7.3 Checkpoint builder

target 构造顺序：

1. layer 0 从 request `upper_fd` 构建 writable private mount；
2. layer 1 从 snapshot 中 current upper source path clone 为 read-only lower；
3. layer 2.. 从 snapshot current lowers 依次 clone；
4. current traps 通过 snapshot 的独立 `igrab()` 引用复用；
5. frozen upper 的 lower name在 controller rename 后从 source path 生成；
6. current lower names可以重新 `d_path()`，不依赖过期 config string；
7. target lower count 为 `current_numlower + 1`。

### 7.4 Restore builder

target 构造顺序：

1. layer 0 从 `fds[0]` 构建 writable upper；
2. work base 从 `fds[1]` 构建；
3. top lower prefix 从 `fds[2..nr_fds-1]` 构建；
4. bottom suffix 从 snapshot current lowers 最后 `keep_bottom` 层构建；
5. 所有 target lower private mounts 设置 read-only/noatime；
6. 所有 slot 按 target 顺序设置新 idx；
7. target lower count 为 `nr_fds - 2 + keep_bottom`。

prefix layer 可能对应 retired state 中已有 trap。builder 在 `delta_lock` 下按 backing
`(superblock, inode)` 扫描 active 和 `delta_retired`：找到则 `igrab()`，找不到才调用
`ovl_get_trap_inode()`。取消嵌套检查不等于取消 trap owner 复用。

### 7.5 Failure unwind

每个资源获取点继续保留 debug error-injection checkpoint。unwind 必须覆盖：

- user path refs 和 current snapshot path refs；
- private mount clones；
- source paths；
- layer/work traps；
- upper/work in-use locks；
- workbasedir/workdir/whiteout；
- root upper/root oe；
- config strings 和 state container。

work helper 已在 fresh work base 中创建的内部目录由 controller 删除整个 fresh branch
时清理，kernel free helper 不执行反向 VFS 删除。

## 8. 非 RCU commit 协议

### 8.1 前置条件

进入 commit 前必须已经完成所有可能失败的分配、clone、path lookup 和 root 状态计算。
commit 内部函数不返回错误。

final validation 在 `delta_lock` 下检查：

- control dentry 仍是 `sb->s_root`；
- expected generation 等于 `ofs->delta_generation`；
- generation 不为 `U64_MAX`；
- checkpoint current chain 未变化；
- restore `keep_bottom` 仍不越界；
- target state 与 captured current identity 一致。

### 8.2 锁序

固定锁序保持 v1：

```text
ofs->delta_lock
  -> root inode i_rwsem
    -> root ovl_inode.lock
```

workload quiescence 是调用前置条件。该锁序不承诺与任意普通 OverlayFS data path 并发
切换，只保证 control path writer、root 更新和 teardown owner 移动有确定顺序。

### 8.3 Commit 顺序

1. 把 current layers/numlayer 移入预分配 old state；
2. 把 current work、whiteout、trap 和 in-use lock owner 移入 old state；
3. 把 current config path strings 移入 old state；
4. 把 root old upper/root oe 移入 old state；
5. 把 target layer/work/config owner 安装到 `ovl_fs`；
6. 把 target root upper/root oe 安装到 root inode；
7. 更新 root attributes、flags 和 readdir version；
8. 设置 root inode generation；
9. 把 old state加入 `delta_retired`；
10. 最后 `smp_store_release()` 发布 new generation。

步骤 1 到 6 不能把 active owner 先清成 NULL。target 中移出的所有字段必须清零，使
target container 可以直接释放而不会释放已安装 owner。

### 8.4 Reader 与 cache 语义

继续使用 v1 generation cache 协议：

- 正 dentry 先比较 inode generation 与 current generation；
- mismatch + `LOOKUP_RCU` 返回 `-ECHILD`；
- mismatch + ref-walk 返回 0，VFS 重新 lookup；
- negative dentry lookup 完成后 `d_drop()`；
- inode cache key 为 `(real inode, generation)`；
- root inode 原地更新并递增 readdir version。

这里的 `LOOKUP_RCU` 是 VFS pathwalk 已有模式，不表示 v2 使用 RCU 发布 layer stack。
旧 layer array 的安全性仍由 retired-until-unmount 保证。

## 9. Controller 与持久化状态

### 9.1 Manifest format

`state.json` 和 `transaction.json` 使用 `format` 2。full
`active_lowers` 和 snapshot chain 仍保存完整路径；format bump 防止 v1 controller
误用新 UABI。

restore transaction 额外记录：

```json
{
  "format": 2,
  "operation": "restore",
  "from_generation": 7,
  "to_generation": 8,
  "keep_bottom": 2,
  "new_lower_prefix": ["layers/X"],
  "target_lowers": ["layers/X", "layers/A", "base"]
}
```

### 9.2 Checkpoint transaction

1. 锁 `controller.lock`，严格读取 format-2 manifest；
2. 确认 workload 已静止，打开 merged root，执行 `syncfs()`；
3. 计算 full target：`[new checkpoint layer] + active_lowers`；
4. durable 写入 transaction；
5. 创建并 fsync fresh `gN+1/{upper,work}`；
6. rename `branches/gN/upper -> layers/<id>` 并 fsync；
7. 只打开 fresh upper/work，填 checkpoint request；
8. ioctl 成功后原子写 state，删除 transaction；
9. ioctl 前失败时 rename 回旧位置并删除 fresh branch。

kernel 通过 current upper source path 观察 rename 后的同一 backing dentry。

### 9.3 Restore transaction

1. 从 snapshot 读取完整 target lower chain；
2. 计算 `active_lowers` 与 target 的最长公共后缀长度 `keep_bottom`；
3. 计算 `new_prefix = target[0 .. target_count - keep_bottom)`；
4. durable transaction 记录 full target、prefix、keep_bottom；
5. 创建 fresh branch；
6. 只打开 fresh upper/work 和 prefix lower fd；
7. 按 `[upper, work, prefix...]` 填 restore request；
8. ioctl 成功后写入完整 `active_lowers = target`；
9. ioctl 前失败时删除 fresh branch，active manifest 不变。

最长公共后缀按 controller 管理的规范相对路径比较。kernel 根据 captured current
suffix 和 fd prefix 构造最终 stack。

### 9.4 崩溃与升级边界

ioctl 返回成功仍是不可回滚点。成功后 state 更新或 fsync 失败只能保留 transaction
并 fail-stop，不能 rename 回旧 upper 或删除 active branch。

v1 sandbox 不支持在线升级为 v2。升级必须停 workload、卸载 v1 mount、显式迁移
manifest format，再使用 recorded active chain 重新 mount。

## 10. 错误语义

| errno | v2 条件 | active state |
|---|---|---|
| `-ENOIOCTLCMD` | 未知或旧 checkpoint command | 不变 |
| `-ENOTTY` | control fd 不是 merged root | 不变 |
| `-EPERM` | 缺少目标 user namespace `CAP_SYS_ADMIN` | 不变 |
| `-EFAULT` | request copy 失败 | 不变 |
| `-EINVAL` | ABI、fd 布局、重复 root、空 target 或 keep_bottom 越界 | 不变 |
| `-EBADF` | fd 无效 | 不变 |
| `-ENOTDIR` | fd 不是目录 | 不变 |
| `-ENOTEMPTY` | fresh upper/work 非空 | 不变 |
| `-EBUSY` | upper/work in-use lock 失败 | 不变 |
| `-EROFS` | 无 writable upper/work 或 mount 只读 | 不变 |
| `-EOPNOTSUPP` | feature、idmap、OverlayFS backing 等不支持 | 不变 |
| `-EXDEV` | 输入不在固定 backing superblock | 不变 |
| `-E2BIG` | target lower 超过 128 | 不变 |
| `-ESTALE` | expected generation 或 captured current state 已变化 | 不变 |
| `-EOVERFLOW` | generation 为 `U64_MAX` | 不变 |
| `-ENOMEM` | build/preallocation 失败 | 不变 |

任意失败都不能增加 generation、替换 active owner或向 retired list 添加节点。

## 11. 风险与取舍

### 11.1 主要风险

1. 初始 mount 和所有 builder error path 都必须平衡 source path 引用。
2. checkpoint 在 rename 后派生 current upper，必须使用 source path而不是 private
   mount root 的显示路径。
3. restore retained suffix 的 slot idx 会变化，不能直接 memcpy current layer slot。
4. historical prefix 的 trap 仍需在 active/retired states 中复用。
5. controller 与 kernel 对 `keep_bottom`、prefix 顺序的解释必须完全一致。

### 11.2 已接受的限制

- retired state 只在 unmount 释放；
- private mount、trap、路径字符串和内存随成功 switch 次数增长；
- layer ancestor/descendant 完全由 controller 保证；
- 不支持普通 filesystem 操作与 commit 并发；
- 不共享不同 state 的 private mount owner；
- 不在线删除 physical frozen layer。

### 11.3 性能预期

checkpoint 不再传或重新 `fget` 完整 lower fd 数组。restore 只传 target 与 current 的
差异前缀，fd 数和用户态 request 准备成本下降。

kernel 仍为 target 的派生 layer clone private mounts，因此 builder 成本与 target
depth 近似线性。mutex commit 本身只移动固定数量的 owner pointer，但完整 ioctl
不能宣称 O(1)。E2 必须记录 target depth、keep_bottom、prefix depth、request fd count
和 ioctl latency；controller 端到端耗时属于独立 benchmark，不能混入 E2 ioctl latency。

## 12. 验证策略

### 12.1 静态门禁

实现期间只在本开发环境执行静态验证：

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make M=fs/overlayfs W=1
make C=2 CHECK=sparse M=fs/overlayfs
make -C tools/deltafs clean v2-tools
make -C tools/deltafs test-v2-controller
make -C tools/deltafs check-v2-layout
make -C tools/deltafs check-v2-checkpoints CHECKPOINT_MODE=auto
for f in tools/deltafs/*.sh; do bash -n "$f"; done
```

还必须对代码变更运行 `scripts/checkpatch.pl --no-tree --strict`，并静态确认 v2 未引入
layer-stack RCU pointer、callback 或 reclaim worker。本环境不加载 `overlay.ko`、
不 mount DeltaFS，也不产生运行态结论。

### 12.2 测试入口

v2 不拆分 P1 到 P7 阶段脚本。旧 P5--P7 helper/harness 已删除；最终 HEAD 维护四个入口：

| 入口 | 运行位置 | 主要证明 |
|---|---|---|
| `check-v2-layout` | host | 两种 request 的 size、offset 和 fd index |
| `check-v2-checkpoints` | host | debug/production build 的 ownership fault-injection 调用点完整性 |
| `test-v2-controller` | host/guest | format-2 parser、最长公共后缀和失败补偿 |
| `deltafs_v2_acceptance_test.sh` | QEMU guest | kernel UABI、切换、cache、压力和 teardown |

acceptance harness 内部调用一个 native `deltafs_v2_ioctl_test` helper，并必须覆盖：

- checkpoint request 没有 lower fd；
- `keep_bottom=0/1/all/out-of-range`；
- prefix 为 0、1、8、32、64、128 层；
- target lower 1 和 128 成功，129 返回 `E2BIG`；
- A -> B -> C 后 restore A 使用公共 suffix；
- unrelated restore 使用 `keep_bottom=0`；
- 预热正/负 dentry 和 root readdir cache 后切换；
- builder 全部 owner 获取点故障注入；
- 多次 switch 后 retired state 保持有效，unmount 后全部释放；
- 反复 mount/unmount/module unload；
- dmesg 无 UAF、refcount、lockdep、KASAN/KFENCE/UBSAN 报告；
- kmemleak 无新增 DeltaFS owner 泄漏。

## 13. QEMU/KVM 完整测试交接

本节定义完整 v2 实现后的运行态 handoff。v2 layout、format-2 controller、native
ioctl binary 和总 acceptance 已在源码树中提供。本开发环境执行的静态门禁与用户在
QEMU debug guest 中执行的总验收均已通过（2026-08-15）。

第一阶段的独立静态与 QEMU 交接见
`docs/deltafs_v2_phase1_test.md`。第二阶段总入口为
`tools/deltafs/deltafs_v2_acceptance_test.sh`，它会额外执行 format-2 controller、
最长公共后缀、lower 边界和 teardown 验收。

guest 运行态由 `tools/deltafs/deltafs_v2_acceptance_test.sh` 一键编排；用户无需逐条
复制负向矩阵、checkpoint/restore 或 ownership fault-injection 命令。

### 13.1 Host 构建

```bash
cd /home/wangmingyu/repos/agentfs/fs/deltafs
make -j"$(nproc)" bzImage modules
make -C tools/deltafs clean v2-tools
make -C tools/deltafs check-v2-layout
```

debug kernel 至少启用：

```text
CONFIG_OVERLAY_FS=m
CONFIG_MODULE_UNLOAD=y
CONFIG_DEBUG_KERNEL=y
CONFIG_DEBUG_FS=y
CONFIG_DEBUG_KMEMLEAK=y
CONFIG_FUNCTION_ERROR_INJECTION=y
CONFIG_KASAN=y
CONFIG_KFENCE=y
CONFIG_UBSAN=y
CONFIG_PROVE_LOCKING=y
```

期望生成：

```text
arch/x86/boot/bzImage
fs/overlayfs/overlay.ko
tools/deltafs/deltafsctl
tools/deltafs/deltafs_v2_ioctl_test
tools/deltafs/deltafs_v2_acceptance_test.sh
```

### 13.2 启动 QEMU

准备 rootfs 和两个已经格式化、属于不同 superblock 的数据盘：

```bash
KERNEL=/home/wangmingyu/repos/agentfs/fs/deltafs/arch/x86/boot/bzImage
ROOTFS=/absolute/path/to/rootfs.qcow2
DATA1=/absolute/path/to/deltafs-v2-data1.raw
DATA2=/absolute/path/to/deltafs-v2-data2.raw

qemu-system-x86_64 \
  -enable-kvm -cpu host -smp 4 -m 4096 -nographic \
  -kernel "$KERNEL" \
  -append 'root=/dev/vda1 rw console=ttyS0 nokaslr kmemleak=on' \
  -drive if=virtio,format=qcow2,file="$ROOTFS" \
  -drive if=virtio,format=raw,file="$DATA1" \
  -drive if=virtio,format=raw,file="$DATA2" \
  -virtfs local,path=/home/wangmingyu/repos/agentfs/fs/deltafs,mount_tag=host,security_model=none
```

若 root 分区不是 `/dev/vda1`，只调整 `root=`。不要对未知或已挂载设备执行 `mkfs`。

### 13.3 Guest 准备与 mount 形状

```bash
mkdir -p /mnt/host /mnt/deltafs-v2/{disk1,disk2}
mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host
mount /dev/vdb /mnt/deltafs-v2/disk1
mount /dev/vdc /mnt/deltafs-v2/disk2
mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug

install -D -m 0644 /mnt/host/fs/overlayfs/overlay.ko \
  "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
depmod -a
ulimit -n 192

cd /mnt/host
make -C tools/deltafs v2-tools
```

各脚本创建并卸载自己的 mount。手工 smoke 的 mount 形状为：

```bash
R=/mnt/deltafs-v2/disk1/manual
rm -rf "$R"
mkdir -p "$R"/{base,branches/g1/upper,branches/g1/work,merged}
printf 'base\n' > "$R/base/state"
OPTS="lowerdir=$R/base,upperdir=$R/branches/g1/upper"
OPTS+=",workdir=$R/branches/g1/work,index=off,nfs_export=off"
OPTS+=",metacopy=off,xino=off,uuid=off,redirect_dir=nofollow"
mount -t overlay overlay -o "$OPTS" "$R/merged"
cat "$R/merged/state"
# 期望：base
umount "$R/merged"
```

### 13.4 Controller unit test 与总验收

```bash
mkdir -p /mnt/deltafs-v2/disk1/acceptance

make -C tools/deltafs test-v2-controller
# 期望：All DeltaFS v2 controller unit tests passed

sudo tools/deltafs/deltafs_v2_acceptance_test.sh \
  --backing-root /mnt/deltafs-v2/disk1/acceptance \
  --extra-backing-root /mnt/deltafs-v2/disk2
# 期望：All DeltaFS v2 acceptance checks passed
```

完整 acceptance summary 至少包含：

```text
target_lower_128=PASS
target_lower_129_e2big=PASS
restore_keep_bottom_0=PASS
restore_keep_bottom_1=PASS
restore_keep_bottom_all=PASS
checkpoint_derived_chain=PASS
retired_state_teardown=PASS
fault_injection_exhausted=PASS
module_unload_cycles=PASS
sanitizer_dmesg=PASS
kmemleak=PASS
```

### 13.5 日志与失败诊断

失败时保留脚本目录和 transaction，不手工删除 frozen layer：

```bash
OUT=/mnt/deltafs-v2/disk1/v2-failure-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
dmesg -T > "$OUT/dmesg-full.log"
findmnt -J > "$OUT/findmnt.json"
cat /proc/mounts > "$OUT/proc-mounts.txt"
cat /sys/kernel/debug/kmemleak > "$OUT/kmemleak.log" 2>/dev/null || true
cat /proc/config.gz > "$OUT/kernel-config.gz" 2>/dev/null || \
  cp "/boot/config-$(uname -r)" "$OUT/kernel-config"
uname -a > "$OUT/uname.txt"
grep -Ei 'deltafs|overlay|use-after-free|refcount|lockdep|BUG:|WARNING:' \
  "$OUT/dmesg-full.log" > "$OUT/dmesg-focus.log" || true
cp -a "$OUT" /mnt/host/
```

报告必须包含失败命令、退出码、stdout/stderr、`state.json`、`transaction.json`、
active/target chain、`keep_bottom`、`nr_fds` 和故障注入序号。

## 14. 完成标准

v2 只有同时满足以下条件才可完成：

1. checkpoint UAPI 不再接收 lower fd，target 严格派生自 current state。
2. restore 按 prefix + current bottom suffix 构造 target，边界 errno 固定。
3. layer stack 由 `delta_lock` 和 root locks 提交，不包含 RCU publication。
4. old layer arrays 到 unmount 前保持有效，retired teardown 无泄漏或 double free。
5. source path 在 initial/build/active/retired/error path 中所有权平衡。
6. 所有失败在 commit 前结束，generation、active state 和 retired list 不变。
7. format-2 controller 事务、补偿和崩溃 fail-stop 通过测试。
8. 1..128 lower、cache、branch、故障注入和反复卸载通过 QEMU 总验收。
9. 静态构建、sparse、checkpatch、userspace `-Werror` 全部通过。
10. 文档明确保留 layer 嵌套、并发、旧 fd/mmap、在线回收和 GC 限制。
