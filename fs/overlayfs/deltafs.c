// SPDX-License-Identifier: GPL-2.0-only
/*
 * DeltaFS v1 ioctl front-end, target state builder, and view commit.
 *
 * A target view is fully built outside the active ovl_fs.  Commit then moves
 * every dynamic owner under a fixed lock order, retires the old view, and
 * publishes the new generation only after the new root binding is complete.
 */

#include <uapi/linux/deltafs.h>
#include <linux/capability.h>
#include <linux/compiler.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/error-injection.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xattr.h>

#include "overlayfs.h"

struct ovl_delta_paths {
	struct path upper;
	struct path work;
	unsigned int nr_lower;
	struct path lower[];
};

struct ovl_delta_empty_ctx {
	struct dir_context ctx;
	bool empty;
};

/*
 * Debug kernels keep one injectable checkpoint after every successful
 * ownership acquisition.  fail_function can select the Nth call by resetting
 * its space control, without adding test flags to the DeltaFS UAPI.  The
 * compiler barrier is an intentional observable compiler side effect:
 * noinline alone still lets GCC prove that this function always returns zero
 * and delete most callers during IPA.
 *
 * Production kernels compile the inline stub and all checkpoint branches
 * away when function error injection is disabled.
 */
#ifdef CONFIG_FUNCTION_ERROR_INJECTION
noinline int ovl_deltafs_build_checkpoint(void);
noinline int ovl_deltafs_build_checkpoint(void)
{
	barrier();
	return 0;
}
ALLOW_ERROR_INJECTION(ovl_deltafs_build_checkpoint, ERRNO);
#else
static __always_inline int ovl_deltafs_build_checkpoint(void)
{
	return 0;
}
#endif

static int ovl_deltafs_validate_abi(const struct deltafs_ioc_switch_v1 *req)
{
	unsigned int i;

	if (req->size != sizeof(*req) ||
	    req->version != DELTAFS_ABI_VERSION || req->flags ||
	    req->reserved0 || memchr_inv(req->reserved, 0, sizeof(req->reserved)))
		return -EINVAL;
	if (!req->nr_lower)
		return -EINVAL;
	if (req->nr_lower > DELTAFS_V1_MAX_LOWERS)
		return -E2BIG;
	for (i = req->nr_lower; i < DELTAFS_V1_MAX_LOWERS; i++) {
		if (req->lower_fds[i] != -1)
			return -EINVAL;
	}

	return 0;
}

static int ovl_deltafs_validate_generation(struct ovl_fs *ofs,
					   u64 expected_generation)
{
	u64 generation = smp_load_acquire(&ofs->delta_generation);

	if (expected_generation != generation)
		return -ESTALE;
	if (generation == U64_MAX)
		return -EOVERFLOW;

	return 0;
}

static int ovl_deltafs_get_path(int fd, struct path *path)
{
	struct file *file = fget_raw(fd);
	int err = 0;

	if (!file)
		return -EBADF;
	if (!S_ISDIR(file_inode(file)->i_mode)) {
		err = -ENOTDIR;
	} else if (!(file->f_mode & FMODE_PATH)) {
		err = -EINVAL;
	} else {
		*path = file->f_path;
		path_get(path);
	}
	fput(file);

	return err;
}

static void ovl_deltafs_put_paths(struct ovl_delta_paths *paths)
{
	unsigned int i;

	if (!paths)
		return;
	if (paths->upper.dentry)
		path_put(&paths->upper);
	if (paths->work.dentry)
		path_put(&paths->work);
	for (i = 0; i < paths->nr_lower; i++) {
		if (paths->lower[i].dentry)
			path_put(&paths->lower[i]);
	}
	kfree(paths);
}

static struct ovl_delta_paths *
ovl_deltafs_get_paths(const struct deltafs_ioc_switch_v1 *req)
{
	struct ovl_delta_paths *paths;
	unsigned int i;
	int err;

	paths = kzalloc(struct_size(paths, lower, req->nr_lower), GFP_KERNEL);
	if (!paths)
		return ERR_PTR(-ENOMEM);
	paths->nr_lower = req->nr_lower;

	err = ovl_deltafs_get_path(req->upper_fd, &paths->upper);
	if (err)
		goto out_err;
	err = ovl_deltafs_get_path(req->work_fd, &paths->work);
	if (err)
		goto out_err;
	for (i = 0; i < req->nr_lower; i++) {
		err = ovl_deltafs_get_path(req->lower_fds[i], &paths->lower[i]);
		if (err)
			goto out_err;
	}

	return paths;

out_err:
	ovl_deltafs_put_paths(paths);
	return ERR_PTR(err);
}

static const struct path *ovl_deltafs_path(const struct ovl_delta_paths *paths,
					    unsigned int index)
{
	if (!index)
		return &paths->upper;
	if (index == 1)
		return &paths->work;
	return &paths->lower[index - 2];
}

static bool ovl_deltafs_same_root(const struct path *a, const struct path *b)
{
	return a->mnt->mnt_sb == b->mnt->mnt_sb &&
	       d_inode(a->dentry) == d_inode(b->dentry);
}

static bool ovl_deltafs_overlaps(const struct path *a, const struct path *b)
{
	if (a->mnt->mnt_sb != b->mnt->mnt_sb)
		return false;

	return is_subdir(a->dentry, b->dentry) ||
	       is_subdir(b->dentry, a->dentry);
}

static bool ovl_deltafs_overlaps_dentry(const struct path *path,
					 struct dentry *dentry)
{
	if (!dentry || path->mnt->mnt_sb != dentry->d_sb)
		return false;

	return is_subdir(path->dentry, dentry) ||
	       is_subdir(dentry, path->dentry);
}

static bool ovl_deltafs_empty_actor(struct dir_context *ctx, const char *name,
				    int namelen, loff_t offset, u64 ino,
				    unsigned int d_type)
{
	struct ovl_delta_empty_ctx *empty =
		container_of(ctx, struct ovl_delta_empty_ctx, ctx);

	if (namelen == 1 && name[0] == '.')
		return true;
	if (namelen == 2 && name[0] == '.' && name[1] == '.')
		return true;

	empty->empty = false;
	return false;
}

static int ovl_deltafs_check_empty(const struct path *path)
{
	struct ovl_delta_empty_ctx empty = {
		.ctx.actor = ovl_deltafs_empty_actor,
		.empty = true,
	};
	struct file *file;
	int err;

	file = ovl_path_open(path, O_RDONLY | O_LARGEFILE);
	if (IS_ERR(file))
		return PTR_ERR(file);
	err = iterate_dir(file, &empty.ctx);
	fput(file);
	if (err)
		return err;

	return empty.empty ? 0 : -ENOTEMPTY;
}

static int ovl_deltafs_validate_features(struct super_block *sb,
					 struct ovl_fs *ofs)
{
	if (!ofs->layers || !ofs->numlayer || !ovl_upper_mnt(ofs) ||
	    !ofs->config.upperdir || !ofs->config.workdir ||
	    !ofs->workbasedir || !ofs->workdir || !ofs->delta_backing_sb ||
	    sb_rdonly(sb))
		return -EROFS;

	if (!ofs->upperdir_locked || !ofs->workdir_locked)
		return -EBUSY;

	if (ofs->config.index || ofs->config.nfs_export ||
	    ofs->config.metacopy || ofs->config.xino != OVL_XINO_OFF ||
	    ofs->config.uuid != OVL_UUID_OFF ||
	    ofs->config.redirect_mode != OVL_REDIRECT_NOFOLLOW ||
	    ofs->config.ovl_volatile || ofs->numdatalayer ||
	    ofs->numfs != 1 || ofs->xino_mode != 0)
		return -EOPNOTSUPP;

	return 0;
}

static int ovl_deltafs_validate_paths(struct ovl_fs *ofs,
				      const struct ovl_delta_paths *paths)
{
	unsigned int nr_paths = paths->nr_lower + 2;
	unsigned int i, j;

	if (paths->upper.mnt != paths->work.mnt)
		return -EINVAL;
	if (__mnt_is_readonly(paths->upper.mnt) ||
	    __mnt_is_readonly(paths->work.mnt))
		return -EROFS;

	for (i = 0; i < nr_paths; i++) {
		const struct path *path = ovl_deltafs_path(paths, i);

		if (d_unlinked(path->dentry))
			return -EINVAL;
		if (path->mnt->mnt_sb->s_type == &ovl_fs_type)
			return -EOPNOTSUPP;
		if (is_idmapped_mnt(path->mnt))
			return -EOPNOTSUPP;
		if (path->mnt->mnt_sb != ofs->delta_backing_sb)
			return -EXDEV;
	}

	for (i = 0; i < nr_paths; i++) {
		for (j = i + 1; j < nr_paths; j++) {
			const struct path *a = ovl_deltafs_path(paths, i);
			const struct path *b = ovl_deltafs_path(paths, j);

			if (ovl_deltafs_same_root(a, b) ||
			    ovl_deltafs_overlaps(a, b))
				return -EINVAL;
		}
	}

	return 0;
}

static int ovl_deltafs_validate_empty(struct super_block *sb,
				      const struct ovl_delta_paths *paths)
{
	const struct cred *old_cred;
	int err;

	old_cred = ovl_override_creds(sb);
	err = ovl_deltafs_check_empty(&paths->upper);
	if (!err)
		err = ovl_deltafs_check_empty(&paths->work);
	revert_creds(old_cred);

	return err;
}

static char *ovl_deltafs_path_name(const struct path *path)
{
	char *buf, *name, *result;

	buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	name = d_path(path, buf, PATH_MAX);
	if (IS_ERR(name)) {
		result = name;
	} else {
		result = kstrdup(name, GFP_KERNEL);
		if (!result)
			result = ERR_PTR(-ENOMEM);
	}
	kfree(buf);

	return result;
}

static void ovl_deltafs_free_state(struct ovl_delta_state *state)
{
	struct vfsmount **mounts;
	unsigned int i, nr_mounts = 0;

	if (!state)
		return;

	dput(state->root_upperdentry);
	ovl_free_entry(state->root_oe);
	dput(state->whiteout);
	dput(state->workdir);
	iput(state->workdir_trap);
	iput(state->workbasedir_trap);

	if (state->workdir_locked)
		ovl_inuse_unlock(state->workbasedir);
	dput(state->workbasedir);

	if (state->upperdir_locked && state->layers && state->layers[0].mnt)
		ovl_inuse_unlock(state->layers[0].mnt->mnt_root);

	mounts = (struct vfsmount **)state->lowerdir_names;
	for (i = 0; i < state->numlayer; i++) {
		if (state->layers)
			iput(state->layers[i].trap);
		if (state->lowerdir_names)
			kfree(state->lowerdir_names[i]);
		if (state->layers && state->layers[i].mnt) {
			if (mounts)
				mounts[nr_mounts++] = state->layers[i].mnt;
			else
				kern_unmount(state->layers[i].mnt);
		}
	}
	if (nr_mounts)
		kern_unmount_array(mounts, nr_mounts);

	kfree(state->lowerdir_names);
	kfree(state->upperdir_name);
	kfree(state->workdir_name);
	kfree(state->layers);
	kfree(state);
}

static struct ovl_delta_state *
ovl_deltafs_alloc_state(const struct deltafs_ioc_switch_v1 *req,
			const struct ovl_delta_paths *paths)
{
	struct ovl_delta_state *state;
	char *name;
	unsigned int i;
	int err;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&state->node);
	state->generation = req->expected_generation + 1;
	state->numlayer = req->nr_lower + 1;

	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	state->layers = kcalloc(state->numlayer, sizeof(*state->layers),
				GFP_KERNEL);
	if (!state->layers) {
		err = -ENOMEM;
		goto out_err;
	}
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	state->lowerdir_names = kcalloc(state->numlayer,
					 sizeof(*state->lowerdir_names), GFP_KERNEL);
	if (!state->lowerdir_names) {
		err = -ENOMEM;
		goto out_err;
	}
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	state->root_oe = ovl_alloc_entry(req->nr_lower);
	if (!state->root_oe) {
		err = -ENOMEM;
		goto out_err;
	}
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	name = ovl_deltafs_path_name(&paths->upper);
	if (IS_ERR(name)) {
		err = PTR_ERR(name);
		goto out_err;
	}
	state->upperdir_name = name;
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	name = ovl_deltafs_path_name(&paths->work);
	if (IS_ERR(name)) {
		err = PTR_ERR(name);
		goto out_err;
	}
	state->workdir_name = name;
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_err;

	for (i = 0; i < req->nr_lower; i++) {
		name = ovl_deltafs_path_name(&paths->lower[i]);
		if (IS_ERR(name)) {
			err = PTR_ERR(name);
			goto out_err;
		}
		state->lowerdir_names[i + 1] = name;
		err = ovl_deltafs_build_checkpoint();
		if (err)
			goto out_err;
	}

	return state;

out_err:
	ovl_deltafs_free_state(state);
	return ERR_PTR(err);
}

static bool ovl_deltafs_path_matches_layer(const struct path *path,
					    const struct ovl_layer *layer)
{
	return layer->mnt && path->mnt->mnt_sb == layer->mnt->mnt_sb &&
	       d_inode(path->dentry) == d_inode(layer->mnt->mnt_root);
}

static int ovl_deltafs_validate_command_locked(
		struct ovl_fs *ofs, unsigned int cmd,
		const struct deltafs_ioc_switch_v1 *req,
		const struct ovl_delta_paths *paths)
{
	unsigned int i;

	if (cmd == DELTAFS_IOC_CHECKPOINT) {
		if (req->nr_lower != ofs->numlayer)
			return -EINVAL;
		for (i = 0; i < req->nr_lower; i++) {
			if (!ovl_deltafs_path_matches_layer(&paths->lower[i],
							    &ofs->layers[i]))
				return -EINVAL;
		}
	} else {
		for (i = 0; i < req->nr_lower; i++) {
			if (ovl_deltafs_path_matches_layer(&paths->lower[i],
							   &ofs->layers[0]))
				return -EINVAL;
		}
	}

	return 0;
}

static int ovl_deltafs_validate_active_path_locked(
		struct ovl_fs *ofs, const struct path *path)
{
	struct ovl_delta_state *retired;
	unsigned int i;

	for (i = 0; i < ofs->numlayer; i++) {
		if (ofs->layers[i].mnt &&
		    ovl_deltafs_overlaps_dentry(path,
						   ofs->layers[i].mnt->mnt_root))
			return -EINVAL;
	}
	if (ovl_deltafs_overlaps_dentry(path, ofs->workbasedir) ||
	    ovl_deltafs_overlaps_dentry(path, ofs->workdir))
		return -EINVAL;

	list_for_each_entry(retired, &ofs->delta_retired, node) {
		for (i = 0; i < retired->numlayer; i++) {
			if (retired->layers[i].mnt &&
			    ovl_deltafs_overlaps_dentry(
				    path, retired->layers[i].mnt->mnt_root))
				return -EINVAL;
		}
		if (ovl_deltafs_overlaps_dentry(path, retired->workbasedir) ||
		    ovl_deltafs_overlaps_dentry(path, retired->workdir))
			return -EINVAL;
	}

	return 0;
}

static struct inode *ovl_deltafs_find_trap_locked(
		struct ovl_fs *ofs, const struct path *path)
{
	struct ovl_delta_state *retired;
	struct inode *trap;
	unsigned int i;

	for (i = 0; i < ofs->numlayer; i++) {
		if (!ovl_deltafs_path_matches_layer(path, &ofs->layers[i]))
			continue;
		trap = igrab(ofs->layers[i].trap);
		return trap ?: ERR_PTR(-ESTALE);
	}

	list_for_each_entry(retired, &ofs->delta_retired, node) {
		for (i = 0; i < retired->numlayer; i++) {
			if (!ovl_deltafs_path_matches_layer(path,
							    &retired->layers[i]))
				continue;
			trap = igrab(retired->layers[i].trap);
			return trap ?: ERR_PTR(-ESTALE);
		}
	}

	return NULL;
}

static int ovl_deltafs_get_layer_traps_locked(
		struct super_block *sb, struct ovl_fs *ofs,
		const struct ovl_delta_paths *paths,
		struct ovl_delta_state *state)
{
	const struct path *path;
	struct inode *trap;
	unsigned int i;
	int err;

	for (i = 0; i < state->numlayer; i++) {
		path = !i ? &paths->upper : &paths->lower[i - 1];
		trap = ovl_deltafs_find_trap_locked(ofs, path);
		if (!trap)
			trap = ovl_get_trap_inode(sb, path->dentry);
		if (IS_ERR(trap)) {
			err = PTR_ERR(trap);
			return err == -ELOOP ? -EINVAL : err;
		}
		state->layers[i].trap = trap;
		err = ovl_deltafs_build_checkpoint();
		if (err)
			return err;
	}

	return 0;
}

static int ovl_deltafs_prepare_locked(
		struct file *file, unsigned int cmd,
		const struct deltafs_ioc_switch_v1 *req,
		const struct ovl_delta_paths *paths,
		struct ovl_delta_state *state)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct ovl_fs *ofs = OVL_FS(sb);
	int err;

	err = ovl_deltafs_validate_generation(ofs, req->expected_generation);
	if (err)
		return err;
	if (file_dentry(file) != sb->s_root)
		return -ENOTTY;
	err = ovl_deltafs_validate_features(sb, ofs);
	if (err)
		return err;
	err = ovl_deltafs_validate_active_path_locked(ofs, &paths->upper);
	if (err)
		return err;
	err = ovl_deltafs_validate_active_path_locked(ofs, &paths->work);
	if (err)
		return err;
	err = ovl_deltafs_validate_command_locked(ofs, cmd, req, paths);
	if (err)
		return err;

	return ovl_deltafs_get_layer_traps_locked(sb, ofs, paths, state);
}

static int ovl_deltafs_build_layers(struct ovl_fs *ofs,
				    const struct ovl_delta_paths *paths,
				    struct ovl_delta_state *state)
{
	const struct path *path;
	struct ovl_layer *layer;
	struct vfsmount *mnt;
	unsigned int i;
	int err;

	for (i = 0; i < state->numlayer; i++) {
		path = !i ? &paths->upper : &paths->lower[i - 1];
		layer = &state->layers[i];
		mnt = clone_private_mount(path);
		if (IS_ERR(mnt))
			return PTR_ERR(mnt);

		if (!i)
			mnt->mnt_flags &= ~(MNT_NOATIME | MNT_NODIRATIME |
					    MNT_RELATIME);
		else
			mnt->mnt_flags |= MNT_READONLY | MNT_NOATIME;

		layer->mnt = mnt;
		layer->idx = i;
		layer->fsid = 0;
		layer->fs = &ofs->fs[0];

		err = ovl_deltafs_build_checkpoint();
		if (err)
			return err;
	}

	if (!ovl_inuse_trylock(state->layers[0].mnt->mnt_root))
		return -EBUSY;
	state->upperdir_locked = true;

	return ovl_deltafs_build_checkpoint();
}

static void ovl_deltafs_init_work_view(struct ovl_fs *view,
				       const struct ovl_fs *ofs,
				       struct ovl_delta_state *state)
{
	memset(view, 0, sizeof(*view));
	view->numlayer = state->numlayer;
	view->numfs = ofs->numfs;
	view->layers = state->layers;
	view->fs = ofs->fs;
	view->workbasedir = state->workbasedir;
	view->config.upperdir = state->upperdir_name;
	view->config.workdir = state->workdir_name;
	view->config.lowerdirs = state->lowerdir_names;
	view->config.default_permissions = ofs->config.default_permissions;
	view->config.redirect_mode = ofs->config.redirect_mode;
	view->config.verity_mode = ofs->config.verity_mode;
	view->config.index = ofs->config.index;
	view->config.uuid = ofs->config.uuid;
	view->config.nfs_export = ofs->config.nfs_export;
	view->config.xino = ofs->config.xino;
	view->config.metacopy = ofs->config.metacopy;
	view->config.userxattr = ofs->config.userxattr;
	view->config.ovl_volatile = ofs->config.ovl_volatile;
	view->xino_mode = ofs->xino_mode;
}

static int ovl_deltafs_build_workdir(struct super_block *sb,
				     struct ovl_fs *ofs,
				     const struct ovl_delta_paths *paths,
				     struct ovl_delta_state *state)
{
	struct ovl_fs view;
	struct inode *trap;
	int err;

	state->workbasedir = dget(paths->work.dentry);
	err = ovl_deltafs_build_checkpoint();
	if (err)
		return err;

	if (!ovl_inuse_trylock(state->workbasedir))
		return -EBUSY;
	state->workdir_locked = true;
	err = ovl_deltafs_build_checkpoint();
	if (err)
		return err;

	trap = ovl_get_trap_inode(sb, state->workbasedir);
	if (IS_ERR(trap)) {
		err = PTR_ERR(trap);
		return err == -ELOOP ? -EINVAL : err;
	}
	state->workbasedir_trap = trap;
	err = ovl_deltafs_build_checkpoint();
	if (err)
		return err;

	ovl_deltafs_init_work_view(&view, ofs, state);
	err = ovl_make_workdir(sb, &view, &paths->work, true);
	state->workdir = view.workdir;
	state->workdir_trap = view.workdir_trap;
	if (err)
		return err;

	/* These are superblock-static in DeltaFS v1.  A weaker result for the
	 * new directory would make the active capability fields lie. */
	if (view.tmpfile != ofs->tmpfile || view.noxattr != ofs->noxattr ||
	    view.nofh != ofs->nofh)
		return -EOPNOTSUPP;

	return ovl_deltafs_build_checkpoint();
}

static int ovl_deltafs_build_root(struct ovl_fs *ofs,
				  struct ovl_delta_state *state)
{
	struct ovl_path *lowerstack = ovl_lowerstack(state->root_oe);
	struct path path;
	unsigned int i;
	int err;

	state->root_upperdentry = dget(state->layers[0].mnt->mnt_root);
	err = ovl_deltafs_build_checkpoint();
	if (err)
		return err;

	for (i = 0; i < state->numlayer - 1; i++) {
		lowerstack[i].layer = &state->layers[i + 1];
		lowerstack[i].dentry =
			dget(state->layers[i + 1].mnt->mnt_root);
		err = ovl_deltafs_build_checkpoint();
		if (err)
			return err;
	}

	path.mnt = state->layers[0].mnt;
	path.dentry = state->layers[0].mnt->mnt_root;
	state->root_impure =
		ovl_get_dir_xattr_val(ofs, &path, OVL_XATTR_IMPURE) == 'y';

	/* Match ovl_get_root(): the bottommost lower cannot contain xwhiteouts
	 * that hide an even lower layer. */
	for (i = 1; i + 1 < state->numlayer; i++) {
		path.mnt = state->layers[i].mnt;
		path.dentry = state->layers[i].mnt->mnt_root;
		if (ovl_get_opaquedir_val(ofs, &path) == 'x') {
			state->layers[i].has_xwhiteouts = true;
			state->root_xwhiteouts = true;
		}
	}

	return ovl_deltafs_build_checkpoint();
}

static int ovl_deltafs_build_state(struct file *file, unsigned int cmd,
				   const struct deltafs_ioc_switch_v1 *req,
				   const struct ovl_delta_paths *paths,
				   struct ovl_delta_state **statep)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct ovl_fs *ofs = OVL_FS(sb);
	struct ovl_delta_state *state;
	const struct cred *old_cred;
	int err;

	state = ovl_deltafs_alloc_state(req, paths);
	if (IS_ERR(state))
		return PTR_ERR(state);

	mutex_lock(&ofs->delta_lock);
	err = ovl_deltafs_prepare_locked(file, cmd, req, paths, state);
	mutex_unlock(&ofs->delta_lock);
	if (err)
		goto out_err;

	err = ovl_deltafs_build_layers(ofs, paths, state);
	if (err)
		goto out_err;

	old_cred = ovl_override_creds(sb);
	err = ovl_deltafs_build_workdir(sb, ofs, paths, state);
	if (!err)
		err = ovl_deltafs_build_root(ofs, state);
	revert_creds(old_cred);
	if (err)
		goto out_err;

	*statep = state;
	return 0;

out_err:
	ovl_deltafs_free_state(state);
	return err;
}

static int ovl_deltafs_final_revalidate_locked(
		struct file *file, unsigned int cmd,
		const struct deltafs_ioc_switch_v1 *req,
		const struct ovl_delta_paths *paths)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct ovl_fs *ofs = OVL_FS(sb);
	int err;

	err = ovl_deltafs_validate_generation(ofs, req->expected_generation);
	if (err)
		return err;
	if (file_dentry(file) != sb->s_root)
		return -ENOTTY;

	return ovl_deltafs_validate_command_locked(ofs, cmd, req, paths);
}

static void ovl_deltafs_update_root_locked(struct super_block *sb,
					   struct ovl_delta_state *state,
					   u64 generation)
{
	struct ovl_fs *ofs = OVL_FS(sb);
	struct dentry *root = sb->s_root;
	struct inode *inode = d_inode(root);
	struct ovl_inode *oi = OVL_I(inode);

	lockdep_assert_held_write(&inode->i_rwsem);
	lockdep_assert_held(&oi->lock);

	if (state->root_impure)
		ovl_set_flag(OVL_IMPURE, inode);
	else
		ovl_clear_flag(OVL_IMPURE, inode);

	if (state->root_xwhiteouts)
		ovl_dentry_set_xwhiteouts(root);
	else
		ovl_dentry_clear_flag(OVL_E_XWHITEOUTS, root);

	/* These are invariant for a writable DeltaFS root. */
	ovl_dentry_set_flag(OVL_E_CONNECTED, root);
	ovl_dentry_set_upper_alias(root);
	ovl_set_flag(OVL_WHITEOUTS, inode);
	ovl_set_upperdata(inode);

	ovl_copyattr(inode);
	ovl_copyflags(d_inode(oi->__upperdentry), inode);
	ovl_dentry_init_flags(root, oi->__upperdentry, oi->oe,
			      DCACHE_OP_WEAK_REVALIDATE);
	ovl_inode_version_inc(inode);
	WRITE_ONCE(oi->delta_generation, generation);

	/* The root real path used above must resolve through the new layer set. */
	WARN_ON_ONCE(ovl_upper_mnt(ofs)->mnt_root != oi->__upperdentry);
}

/*
 * Consume every owner in @state, install it as the active view, and turn
 * @old into a completely owned retired view.  The caller has performed the
 * final revalidation and holds ofs->delta_lock.  This function cannot fail.
 */
static void ovl_deltafs_commit_locked(struct super_block *sb,
				      struct ovl_delta_state *state,
				      struct ovl_delta_state *old)
{
	struct ovl_fs *ofs = OVL_FS(sb);
	struct inode *root_inode = d_inode(sb->s_root);
	struct ovl_inode *root_oi = OVL_I(root_inode);
	u64 old_generation = READ_ONCE(ofs->delta_generation);
	u64 new_generation = state->generation;

	lockdep_assert_held(&ofs->delta_lock);
	WARN_ON_ONCE(new_generation != old_generation + 1);

	/* Fixed order: delta_lock -> root i_rwsem -> root ovl_inode.lock. */
	inode_lock(root_inode);
	ovl_inode_lock(root_inode);

	old->generation = old_generation;
	old->numlayer = ofs->numlayer;
	old->layers = ofs->layers;
	old->workbasedir = ofs->workbasedir;
	old->workdir = ofs->workdir;
	old->whiteout = ofs->whiteout;
	old->workbasedir_trap = ofs->workbasedir_trap;
	old->workdir_trap = ofs->workdir_trap;
	old->upperdir_locked = ofs->upperdir_locked;
	old->workdir_locked = ofs->workdir_locked;
	old->no_shared_whiteout = ofs->no_shared_whiteout;
	old->upperdir_name = ofs->config.upperdir;
	old->workdir_name = ofs->config.workdir;
	old->lowerdir_names = ofs->config.lowerdirs;
	old->root_upperdentry = ovl_upperdentry_dereference(root_oi);
	old->root_oe = READ_ONCE(root_oi->oe);
	old->root_impure = ovl_test_flag(OVL_IMPURE, root_inode);
	old->root_xwhiteouts =
		ovl_dentry_has_xwhiteouts(sb->s_root);

	ofs->numlayer = state->numlayer;
	ofs->layers = state->layers;
	ofs->workbasedir = state->workbasedir;
	ofs->workdir = state->workdir;
	ofs->whiteout = state->whiteout;
	ofs->workbasedir_trap = state->workbasedir_trap;
	ofs->workdir_trap = state->workdir_trap;
	ofs->upperdir_locked = state->upperdir_locked;
	ofs->workdir_locked = state->workdir_locked;
	ofs->no_shared_whiteout = state->no_shared_whiteout;
	ofs->config.upperdir = state->upperdir_name;
	ofs->config.workdir = state->workdir_name;
	ofs->config.lowerdirs = state->lowerdir_names;
	WRITE_ONCE(root_oi->__upperdentry, state->root_upperdentry);
	WRITE_ONCE(root_oi->oe, state->root_oe);

	/* The target container no longer owns any resource installed above. */
	state->numlayer = 0;
	state->layers = NULL;
	state->workbasedir = NULL;
	state->workdir = NULL;
	state->whiteout = NULL;
	state->workbasedir_trap = NULL;
	state->workdir_trap = NULL;
	state->upperdir_locked = false;
	state->workdir_locked = false;
	state->no_shared_whiteout = false;
	state->upperdir_name = NULL;
	state->workdir_name = NULL;
	state->lowerdir_names = NULL;
	state->root_upperdentry = NULL;
	state->root_oe = NULL;

	ovl_deltafs_update_root_locked(sb, state, new_generation);

	ovl_inode_unlock(root_inode);
	inode_unlock(root_inode);

	list_add_tail(&old->node, &ofs->delta_retired);
	/* Pairs with generation acquire-loads in lookup and revalidation. */
	smp_store_release(&ofs->delta_generation, new_generation);
}

long ovl_deltafs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct deltafs_ioc_switch_v1 *req;
	struct super_block *sb = file_inode(file)->i_sb;
	struct ovl_delta_state *state = NULL;
	struct ovl_delta_state *old = NULL;
	struct ovl_delta_paths *paths;
	struct ovl_fs *ofs = OVL_FS(sb);
	int err;

	switch (cmd) {
	case DELTAFS_IOC_CHECKPOINT:
	case DELTAFS_IOC_RESTORE:
		break;
	default:
		return -ENOIOCTLCMD;
	}

	if (file_dentry(file) != sb->s_root)
		return -ENOTTY;
	if (!ns_capable(sb->s_user_ns, CAP_SYS_ADMIN))
		return -EPERM;

	/* The request struct is ~320 bytes; keep it off the ioctl stack frame. */
	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;
	if (copy_from_user(req, (void __user *)arg, sizeof(*req))) {
		err = -EFAULT;
		goto out_req;
	}

	err = ovl_deltafs_validate_abi(req);
	if (err)
		goto out_req;

	mutex_lock(&ofs->delta_lock);
	err = ovl_deltafs_validate_generation(ofs, req->expected_generation);
	mutex_unlock(&ofs->delta_lock);
	if (err)
		goto out_req;

	paths = ovl_deltafs_get_paths(req);
	if (IS_ERR(paths)) {
		err = PTR_ERR(paths);
		goto out_req;
	}

	mutex_lock(&ofs->delta_lock);
	err = ovl_deltafs_validate_generation(ofs, req->expected_generation);
	if (!err)
		err = ovl_deltafs_validate_features(sb, ofs);
	mutex_unlock(&ofs->delta_lock);
	if (err)
		goto out_paths;

	err = ovl_deltafs_validate_paths(ofs, paths);
	if (err)
		goto out_paths;
	err = ovl_deltafs_validate_empty(sb, paths);
	if (err)
		goto out_paths;

	err = ovl_deltafs_build_state(file, cmd, req, paths, &state);
	if (err)
		goto out_paths;

	old = kzalloc(sizeof(*old), GFP_KERNEL);
	if (!old) {
		err = -ENOMEM;
		goto out_state;
	}
	INIT_LIST_HEAD(&old->node);
	err = ovl_deltafs_build_checkpoint();
	if (err)
		goto out_state;

	mutex_lock(&ofs->delta_lock);
	err = ovl_deltafs_final_revalidate_locked(file, cmd, req, paths);
	if (!err)
		ovl_deltafs_commit_locked(sb, state, old);
	mutex_unlock(&ofs->delta_lock);
	if (!err) {
		/* Commit consumed all target owners and linked old into retired. */
		kfree(state);
		state = NULL;
		old = NULL;
	}

out_state:
	ovl_deltafs_free_state(old);
	ovl_deltafs_free_state(state);
out_paths:
	ovl_deltafs_put_paths(paths);
out_req:
	kfree(req);
	return err;
}

void ovl_deltafs_cleanup(struct ovl_fs *ofs)
{
	struct ovl_delta_state *state, *next;

	list_for_each_entry_safe(state, next, &ofs->delta_retired, node) {
		list_del_init(&state->node);
		ovl_deltafs_free_state(state);
	}
	mutex_destroy(&ofs->delta_lock);
}
