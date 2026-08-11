# DeltaFS v1 详细设计

> 状态：设计定稿，P1--P6 代码与 QEMU/KVM 运行时验收已完成；P7 验收
> harness 已实现，运行结论必须由 QEMU/KVM debug guest 产生
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

`ovl_dir_operations` 增加 `unlocked_ioctl`；现有的 generic compat hook 可以保留，
但 v1 原型不承诺或验收 32 位用户程序。Dispatcher 的顺序是：

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
  "active_lowers": [
    "layers/cp-000002",
    "layers/cp-000001",
    "base"
  ],
  "retired_branches": ["g1", "g2"],
  "snapshots": {
    "cp-000001": {
      "lowers": [
        "layers/cp-000001",
        "base"
      ]
    },
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
- `active_branch` 只表示当前临时 RW upper/work，且固定使用当前 runtime
  generation 对应的 `gN`；
- `active_lowers` 显式保存当前完整 lower chain，使 restore 后的下一次
  checkpoint 不依赖猜测或 snapshot ID 反推；
- 每次成功切换都把旧 active branch 加入 `retired_branches`，v1 在线期间不删除；
- snapshot 的第一层必须是自己的 `layers/<checkpoint-id>`，其余部分必须完整
  等于父 snapshot chain，最后一层必须为 `base`；
- 每个 snapshot 的 parent 必须在 metadata 中先出现，禁止循环或前向 parent 引用；
- generation 是 runtime CAS 值，不是 snapshot ID；
- controller 使用 `controller.lock` 保证单实例操作。

Controller 只接受无转义的 printable-ASCII checkpoint ID 和相对路径组件，拒绝
未知/重复 JSON 字段、重复 lower、断裂的 parent chain、generation/branch
不一致以及超过上限的 metadata。该限制让高权限 controller 不需要通用 JSON
扩展语义，也不会通过 manifest 路径越出 sandbox root。

P6 controller 的两个运行时命令为：

```text
deltafsctl --assume-quiesced checkpoint <sandbox-root> <checkpoint-id>
deltafsctl --assume-quiesced restore    <sandbox-root> <checkpoint-id>
```

Sandbox provisioning 负责创建初始 `g1` branch、`base`、`merged` mount 和上面的
generation-1 metadata；controller 不创建 mount，也不尝试探测或停止 workload。

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

lower-only mount 保持 `delta_backing_sb == NULL`。Linux 6.8 基线拒绝没有
upperdir 且只有一个 lowerdir 的配置，因此生命周期测试使用两个 lowerdir
建立 lower-only OverlayFS。

### 7.2 `struct ovl_inode` 扩展

新增：

```c
u64 delta_generation;
```

不能复用 `ovl_inode.version`，后者已经用于 merge-directory readdir cache。

普通 overlay inode 必须在加入 inode cache 前记录 generation：generation-aware `iget5` set callback 同时设置 `i_private` 和 `delta_generation`。对不经过 `iget5` 的 unhashed/root inode，`ovl_inode_init()` 再记录 acquire-load 的当前 generation。`ovl_inode_init()` 不得覆盖一个已经由 cache key 确定的 generation。Trap inode不代表可见 overlay dentry，不参与 generation revalidation。Trap 创建和 layer-root 冲突检测继续使用只比较 real inode 的 identity test/set；trap 查询使用额外检查 trap 状态的专用 test，避免同一 real inode 的多个普通 generation cache entry 遮蔽真正的 trap。

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

1. `fget_raw()` 取得 file；用 `fget_raw()` 而非 `fdget_raw()`，因为后者是
   `static inline`，会展开为未导出给模块的 `__fdget_raw()`，而 DeltaFS 构建在
   `CONFIG_OVERLAY_FS=m` 的可加载模块中；`fget_raw()` 经 `EXPORT_SYMBOL`
   导出，同样接受 `O_PATH` fd（不过滤 `FMODE_PATH`）；
2. 检查 file 设置了 `FMODE_PATH`，且
   `S_ISDIR(file_inode(file)->i_mode)`；
3. 复制 `file->f_path` 并 `path_get()`；
4. `fput()`；
5. 构建结束后统一 `path_put()`。

关闭用户 fd 不影响内核构建，因为 builder 已持有 path 引用。

### 8.3 通用验证

验证必须在 clone mount 前完成可提前完成的部分：

- `nr_lower` 在 `[1, 64]`；
- upper/work 不是只读 mount；
- upper/work 位于同一个 `vfsmount`，避免 idmap 和 mount identity 不一致；
- 所有输入 mount 都不是 idmapped mount；
- 所有 backing `mnt_sb == ofs->delta_backing_sb`；
- 任一路径不位于当前或其他 OverlayFS superblock；
- 路径没有重复；
- upper、work、各 lower 两两不为祖先/后代；
- upper/work 与 active 或 retired state 中仍持有的 layer root、work base
  和内部 workdir 均不相等、也不构成祖先/后代关系；
- lower 不等于此次 fresh upper/work；
- restore 的 lower 不等于当前 active upper；
- layer 数未超过 OverlayFS 和 DeltaFS 双重上限。

Fresh upper 和 work base 由 controller 通过唯一名称和 `mkdirat()` 创建。内核额外用 directory iteration 验证两者在构建开始时为空；work helper 创建内部目录后不再要求 work base 为空。

Active upper/work 必须已经持有 OverlayFS in-use lock；new upper/work 也必须
成功获取各自的 in-use lock，否则返回 `-EBUSY`。DeltaFS 不沿用 stock
OverlayFS 在 `index=off` 时对共享 upper/work 只警告后继续的兼容行为。

所有读取或修改 backing 对象的 VFS 操作使用 `ofs->creator_cred`。fd/path
获取和 `d_path()` 显示字符串仍基于调用 controller 的 fd 与 mount namespace。

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

对 active/retired state 的 root 比较和 trap 复用在 `ofs->delta_lock` 下进行；
builder 获得自己的 trap 引用后即可在锁外继续构建。另一个 commit 只会把旧
active state 移入 retired list，不会使 builder 持有的引用失效。

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

Builder 复用 `ovl_get_root()` 的 whiteout/xwhiteout 检查逻辑计算新 root 所需
flags，但不创建新的 overlay root inode。至少预先记录 new upper 的 impure
状态、各 lower 的 `has_xwhiteouts` 和 root xwhiteout 汇总状态；real-dentry
revalidation flags 可以在 commit 中从已经持有引用的 root binding 无失败地
重新汇总。

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

P4 只验证 builder：完整 target state 构建成功后立即调用与 retired state
共用的 free helper，且 ioctl 仍返回 `-EOPNOTSUPP`。P4 不安装 target state、
不增加 generation、也不向 retired list 加入节点。P5 才在最终重检成功后
调用无失败 commit。

“完整释放”指 mount、trap、dentry、in-use lock、root binding 和配置字符串
的内核所有权全部平衡。严格 work helper 已经在 fresh work base 中创建的内部
`work` 目录不由 free helper 删除；ioctl 失败后 controller 删除整个 fresh
branch。

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

1. 在仍持有 `delta_lock` 时，先获取 root inode `i_rwsem`，再获取 root
   `ovl_inode.lock`。在三把锁全部持有前不得移动任何 active 所有权。
2. 将当前 `ofs->layers/numlayer` 移入预分配 old state。
3. 将当前 workbasedir、workdir、whiteout、traps 和 in-use lock 状态移入
   old state。
4. 将当前 `config.upperdir/workdir/lowerdirs` 移入 old state。
5. 将 root inode 的旧 `__upperdentry` 和旧 `oe` 移入 old state。
6. 把 new state 的 layer/work/config 字段安装到 `ovl_fs`。
7. 把 new root upper 和 `ovl_entry` 安装到 root `ovl_inode`。
8. 依据新 root backing 更新 root inode attributes 和 state-derived flags。
9. 递增 root inode 的 readdir `version`，使 root directory cache 失效。
10. 设置 root inode `delta_generation = new_generation`。
11. 先释放 root `ovl_inode.lock`，再释放 root inode `i_rwsem`。
12. 将 old state 加入 `delta_retired`。
13. 用 `smp_store_release()` 最后发布 `ofs->delta_generation`。

步骤 2--7 只记录旧 owner 并用新 owner 覆盖对应字段，不得先把 active 字段
清成 `NULL`。每个从 new state 移出的指针和 lock ownership 都必须在 source 中
清零；提交完成后 new state 容器为空，可以直接释放容器，不能再调用会释放已
转移资源的完整 state teardown。

全局 generation 最后发布，保证后续 acquire-load 观察到新 generation 时，也能观察到完整的新 layer 和 root binding。

该 release/acquire 关系只提供“观察到新 generation 必然观察到完整新 view”的
单向发布保证，不是并发 reader 的 seqlock。一个在提交前已经读到旧 generation
的并发路径操作不在此协议保护范围内；v1 仍要求提交期间 workload 完全静止。

root readdir `version` 和目录 cache 由 inode `i_rwsem` 保护；仅持有 `ovl_inode.lock` 不足以与目录迭代同步。锁顺序固定为 `delta_lock -> root inode i_rwsem -> root ovl_inode.lock`。

### 10.3 Root flags

Root 不重新创建 inode。更新 helper 应镜像 `ovl_get_root()` 中与 backing view 相关的初始化：

- root 始终设置 connected 和 whiteout 能力；
- 有新 upper 时设置 upper alias/upper data；
- 根据新 upper 更新 impure 状态；
- 根据 lower roots 更新 xwhiteout 标志；
- 调用 `ovl_copyattr()` 从新 real inode更新可见 attributes；
- 调用 `ovl_copyflags()` 更新允许从 real inode 复制的 VFS inode flags；
- 重新初始化 real-dentry revalidation flags。

强制的 `DCACHE_OP_REVALIDATE` 必须保留，只按新 root backing 重算 weak
revalidation flags。Root VFS inode 对象及其 `i_ino` 保持稳定；same-fs v1 配置
下的用户可见 `stat(2)` inode number 仍由新的 real root 提供。不得清除与动态
view 无关的通用 inode 状态。

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

`ovl_alloc_inode()` 先把 `delta_generation` 初始化为 0；cache set callback 或 unhashed/root inode 的 `ovl_inode_init()` 再赋予有效值。这样同一 backing inode 在多个 generation 中可以对应多个 overlay inode，也不会出现 inode 已进入 hash、generation 仍为 0 的窗口。

Trap inode 保持 generation 为 0。Trap 创建仍使用原有 identity-only test/set，使任何同 real inode 的普通 inode 都能触发 layer-root 冲突；`ovl_lookup_trap_inode()` 则使用 trap-only test，使 hash bucket 中先出现的普通旧代 inode不会造成假阴性。普通 `ovl_lookup_inode()` 在进行 generation-aware lookup 前必须单独检查并拒绝 trap，以保留 export/decode 路径对 layer root 返回 `-ESTALE` 的原生语义。

### 11.4 负 dentry

Negative dentry 没有 inode，无法读取 inode generation。v1 不为 `d_fsdata` 引入新的分配对象，而是在 `ovl_lookup()` 得到 negative 结果、由 `d_splice_alias(NULL, dentry)` 完成 lookup 后调用 `d_drop()`。不能在 `d_splice_alias()` 前 drop，否则该 helper 会重新将 negative dentry 加入 hash。

结果是：

- ENOENT 不跨 syscall 长期缓存；
- checkpoint/restore 后不会返回旧 generation 的 negative lookup；
- 代价是所有 OverlayFS mount 上负路径查找性能下降，作为 v1 已知取舍记录。

### 11.5 Root inode 例外

`sb->s_root` 不能像普通 dentry 一样丢弃重建，因此 root inode 在 commit 中原地更新并直接设置新 generation。Control fd 虽然跨越 commit，但只执行当前 ioctl，返回后立即关闭。

root readdir cache 复用 `ovl_inode.version` 协议。P3 只抽取要求持有 inode `i_rwsem` 的 version increment helper，不发生额外递增；真正的 view commit 调用该 helper使旧 root cache 失效。

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
6. 计算完整 target chain、`gN+1` 和提交后的 `state.json`，完成所有用户态
   内存预分配；
7. 用临时文件、file `fsync()`、`rename()` 和 meta directory `fsync()` 先持久化
   `transaction.json`；
8. transaction durable 后才允许创建 fresh branch 或执行其他 backing-tree
   修改；
9. 以 `O_PATH|O_DIRECTORY` 打开 backing directories；
10. 填充全部未使用 fd 槽为 `-1`。

Controller 以 C 实现 ioctl、rename、fd 和 fsync 的核心路径；测试编排可以使用 shell 或 Python。

### 13.2 Checkpoint

假设当前 active branch 是 `gN`：

1. 验证 checkpoint ID 不存在。
2. 组成 target lower chain：`[new frozen layer] + active_lowers`。
3. 写入 `transaction.json`，状态为 `prepared`。
4. 创建并 fsync `branches/gN+1/upper` 和 `branches/gN+1/work`。
5. 将 `branches/gN/upper` rename 为 `layers/<checkpoint-id>`。
6. fsync `branches/gN` 和 `layers`。
7. 调用 `DELTAFS_IOC_CHECKPOINT(expected_generation=N)`。
8. 成功后将新 snapshot、`active_lowers`、active/retired branch 和
   `kernel_generation=N+1` 写入临时 state 文件。
9. fsync 临时文件，rename 覆盖 `state.json`，再 fsync `meta`。
10. 删除 `transaction.json` 并 fsync `meta`，释放 controller lock。

ioctl 失败时：

- 当前内核 view 保持不变；
- 将 frozen layer rename 回 `branches/gN/upper`；
- 删除 fresh branch；
- fsync 补偿后的目录；全部补偿成功后才删除并 fsync transaction 文件；
- 返回原始 errno。

### 13.3 Restore

1. 查找目标 checkpoint 的 immutable lower chain。
2. 写入 `transaction.json`，状态为 `prepared`。
3. 创建并 fsync 全新的 `branches/gN+1/upper` 和 `work`。
4. 打开目标完整 lower chain 的 directory fds。
5. 调用 `DELTAFS_IOC_RESTORE(expected_generation=N)`。
6. 成功后更新 active branch、`active_lowers` 和 `kernel_generation=N+1`。
7. 原 active branch 标记为 retired，但不删除。
8. 原子替换 `state.json`，删除 transaction 文件。

ioctl 失败时删除 fresh branch并 fsync `branches`；补偿成功后删除 transaction，
当前 active branch 与 manifest 不变。

### 13.4 Controller 崩溃边界

v1 不提供 `GET_STATE` ioctl，也不承诺掉电事务恢复。`transaction.json`、
`.transaction.json.tmp` 或 `.state.json.tmp` 任一存在时，后续命令都必须
fail-stop。尤其是 ioctl 已返回成功、但 controller 在提交 `state.json` 前失败时：

- `transaction.json` 必须保留；
- 后续命令必须拒绝继续操作；
- 管理员需要卸载并依据 manifest/transaction 选择明确的 layer chain重新挂载；
- 工具不得猜测 ioctl 是否提交，也不得自动删除任何相关目录。

这保证故障可见，但不属于自动恢复。

ioctl 返回成功是用户态事务的不可回滚点。其后的 state write、file/directory
fsync 或 transaction unlink 失败时，controller 不得把 layer rename 回去、不得
删除 fresh branch，只能保留可见故障并要求管理员明确恢复。

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
| `-ENOTEMPTY` | fresh upper 或 fresh work base 在构建开始时非空 | 否 |
| `-EBUSY` | active 或 new upper/work 未能持有独占 in-use lock | 否 |
| `-EROFS` | OverlayFS 无 writable upper/work 或 superblock 只读 | 否 |
| `-EOPNOTSUPP` | feature、idmap、嵌套布局或其他 v1 不支持条件 | 否 |
| `-EXDEV` | backing path 不在 `delta_backing_sb` | 否 |
| `-E2BIG` | lower 超过 64 | 否 |
| `-ESTALE` | expected generation 不匹配 | 否 |
| `-EOVERFLOW` | generation 已为 `U64_MAX` | 否 |
| `-ENOMEM` | build/preallocation 失败 | 否 |

Backing VFS 操作产生的 `-EACCES`、`-ENOSPC`、`-ENAMETOOLONG` 等错误可以
原样返回。可归因于重复、重叠或已有普通 overlay inode 的 trap 冲突统一映射
为 `-EINVAL`，不把内部 `-ELOOP` 作为 DeltaFS 路径验证 ABI 暴露。

一旦 commit 开始，内部函数没有失败返回。ioctl 返回 0 时 active view 已完整切换。

## 15. 代码组织建议

主要实现边界：

- `include/uapi/linux/deltafs.h`：公开 ABI；
- `fs/overlayfs/deltafs.c`：ioctl、validation、state builder、commit、retired free；
- `fs/overlayfs/ovl_entry.h`：generation 与 DeltaFS state 字段；
- OverlayFS 的 super/inode/namei/readdir 集成点：初始化、root 更新、generation-aware inode lookup、dentry revalidation 和 directory ioctl hook；
- 用户态工具目录：controller、manifest 和 CLI；
- OverlayFS selftests：ABI 负向测试、cache、checkpoint/restore 和 teardown。

P6 的具体用户态产物：

- `tools/deltafs/deltafsctl.c`：仅包含 `checkpoint` 和 `restore` 两种运行时操作；
- `tools/deltafs/deltafsctl_test`：只供 harness 使用的 controller failpoint build；
- `tools/deltafs/p6_controller_unit_test.sh`：不挂载文件系统的 parser/补偿测试；
- `tools/deltafs/p6_controller_test.sh`：QEMU/KVM 双操作、分支和崩溃边界验收；
- `tools/deltafs/p6_generation_test.c`：利用 errno 顺序确认精确 runtime generation。

宿主机可运行 `make -C tools/deltafs test-p6-controller`；它使用非 OverlayFS
目录让 ioctl 返回 `ENOTTY`，并验证同步失败的 rename/branch/transaction
补偿、未知 restore、遗留 transaction、缺失 frozen target 和循环 manifest
均在任何内核切换前 fail closed。完整功能验收只能执行
`p6_controller_test.sh --backing-root PATH`，该脚本明确限制在项目 QEMU/KVM guest。

P7 的验收产物：

- `tools/deltafs/p7_acceptance_test.sh`：唯一的 guest-only v1 验收入口，串联
  具备最终提交语义的 P5/P6 运行时证据和 P7 深层压力矩阵；
- `tools/deltafs/p7_ioctl_test.c`：native ABI 负向矩阵，逐例确认 failure atomicity；
- `tools/deltafs/p7_checkpoint_callsite_test.sh`：构建后静态检查，确认编译器至少保留
  源码中的全部 `ovl_deltafs_build_checkpoint()` 静态调用点（当前 18 个，其中循环
  调用点会按 layer 数动态执行），防止恒零 helper 被 IPA 优化后产生故障覆盖
  假阳性；
- `make -C tools/deltafs p7-tools`：构建 P7 所需 native helper；
- `make -C tools/deltafs test-p7 BACKING_ROOT=... EXTRA_BACKING_ROOT=...`：仅供
  已启动的项目 QEMU/KVM debug guest 执行。

固定请求结构没有用户指针，因此本原型不需要维护 compat 指针转换；32 位用户程序
不属于 v1 的支持或 P7 验收范围，也不以缺少 multilib/toolchain 作为失败条件。

P1--P4 的合法 build 测试属于阶段性证据：在 P5 引入原子 commit 前，它们刻意要求
完整构建后以 `-EOPNOTSUPP` 结束且 generation 不变。最终内核对相同合法请求必须
成功提交，因此 P7 不直接重跑这些互斥的旧终态断言；其 ABI/路径负向覆盖和 builder
unwind 分别由 P7 native matrix 与 64 层动态故障注入重新验证。

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
- 至少包含两个 lowerdir 的 lower-only mount 可正常读，DeltaFS ioctl 返回
  `-EROFS`。
- 不支持 feature 的普通 OverlayFS 仍可使用，但 ioctl 返回 `-EOPNOTSUPP`。
- 固定结构不含用户指针；32 位用户程序不属于本原型的支持或验收范围。

### 16.2 ABI 和失败原子性

- bad size/version/flags/reserved；
- 非 root control fd；
- 无权限调用；
- 关闭、非 `O_PATH`、非目录和跨 mount namespace fd；
- upper/work/lower 重复或重叠；
- 不同 backing superblock；
- stale expected generation；
- checkpoint layer 顺序错误；
- restore 包含 current upper；
- 0、64、65 个 lower；
- 对每个 build allocation/clone/trap/workdir 步骤注入失败。

P4 中，builder 成功后的阶段性终值仍为 `-EOPNOTSUPP`；测试必须同时确认
state 已完整释放且 active generation 仍接受原 `expected_generation`。故障注入
使用仅在内核 fault-injection 配置下生效的内部 build checkpoint，不扩展 UAPI。

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

P5 的 checkpoint/multi-commit 验收在同一 mount 上固定执行以下序列：

```text
generation 1 --checkpoint--> generation 2
generation 2 ----restore---> generation 3
generation 3 --checkpoint--> generation 4
```

第一次 checkpoint 验证 active upper 成为第一层 frozen lower；restore 验证
generation 2 的分支写入被丢弃且历史 checkpoint 保持不变；第二次 checkpoint
验证 layer depth 再次增长时的 active trap 复用。最终必须同时核验当前 upper、
两个 frozen checkpoint、retired generation-2 upper 和原始 lower 的物理内容与
mtime/size/inode，并用 errno 顺序证明 global generation 恰好为 4。

P6 在 controller 路径上固定执行：

```text
generation 1 --checkpoint A--> generation 2
generation 2 --checkpoint B--> generation 3
generation 3 --checkpoint C--> generation 4
generation 4 ----restore A----> generation 5 / fresh A′
```

测试在每个 layer 冻结后记录内容 hash、inode、size 与 mtime；restore 后向
`g5/upper` 写入 A′，并再次确认 A、B、C 以及 base 均未变化，merged view 恢复
A 且不包含 B/C 的分支内容。另有两个 controller 边界：

- ioctl 前注入失败必须把 layer rename、fresh branch、state 与 transaction
  全部补偿回 generation 1；
- ioctl 成功后立即终止 controller 必须保留 transaction，维持旧 state.json，
  且拒绝任何后续 checkpoint/restore，直到管理员人工恢复。

### 16.4 Cache

切换前预热并关闭所有普通 fd：

- 对存在文件反复 `stat/open/close`；
- 对不存在文件反复查询 ENOENT；
- 对目录执行 readdir 后关闭目录 fd，验证契约内的重新打开结果；
- 对多层目录逐级 lookup。

关闭最后一个 OverlayFS 目录 fd 会释放该 fd 持有的 readdir cache，因此上述
契约内用例只能验证切换后重新打开目录得到新 view，不能单独证明 root
`version` 失效路径。另设一个仅用于内核机制验收的测试：在唯一 control fd 上
预热 root readdir cache，`lseek(fd, 0, SEEK_SET)` 后执行 ioctl，再在同一 fd 上
readdir，确认 version mismatch 丢弃旧 cache 并重建。该白盒用例是对“不跨切换
使用目录 fd”前置条件的明确测试例外，不扩展 v1 用户可见语义。

P5 的第一次可观察切换优先使用 restore：初始 view 与目标 lower chain 使用不同
内容，使正路径变化、正路径消失、负路径变为存在以及 root readdir 集合变化都
可直接断言。第一次 checkpoint 在 fresh upper 为空时应保持逻辑内容不变，不适合
单独证明 cache 已经换代。

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

`p5_checkpoint_test.sh` 在三次提交后卸载同一 OverlayFS、卸载 overlay module，
等待 kmemleak minimum age 后连续扫描两次；报告必须为空，marker 窗口内也不得
出现 sanitizer、refcount、lockdep 或 RCU 诊断。

### 16.6 P7 v1 总验收入口

P7 不增加 UAPI 或新的运行时语义，而是把第 17 节的完成判定转成一个可留档的
QEMU/KVM guest 运行。入口为：

```text
make -C tools/deltafs p7-tools
tools/deltafs/p7_acceptance_test.sh \
    --backing-root PATH --extra-backing-root OTHER_PATH
```

它拒绝非 QEMU/KVM 环境、已有 OverlayFS mount、非模块化 overlay，以及缺少
`CONFIG_FUNCTION_ERROR_INJECTION`、KASAN、KFENCE、UBSAN、lockdep/PROVE_RCU、
kmemleak 的内核。它会保存内核 config、每个阶段日志、
dmesg marker window、kmemleak 双扫描结果和 `section-17.tsv`；任何前置条件不满足
都是失败，不是 skip。

P7 先重跑具备最终提交语义的 P5 cache/multi-commit 和 P6 controller 验证；P4
阶段的 build/free 故障覆盖由下述最终语义下的深层动态故障注入取代。随后：

- 在同一 controller sandbox 完成 63 次 checkpoint，得到 `base + 63 snapshot`
  的 64-lower chain；随后 restore 该 chain，并确认 controller 的第 65 次
  checkpoint 在任何树变更前拒绝。native ABI helper 另行发送 `nr_lower=65`，
  确认内核返回 `-E2BIG`；
- 每个 frozen layer 的两个物理文件均记录 hash、inode、size、mode 与 mtime，
  每次后续 switch/失败注入后重新校验；
- 在 64-lower restore 上用 `ovl_deltafs_build_checkpoint()` 的第 N 次动态失败
  注入驱动所有实际 ownership checkpoint。`fail_function` 的全局 `count` 无法通过
  debugfs 重置，因此每轮固定 `interval=1` 并重置 `space=N`；该注入点的 size 为
  1，故能精确命中本轮第 N 次调用。连续 `-ENOMEM` 必须保留 generation、state、
  transaction、fresh branch 和所有 frozen fingerprint；第一个未命中 N 必须成功
  提交，因此计数不会依赖易失的硬编码值。注入 helper 本体必须包含 compiler
  barrier，阻止 GCC 在 `-O2` 下跨过程证明其恒为 0 并删除调用；构建后的
  `deltafs.o` 还必须通过“调用 relocation 数等于源码调用点数”的静态检查；运行时
  实际注入次数必须至少为 64，以证明按 layer 执行的循环调用点也已覆盖；
- 以该成功 restore 作为第 64 次成功切换，再做 36 次历史 restore，使总数恰为
  100，最终 generation 为 101；每次 restore 后的写入必须进入其 `gN/upper`；
- 在独立 sandbox 中至少十次 checkpoint → umount → `modprobe -r overlay`，并在
  最后 module unload 后执行 kmemleak 双扫描和整个 P7 marker window 的 sanitizer
  检查。

P7 脚本实现完成不等于 P7 已通过。只有用户在符合上述配置的 guest 中实际执行并
保存结果目录后，才可以将第 17 节的八项改标为已满足。

### 16.7 不验收场景

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

DeltaFS v1 的实现产物包括 OverlayFS 内核补丁、UAPI、controller、P1--P7 测试
helper 和本设计文档。P7 结果目录是完成判定的运行时证据；在它由目标 QEMU/KVM
debug guest 生成前，本文档不能宣称 v1 已完成。
