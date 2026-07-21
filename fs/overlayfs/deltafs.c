// SPDX-License-Identifier: GPL-2.0-only
/*
 * DeltaFS v1 ioctl ABI front-end.
 *
 * P1 deliberately stops after validating the control fd and the fixed-size
 * request. State construction and commit are added in later phases.
 */

#include <uapi/linux/deltafs.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/xattr.h>

#include "overlayfs.h"

static int ovl_deltafs_validate_fd(int fd)
{
	/* The UAPI requires O_PATH directory fds.  fget() deliberately rejects
	 * FMODE_PATH files, while fget_raw() obtains a reference to them. */
	struct file *file = fget_raw(fd);
	int err = 0;

	if (!file)
		return -EBADF;
	if (!d_is_dir(file_dentry(file)))
		err = -ENOTDIR;
	fput(file);

	return err;
}

static int ovl_deltafs_validate_request(struct ovl_fs *ofs,
					const struct deltafs_ioc_switch_v1 *req)
{
	unsigned int i;
	int err;

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

	if (req->expected_generation != READ_ONCE(ofs->delta_generation))
		return -ESTALE;

	err = ovl_deltafs_validate_fd(req->upper_fd);
	if (err)
		return err;
	err = ovl_deltafs_validate_fd(req->work_fd);
	if (err)
		return err;
	for (i = 0; i < req->nr_lower; i++) {
		err = ovl_deltafs_validate_fd(req->lower_fds[i]);
		if (err)
			return err;
	}

	return 0;
}

long ovl_deltafs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct deltafs_ioc_switch_v1 req;
	struct super_block *sb = file_inode(file)->i_sb;
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
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	mutex_lock(&ofs->delta_ioctl_lock);
	err = ovl_deltafs_validate_request(ofs, &req);
	mutex_unlock(&ofs->delta_ioctl_lock);
	if (err)
		return err;

	return -EOPNOTSUPP;
}
