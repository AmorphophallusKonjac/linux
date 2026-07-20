# DeltaFS v1 详细设计

> 状态：设计定稿，待实现
>
> 基线：Linux 6.8.0，OverlayFS 代码位于 `fs/overlayfs/`
> 范围：单线程、单 OverlayFS、无跨切换打开文件的最小可用版本

## 1. 文档目的

本文定义 DeltaFS v1 的用户可见语义、ioctl ABI、内核数据结构、layer 构建与提交协议、generation 缓存失效机制、用户态 controller 流程以及验收标准。

DeltaFS v1 是对 OverlayFS 的运行时重配置扩展，不是新的磁盘文件系统。它允许一个已经挂载的 OverlayFS 在不执行 umount/mount 的情况下：

- checkpoint：把当前 writable upper 冻结为最上层 read-only lower，并安装 fresh upper/work；
- restore：加载目标 checkpoint 的 lower chain，并安装 fresh upper/work；
- 通过单调递增的 runtime generation，使切换前缓存的 dentry/inode 在下一次路径访问时重新解析。

本文是实现约束。实现若偏离本文定义的 ABI、状态语义或失败原子性，需要先更新设计并重新评审。

## 2. 设计目标与非目标

### 2.1 v1 目标

DeltaFS v1 必须满足：

1. 所有 OverlayFS 实例默认初始化 DeltaFS generation 和 ioctl 能力，不提供 `deltafs=on/off` 挂载开关。
2. checkpoint 和 restore 均通过 merged root directory fd 上的 ioctl 发起。
3. 每次切换先在 active view 之外完整构建新 layer 状态，成功后一次性提交。
4. 成功提交使 runtime generation 恰好加一；任何提交前错误不得改变 active view。
5. root `ovl_inode` 在提交时原地更新；普通 stale inode 通过 VFS revalidation 和重新 lookup 替换。
6. frozen layer 在 checkpoint/restore 后保持不可变，后续写入只进入 fresh upper。
7. restore 可以选择任意已记录 checkpoint，并从该状态创建新分支。

### 2.2 v1 非目标

v1 不实现或不保证：

- 普通文件 fd、目录 fd、`cwd`、进程 root、mmap 跨切换继续使用；
- writable `MAP_SHARED`；
- checkpoint/restore 与路径查找、copy-up 或写 syscall 并发；
- io_uring、异步 I/O 或在途 direct I/O 的 drain；
- NFS export、open-by-handle、metacopy、index、xino、data-only layer；
- 在线回收 retired state 或 backing layer；
- controller 崩溃、内核崩溃或掉电后的自动事务恢复；
- layer squash、压平或超过 v1 层数上限后的 GC；
- 复现论文完整的旧 fd lazy switch 或 mmap 语义。

上述限制不是暂时忽略的正确性问题，而是 v1 的调用前置条件。Controller 在调用 ioctl 前必须保证目标工作负载已经静止并释放相关引用。

## 3. 状态语义

### 3.1 Checkpoint 表示的状态

一个 checkpoint 保存的是刚刚被冻结的 upper 及其祖先 lower chain：

```text
初始：
    RW upper-A
    RO base

checkpoint A 后：
    RW upper-B
    RO layer-A
    RO base

checkpoint B 后：
    RW upper-C
    RO layer-B
    RO layer-A
    RO base

restore A 后：
    RW upper-A-prime
    RO layer-A
    RO base
```

`layer-A` 一旦成为 checkpoint layer，就不能再次作为 writable upper。Restore 必须创建 `upper-A-prime`，否则恢复后的写入会污染 checkpoint A。

### 3.2 两种版本号

设计中区分：

- checkpoint ID：用户态持久化名称，例如 `cp-000123`；
- runtime generation：内核中当前 layer view 的单调递增编号。

恢复到历史 checkpoint 不会恢复旧 runtime generation。假设 checkpoint A 最初位于 generation 2，稍后从 generation 7 restore A，成功后的 runtime generation 是 8，而不是 2。这样可以确保此前任何 inode cache 都不会被误认为属于当前 view。

### 3.3 默认开启的含义

本实现不增加 mount option，也不在 `struct ovl_config` 中增加 DeltaFS 布尔字段。

每个 OverlayFS superblock 都会：

- 将 `delta_generation` 初始化为 1；
- 初始化 DeltaFS ioctl mutex 和 retired-state list；
- 给新建的普通 overlay inode 记录当前 generation；
- 给正 dentry 启用 generation revalidation；
- 在 merged directory file operations 中注册 DeltaFS ioctl。

默认开启不等于每一种 OverlayFS 配置都可以执行切换：

- lower-only 或只读 OverlayFS 调用切换返回 `-EROFS`；
- feature 组合不在 v1 白名单时返回 `-EOPNOTSUPP`；
- backing filesystem 布局不满足同 superblock 条件时返回 `-EXDEV` 或 `-EOPNOTSUPP`。

普通 OverlayFS 操作仍可使用这些挂载；限制只在调用 DeltaFS ioctl 时检查。

## 4. v1 支持边界

### 4.1 文件系统与 feature 要求

执行任一 DeltaFS ioctl 前必须满足：

```text
upperdir present
workdir present
superblock writable
index=off
nfs_export=off
metacopy=off
xino=off
uuid=off
redirect_dir=nofollow
volatile=off
numdatalayer=0
```

初始 upper、work、所有 lower，以及以后传入的 upper/work/lower 必须位于同一个 backing `struct super_block`。推荐使用启用 reflink 的 XFS；内核只强制同 superblock，不硬编码 filesystem type。非 reflink filesystem 仍可保证功能正确，但 copy-up 可能退化为整文件复制。

v1 拒绝 idmapped backing mount 和嵌套 OverlayFS backing path。

### 4.2 层数限制

v1 最多接受 64 个 lower：

```text
1 upper + 1..64 lower
```

`nr_lower == 0` 返回 `-EINVAL`，`nr_lower > 64` 返回 `-E2BIG`。该上限是 v1 UAPI 的一部分，低于 OverlayFS 内部的 `OVL_MAX_STACK`。

### 4.3 静止条件

调用 checkpoint/restore 时允许存在的唯一 OverlayFS file 是发起 ioctl 的 merged root control fd。该 fd 只能用于 ioctl，提交后直接关闭，不能继续执行 readdir、fsync 或相对路径操作。

不得存在：

- 指向 merged mount 的普通文件 fd 或目录 fd；
- 位于 merged mount 内的 cwd 或进程 root；
- 指向 merged backing file 的 VMA；
- 在途 syscall、copy-up、writeback 请求提交或异步 I/O。

允许 checkpoint 前已经产生的 dirty data 在 `syncfs()` 中完成写回，因为它属于即将被冻结的 checkpoint 内容。

## 5. 用户态 ABI

### 5.1 UAPI 定义

新增 `include/uapi/linux/deltafs.h`：

```c
#ifndef _UAPI_LINUX_DELTAFS_H
#define _UAPI_LINUX_DELTAFS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DELTAFS_ABI_VERSION       1
#define DELTAFS_V1_MAX_LOWERS     64
#define DELTAFS_IOC_MAGIC         0xdf

struct deltafs_ioc_switch_v1 {
        __u32 size;
        __u32 version;
        __u64 flags;
        __u64 expected_generation;

        __s32 upper_fd;
        __s32 work_fd;
        __u32 nr_lower;
        __u32 reserved0;

        __s32 lower_fds[DELTAFS_V1_MAX_LOWERS];
        __u64 reserved[4];
};

#define DELTAFS_IOC_CHECKPOINT \
        _IOW(DELTAFS_IOC_MAGIC, 0x01, struct deltafs_ioc_switch_v1)
#define DELTAFS_IOC_RESTORE \
        _IOW(DELTAFS_IOC_MAGIC, 0x02, struct deltafs_ioc_switch_v1)

#endif
```

这是本项目的 clean-room ABI，不声称与论文原型的未公开 ioctl 布局相同。

### 5.2 ABI 约束

调用者必须：

- 用 `O_RDONLY | O_DIRECTORY | O_CLOEXEC` 打开 merged root control fd；
- 用 `O_PATH | O_DIRECTORY | O_CLOEXEC` 打开 upper、work base 和 lower；
- 设置 `size = sizeof(struct deltafs_ioc_switch_v1)`；
- 设置 `version = DELTAFS_ABI_VERSION`；
- 设置 `flags = 0`、`reserved0 = 0`、`reserved[] = 0`；
- 将 `lower_fds[nr_lower..63]` 全部设置为 `-1`；
- 提供精确的 `expected_generation`。

固定 fd 数组避免：

- 用户指针生命周期和二次 copy-from-user；
- 32/64 位 compat 指针转换；
- 大型 `PATH_MAX` 字符串矩阵；
- 路径字符串解析和 rename TOCTOU。

两个 ioctl 都使用 `_IOW`，内核不在成功提交后写回用户内存。成功返回 0 即表示：

```text
new_generation = expected_generation + 1
```

### 5.3 ioctl 入口

`ovl_dir_operations` 增加 `unlocked_ioctl` 和等价 compat 入口。Dispatcher 的顺序是：

1. 未识别命令返回 `-ENOIOCTLCMD`；
2. control fd 的 dentry 不是 `sb->s_root` 时返回 `-ENOTTY`；
3. 调用者在 `sb->s_user_ns` 中不具备 `CAP_SYS_ADMIN` 时返回 `-EPERM`；
4. `copy_from_user()` 固定结构并校验 ABI；
5. 检查 mount 能否执行 DeltaFS v1；
6. 构建和提交 target state。

## 6. 用户态目录与 manifest

推荐每个 sandbox 使用独立目录：

```text
<sandbox-root>/
    base/
    layers/
        cp-000001/
        cp-000002/
    branches/
        g1/
            upper/
            work/
        g2/
            upper/
            work/
    meta/
        state.json
        transaction.json
        controller.lock
    merged/
```

其中 `branches/gN/work` 是传给 ioctl 的 work base；OverlayFS 可在其下创建内部 `work` 目录。

`state.json` 的 v1 schema：

```json
{
  "format": 1,
  "kernel_generation": 3,
  "active_branch": "g3",
  "snapshots": {
    "cp-000002": {
      "lowers": [
        "layers/cp-000002",
        "layers/cp-000001",
        "base"
      ]
    }
  }
}
```

规则：

- 所有路径相对 `<sandbox-root>` 保存，controller 打开后转成 directory fd；
- checkpoint 保存 immutable lower chain，不保存一个未来可写的 checkpoint upper；
- active branch 只表示当前临时 RW upper/work；
- generation 是 runtime CAS 值，不是 snapshot ID；
- controller 使用 `controller.lock` 保证单实例操作。

## 7. 内核数据模型

### 7.1 `struct ovl_fs` 扩展

概念字段：

```c
struct ovl_fs {
        /* existing fields */

        u64 delta_generation;
        struct mutex delta_lock;
        struct list_head delta_retired;
        struct super_block *delta_backing_sb;
};
```

初始化位置应在 `ovl_fs` 分配成功后立即执行，使后续任意 mount error path 都可以安全调用 teardown：

```c
ofs->delta_generation = 1;
mutex_init(&ofs->delta_lock);
INIT_LIST_HEAD(&ofs->delta_retired);
```

成功建立 writable upper 后：

```c
ofs->delta_backing_sb = ovl_upper_mnt(ofs)->mnt_sb;
```

lower-only mount 保持 `delta_backing_sb == NULL`。

### 7.2 `struct ovl_inode` 扩展

新增：

```c
u64 delta_generation;
```

不能复用 `ovl_inode.version`，后者已经用于 merge-directory readdir cache。

普通 overlay inode 必须在加入 inode cache 前记录 generation：generation-aware `iget5` set callback 同时设置 `i_private` 和 `delta_generation`。对不经过 `iget5` 的 unhashed/root inode，`ovl_inode_init()` 再记录 acquire-load 的当前 generation。`ovl_inode_init()` 不得覆盖一个已经由 cache key 确定的 generation。Trap inode不代表可见 overlay dentry，不参与 generation revalidation。

### 7.3 Build/retired state

概念结构：

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
};
```

实际字段命名可遵守 OverlayFS 现有风格，但所有权必须满足下表：

| 资源 | Build 阶段 | Active 阶段 | Retired 阶段 |
|---|---|---|---|
| layer array/private mounts/traps | new state 拥有 | `ovl_fs` 拥有 | retired state 拥有 |
| workbasedir/workdir/whiteout | new state 拥有 | `ovl_fs` 拥有 | retired state 拥有 |
| config 路径字符串 | new state 拥有 | `ovl_fs.config` 拥有 | retired state 拥有 |
| root upper/root `ovl_entry` | new state 拥有 | root `ovl_inode` 拥有 | retired state 拥有 |
| `ofs->fs`/pseudo dev | 不新建 | `ovl_fs` 静态拥有 | 不拥有 |

提交操作是所有权移动，不是复制后由两侧共同释放。

### 7.4 保持静态的字段

同 backing superblock 限制使以下字段不随 view 改变：

- `ofs->fs` 和 `ofs->numfs`；
- `xino_mode`；
- `namelen` 的能力上限；
- backing superblock error sequence；
- creator credentials；
- upper filesystem 对 tmpfile、xattr、file handle 的基础能力。

每个动态 layer 使用：

```text
fsid = 0
fs = &ofs->fs[0]
idx = layer 在新数组中的索引
```

## 8. Target state 构建

### 8.1 构建总原则

新状态必须在 active `ovl_fs` 之外构建。构建函数只能读取 current state，不能临时替换 `ofs->layers` 来复用 mount helper。

允许构建阶段修改的外部对象只有 controller 刚创建的 fresh work base，例如在其下创建内部 `work` 目录。构建失败时 active merged view 不变；controller 负责删除 fresh branch。

### 8.2 fd 转稳定 path

对 upper/work/lower fd：

1. `fdget()` 取得 file；
2. 检查 `S_ISDIR(file_inode(file)->i_mode)`；
3. 复制 `file->f_path` 并 `path_get()`；
4. `fdput()`；
5. 构建结束后统一 `path_put()`。

关闭用户 fd 不影响内核构建，因为 builder 已持有 path 引用。

### 8.3 通用验证

验证必须在 clone mount 前完成可提前完成的部分：

- `nr_lower` 在 `[1, 64]`；
- upper/work 不是只读 mount；
- upper/work 位于同一个 `vfsmount`，避免 idmap 和 mount identity 不一致；
- 所有 backing `mnt_sb == ofs->delta_backing_sb`；
- 任一路径不是当前 OverlayFS superblock；
- 路径没有重复；
- upper、work、各 lower 两两不为祖先/后代；
- upper/work 不等于 active 或 retired state 中仍持有的 root；
- lower 不等于此次 fresh upper/work；
- restore 的 lower 不等于当前 active upper；
- layer 数未超过 OverlayFS 和 DeltaFS 双重上限。

Fresh upper 和 work base 由 controller 通过唯一名称和 `mkdirat()` 创建。内核额外用 directory iteration 验证两者在构建开始时为空；work helper 创建内部目录后不再要求 work base 为空。

### 8.4 Feature 验证

统一 helper 在每个 ioctl 中检查：

```c
!ofs->config.index
!ofs->config.nfs_export
!ofs->config.metacopy
ofs->config.xino == OVL_XINO_OFF
ofs->config.uuid == OVL_UUID_OFF
ofs->config.redirect_mode == OVL_REDIRECT_NOFOLLOW
!ofs->config.ovl_volatile
ofs->numdatalayer == 0
ofs->numfs == 1
ofs->xino_mode == 0
```

任一不满足返回 `-EOPNOTSUPP`，不自动修改 mount feature。

### 8.5 Private mounts 和 traps

新 upper：

- 调用 `clone_private_mount()`；
- 清除继承的 atime flags；
- 保持 writable；
- 设置 `idx=0, fsid=0`。

每个 lower：

- 单独调用 `clone_private_mount()`；
- 设置 `MNT_READONLY | MNT_NOATIME`；
- 按目标顺序设置 `idx=1..nr_lower`；
- 设置 `fsid=0, fs=&ofs->fs[0]`。

历史 checkpoint root 通常已经有 OverlayFS trap。直接再次调用 stock `ovl_get_trap_inode()` 会把合法 restore 误判为 layer-root 冲突。因此 builder 应：

1. 按 `(backing sb, root inode)` 在 active 和 retired states 中查找相同 layer root；
2. 找到时对已有 trap 执行 `igrab()`，让新 state 获得独立引用；
3. 未找到时才创建新 trap；
4. 每个 state 在释放时对自己持有的 trap 执行一次 `iput()`。

每个 state 使用独立 private mount clone，不共享 kern mount 所有权，简化 teardown。

### 8.6 Root binding

构建阶段分配新的 root `ovl_entry`：

```text
root_oe->__numlower = nr_lower
root_oe->__lowerstack[i].layer = &new_layers[i + 1]
root_oe->__lowerstack[i].dentry = dget(new_layers[i + 1].mnt->mnt_root)
```

同时持有：

```text
root_upperdentry = dget(new_layers[0].mnt->mnt_root)
```

Builder 复用 `ovl_get_root()` 的 whiteout/xwhiteout 检查逻辑计算新 root 所需 flags，但不创建新的 overlay root inode。

### 8.7 配置显示字符串

对 fd path 使用 `d_path()` 生成用于 `show_options` 的字符串。新 `config.lowerdirs` 使用 stock `lowerdir+` 表示方式：

```text
lowerdirs[0] = NULL
lowerdirs[1..nr_lower] = 每个 lower 的路径字符串
```

数组长度与 `numlayer == nr_lower + 1` 一致。旧字符串在提交时随 old state 退休。

### 8.8 预分配要求

进入 commit 前必须已经准备好：

- new layers 和全部 mount/trap 引用；
- workdir 及相关 capability 检查；
- new root binding；
- 配置路径字符串；
- retired-state 容器；
- generation 新值；
- root flags 更新描述。

此后不得再发生可能失败的分配或路径解析。

## 9. Checkpoint 与 restore 专用校验

### 9.1 Checkpoint

当前状态：

```text
layers[0]     = current upper
layers[1..N]  = current lowers
numlayer      = N + 1
```

Checkpoint target lower chain 必须为：

```text
lower_fds[0]    = rename 后的 current upper
lower_fds[1..N] = current layers[1..N]
nr_lower        = current numlayer
```

比较 private mount 与用户 fd 时不能比较 `vfsmount *`，因为前者是 private clone。应比较 backing superblock 和 directory inode；目录不存在 hardlink，因此该组合可以标识同一个 root。

这些校验防止 controller 跳过当前 upper、改变祖先顺序或把无关目录冒充 checkpoint。

### 9.2 Restore

Restore 接受 manifest 提供的完整 immutable lower chain。内核不解释 checkpoint ID，也不维护用户态 snapshot tree。

Restore 额外拒绝：

- 目标 lower 中包含 current active upper；
- 目标 lower 中包含此次 fresh upper/work；
- 重复 layer；
- 任意两个 layer 互相重叠。

历史 layer 是否属于 committed manifest 由具备 `CAP_SYS_ADMIN` 的 controller 保证。

## 10. 原子提交协议

### 10.1 提交锁与重检

构建完成后获取 `ofs->delta_lock`，并重新检查：

- `expected_generation == smp_load_acquire(&ofs->delta_generation)`；
- generation 不是 `U64_MAX`；
- control dentry 仍是 `sb->s_root`；
- checkpoint 命令的 current-chain invariant 仍成立。

虽然 v1 声明单 controller，generation CAS 仍用于检测 manifest 与内核状态失配，并为后续版本保留清晰语义。

### 10.2 Commit 顺序

Commit 是一个不返回错误的内部函数，按以下顺序移动所有权：

1. 将当前 `ofs->layers/numlayer` 移入预分配 old state。
2. 将当前 workbasedir、workdir、whiteout、traps 和 in-use lock 状态移入 old state。
3. 将当前 `config.upperdir/workdir/lowerdirs` 移入 old state。
4. 获取 root `ovl_inode.lock`。
5. 将 root inode 的旧 `__upperdentry` 和旧 `oe` 移入 old state。
6. 把 new state 的 layer/work/config 字段安装到 `ovl_fs`。
7. 把 new root upper 和 `ovl_entry` 安装到 root `ovl_inode`。
8. 依据新 root backing 更新 root inode attributes 和 state-derived flags。
9. 递增 root inode 的 readdir `version`，使 root directory cache 失效。
10. 设置 root inode `delta_generation = new_generation`。
11. 释放 root inode lock。
12. 将 old state 加入 `delta_retired`。
13. 用 `smp_store_release()` 最后发布 `ofs->delta_generation`。

全局 generation 最后发布，保证后续 acquire-load 观察到新 generation 时，也能观察到完整的新 layer 和 root binding。

### 10.3 Root flags

Root 不重新创建 inode。更新 helper 应镜像 `ovl_get_root()` 中与 backing view 相关的初始化：

- root 始终设置 connected 和 whiteout 能力；
- 有新 upper 时设置 upper alias/upper data；
- 根据新 upper 更新 impure 状态；
- 根据 lower roots 更新 xwhiteout 标志；
- 调用 `ovl_copyattr()` 从新 real inode更新可见 attributes；
- 重新初始化 real-dentry revalidation flags。

不得清除与动态 view 无关的通用 inode 状态。

## 11. Generation 缓存协议

### 11.1 正 dentry revalidation

所有正 OverlayFS dentry 强制保留 `DCACHE_OP_REVALIDATE`。在 `ovl_dentry_revalidate_common()` 访问 upper/lower backing 前检查：

```c
inode_gen = READ_ONCE(OVL_I(inode)->delta_generation);
fs_gen = smp_load_acquire(&ofs->delta_generation);
```

处理规则：

| 条件 | 返回值/行为 |
|---|---|
| generation 相同 | 继续 stock OverlayFS real-dentry revalidation |
| 不同且 `LOOKUP_RCU` | 返回 `-ECHILD`，转 ref-walk |
| 不同且 ref-walk | 返回 0，让 VFS invalid dentry 并重新 lookup |

generation mismatch 分支必须在解引用 stale `oi->oe`、`ovl_path.layer` 或 `__upperdentry` 前返回。

### 11.2 慢路径

VFS 失效 stale dentry 后，从最近的 current-generation parent 重新调用 `ovl_lookup()`：

1. 从当前 parent `ovl_entry` 和当前 `ofs->layers` 查找 upper/lower；
2. 按 stock OverlayFS whiteout/opaque 规则构造新 stack；
3. 建立新 overlay inode；
4. 记录当前 runtime generation；
5. 将新 inode/dentry 返回给 VFS。

v1 不在 stale inode 上原地替换 backing。没有跨切换 fd/cwd 的前置条件保证旧 inode 不再作为操作入口。

### 11.3 Inode cache key

现有 OverlayFS 主要按 real inode 指针查找 overlay inode。v1 在普通 inode 的 cache test 中加入 generation：

```c
struct ovl_inode_cache_key {
        struct inode *realinode;
        u64 generation;
};
```

Hash 仍可使用 `realinode` 指针；test 同时比较：

```text
inode->i_private == key.realinode
OVL_I(inode)->delta_generation == key.generation
```

generation-aware set callback 必须在 inode 对其他 cache lookup 可见前同时写入：

```text
inode->i_private = key.realinode
OVL_I(inode)->delta_generation = key.generation
```

`ovl_alloc_inode()` 先把 `delta_generation` 初始化为 0；cache set callback 或 unhashed/root inode 的 `ovl_inode_init()` 再赋予有效值。这样同一 backing inode 在多个 generation 中可以对应多个 overlay inode，也不会出现 inode 已进入 hash、generation 仍为 0 的窗口。Trap inode继续使用原有 test/set helper，避免把 generation 逻辑混入 layer-root trap。

### 11.4 负 dentry

Negative dentry 没有 inode，无法读取 inode generation。v1 不为 `d_fsdata` 引入新的分配对象，而是在 `ovl_lookup()` 得到 negative 结果时调用 `d_drop()`。

结果是：

- ENOENT 不跨 syscall 长期缓存；
- checkpoint/restore 后不会返回旧 generation 的 negative lookup；
- 代价是所有 OverlayFS mount 上负路径查找性能下降，作为 v1 已知取舍记录。

### 11.5 Root inode 例外

`sb->s_root` 不能像普通 dentry 一样丢弃重建，因此 root inode 在 commit 中原地更新并直接设置新 generation。Control fd 虽然跨越 commit，但只执行当前 ioctl，返回后立即关闭。

## 12. Retired state 生命周期

### 12.1 为什么 v1 不立即释放

普通 `ovl_entry` 的 `ovl_path.layer` 是指向 `ofs->layers[]` 元素的裸指针。即使 dentry 已被标记 stale，缓存 inode 仍可能在 shrink 前存活。

v1 通过保留整个 old state 到 unmount，保证：

- stale `ovl_path.layer` 始终指向有效数组；
- stale upper dentry 对应的 old upper private mount 仍存活；
- trap、workdir 和配置字符串不会提前释放；
- 不需要在 v1 引入 RCU grace period与长期 view refcount 的组合协议。

### 12.2 释放顺序

Unmount 时，在 OverlayFS inode/dentry 回收完成后，对每个 retired state：

1. `dput(root_upperdentry)`；
2. `ovl_free_entry(root_oe)`，先释放 root lower dentry 引用；
3. `dput(whiteout)` 和内部 `workdir`，并 `iput()` work traps；
4. 若持有 work in-use lock，先对 `workbasedir` 解锁，再 `dput(workbasedir)`；
5. 若持有 upper in-use lock，在 upper private mount 仍存活时对其 root 解锁；
6. 对每层 trap 执行 `iput()`；
7. `kern_unmount_array()` 释放该 state 的 private mounts；
8. 释放 upper/work/lower 路径字符串、layer array 和 state。

随后由 stock `ovl_free_fs()` 的对应逻辑释放 active state，最后再释放共享的 `ofs->fs`、pseudo device 和 creator credentials。

Retired-state free helper不得释放 `ofs->fs` 或 anonymous bdev，因为它们由整个 OverlayFS superblock 共享。

### 12.3 内存代价

v1 的内核内存和 private mount 数量随切换次数与 layer depth 增长。Restore 不增加 lower depth，但仍新增一个 retired view。该行为是明确的 v1 限制；长时间运行或启用 GC 前不得把 v1 用于无界 checkpoint 服务。

## 13. Controller 流程

### 13.1 公共准备

每次命令执行：

1. 对 `meta/controller.lock` 获取 `flock(LOCK_EX)`；
2. 读取并严格校验 `state.json`；
3. 确认目标 workload 已满足静止条件；
4. 打开 merged root control fd；
5. 调用 `syncfs(merged_fd)`；
6. 使用唯一 generation/UUID 名称创建 fresh branch；
7. 以 `O_PATH|O_DIRECTORY` 打开 backing directories；
8. 填充全部未使用 fd 槽为 `-1`。

Controller 以 C 实现 ioctl、rename、fd 和 fsync 的核心路径；测试编排可以使用 shell 或 Python。

### 13.2 Checkpoint

假设当前 active branch 是 `gN`：

1. 验证 checkpoint ID 不存在。
2. 创建 `branches/gN+1/upper` 和 `branches/gN+1/work`。
3. 写入 `transaction.json`，状态为 `preparing-checkpoint`。
4. 将 `branches/gN/upper` rename 为 `layers/<checkpoint-id>`。
5. fsync `branches/gN`、`layers` 及其父目录。
6. 组成 target lower chain：`[new frozen layer] + old lower chain`。
7. 调用 `DELTAFS_IOC_CHECKPOINT(expected_generation=N)`。
8. 成功后将新 snapshot、active branch 和 `kernel_generation=N+1` 写入临时 state 文件。
9. fsync 临时文件，rename 覆盖 `state.json`，再 fsync `meta`。
10. 删除 `transaction.json`，释放 controller lock。

ioctl 失败时：

- 当前内核 view 保持不变；
- 将 frozen layer rename 回 `branches/gN/upper`；
- 删除 fresh branch；
- 删除 transaction 文件；
- 返回原始 errno。

### 13.3 Restore

1. 查找目标 checkpoint 的 immutable lower chain。
2. 创建全新的 `branches/gN+1/upper` 和 `work`。
3. 写入 `transaction.json`，状态为 `preparing-restore`。
4. 打开目标 lower chain 的 directory fds。
5. 调用 `DELTAFS_IOC_RESTORE(expected_generation=N)`。
6. 成功后更新 active branch 和 `kernel_generation=N+1`。
7. 原 active branch 标记为 retired，但不删除。
8. 原子替换 `state.json`，删除 transaction 文件。

ioctl 失败时只删除 fresh branch，当前 active branch 与 manifest 不变。

### 13.4 Controller 崩溃边界

v1 不提供 `GET_STATE` ioctl，也不承诺掉电事务恢复。若 ioctl 已返回成功，但 controller 在提交 `state.json` 前失败：

- `transaction.json` 必须保留；
- 后续命令必须拒绝继续操作；
- 管理员需要卸载并依据 manifest/transaction 选择明确的 layer chain重新挂载；
- 工具不得猜测 ioctl 是否提交，也不得自动删除任何相关目录。

这保证故障可见，但不属于自动恢复。

## 14. 错误语义

| errno | 条件 | Active view 是否改变 |
|---|---|---|
| `-ENOIOCTLCMD` | 未知 ioctl | 否 |
| `-ENOTTY` | 已知 DeltaFS ioctl 但 control fd 不是 merged root | 否 |
| `-EPERM` | 缺少目标 user namespace 的 `CAP_SYS_ADMIN` | 否 |
| `-EFAULT` | 固定参数无法从用户空间复制 | 否 |
| `-EINVAL` | ABI、reserved、fd 顺序、重复/重叠路径或 checkpoint chain 错误 | 否 |
| `-EBADF` | 任一 fd 无效 | 否 |
| `-ENOTDIR` | 任一 fd 不是目录 | 否 |
| `-EROFS` | OverlayFS 无 writable upper/work 或 superblock 只读 | 否 |
| `-EOPNOTSUPP` | feature、idmap、嵌套布局或其他 v1 不支持条件 | 否 |
| `-EXDEV` | backing path 不在 `delta_backing_sb` | 否 |
| `-E2BIG` | lower 超过 64 | 否 |
| `-ESTALE` | expected generation 不匹配 | 否 |
| `-EOVERFLOW` | generation 已为 `U64_MAX` | 否 |
| `-ENOMEM` | build/preallocation 失败 | 否 |

一旦 commit 开始，内部函数没有失败返回。ioctl 返回 0 时 active view 已完整切换。

## 15. 代码组织建议

主要实现边界：

- `include/uapi/linux/deltafs.h`：公开 ABI；
- `fs/overlayfs/deltafs.c`：ioctl、validation、state builder、commit、retired free；
- `fs/overlayfs/ovl_entry.h`：generation 与 DeltaFS state 字段；
- OverlayFS 的 super/inode/namei/readdir 集成点：初始化、root 更新、generation-aware inode lookup、dentry revalidation 和 directory ioctl hook；
- 用户态工具目录：controller、manifest 和 CLI；
- OverlayFS selftests：ABI 负向测试、cache、checkpoint/restore 和 teardown。

关键代码锚点：

| 目标 | Linux 6.8 现有符号 |
|---|---|
| superblock 初始化 | `ovl_init_fs_context()`, `ovl_fill_super()` |
| root binding | `ovl_get_root()` |
| mount/layer 构建参考 | `ovl_get_upper()`, `ovl_get_layers()`, `ovl_get_lowerstack()` |
| dentry revalidate | `ovl_dentry_revalidate_common()` |
| inode 初始化/cache | `ovl_inode_init()`, `ovl_get_inode()`, `ovl_iget5()` |
| 正/负 lookup | `ovl_lookup()` |
| directory ioctl hook | `ovl_dir_operations` |
| teardown | `ovl_free_fs()`, `ovl_destroy_inode()` |

## 16. 测试与验收

### 16.1 基础和回归

- 未调用 DeltaFS ioctl 时运行现有 OverlayFS selftests。
- lower-only mount 可正常读，DeltaFS ioctl 返回 `-EROFS`。
- 不支持 feature 的普通 OverlayFS 仍可使用，但 ioctl 返回 `-EOPNOTSUPP`。
- 32 位 compat 用户程序使用相同固定结构调用 ioctl。

### 16.2 ABI 和失败原子性

- bad size/version/flags/reserved；
- 非 root control fd；
- 无权限调用；
- 关闭、非目录和跨 mount namespace fd；
- upper/work/lower 重复或重叠；
- 不同 backing superblock；
- stale expected generation；
- checkpoint layer 顺序错误；
- restore 包含 current upper；
- 0、64、65 个 lower；
- 对每个 build allocation/clone/trap/workdir 步骤注入失败。

所有失败用例必须断言：

```text
global generation 未变
ofs->layers/numlayer 未变
root upper/root oe 未变
merged 内容未变
无新增 retired state
无 mount/dentry/inode 引用泄漏
```

### 16.3 功能与分支

覆盖：

- create、write、append、truncate；
- unlink、rmdir、whiteout；
- file/dir rename；
- chmod、chown、utime 和基础 xattr；
- checkpoint 后继续写，再 restore 任意历史点；
- A→B→C，restore A 后创建 A′；
- restore 同一 checkpoint 多次，分别形成独立分支；
- 1、8、32、64 层 lookup 和 restore。

每个用例同时验证 merged view 和物理 frozen layer。Frozen layer 的内容 hash、mtime、inode 和文件大小在后续写入后必须保持不变。

### 16.4 Cache

切换前预热并关闭所有 fd：

- 对存在文件反复 `stat/open/close`；
- 对不存在文件反复查询 ENOENT；
- 对目录执行 readdir 后关闭目录 fd；
- 对多层目录逐级 lookup。

切换后验证：

- 正 dentry generation mismatch 先退出 RCU walk，再由 ref-walk 失效；
- `ovl_lookup()` 使用当前 layer stack；
- negative dentry 不返回旧 ENOENT；
- restore 到相同 backing inode 时创建 current-generation overlay inode；
- root readdir cache 因 version 递增而重建。

### 16.5 生命周期

- 连续至少 100 次 checkpoint/restore；
- 长时间保留 stale dcache/inode cache 后继续切换；
- 反复 mount/unmount；
- module unload；
- KASAN、KFENCE、UBSAN、lockdep、RCU debug、kmemleak；
- 检查 private mount、trap、dentry 和 config string 的引用平衡。

### 16.6 不验收场景

以下测试即使偶然通过也不能声明受支持：

- checkpoint 前打开的普通/目录 fd 在切换后继续访问；
- cwd 位于 merged mount 内跨切换；
- writable mmap；
- concurrent syscall/copy-up；
- async I/O；
- controller 崩溃自动恢复；
- 在线删除 retired layer。

## 17. v1 完成判定

只有同时满足以下条件，才能称为 DeltaFS v1：

1. 两个 ioctl 在声明边界内完成 checkpoint 和任意历史 restore。
2. 成功切换 generation 恰好加一，失败不改变 active view。
3. 正/负缓存预热后仍能读取当前 view。
4. frozen layer 在后续写入中保持不变。
5. restore 后的分支写入只进入 fresh upper。
6. 64 层以内功能正确，超限明确失败。
7. 反复切换和 unmount 无 UAF、double free、deadlock 或引用泄漏。
8. 文档明确保留旧 fd、mmap、并发、GC 和崩溃恢复限制。

## 18. 最终产出

本阶段的最终产出仅为本设计文档：`docs/deltafs_v1_design.md`。

本文档包含 DeltaFS v1 的功能边界、UAPI、状态模型、数据结构、layer 构建、原子提交、generation 慢路径、retired 生命周期、controller 流程、errno 语义以及验收测试。本文档定稿不代表已经实现内核补丁、用户态 controller 或测试代码；这些内容应在后续实现任务中依据本文逐项完成。
