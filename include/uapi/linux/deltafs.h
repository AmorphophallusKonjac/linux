/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_DELTAFS_H
#define _UAPI_LINUX_DELTAFS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DELTAFS_ABI_VERSION		2
#define DELTAFS_V2_MAX_LOWERS		128
#define DELTAFS_V2_RESTORE_UPPER_FD	0
#define DELTAFS_V2_RESTORE_WORK_FD	1
#define DELTAFS_V2_RESTORE_LOWER_BASE	2
#define DELTAFS_V2_MAX_RESTORE_FDS \
	(DELTAFS_V2_MAX_LOWERS + DELTAFS_V2_RESTORE_LOWER_BASE)
#define DELTAFS_IOC_MAGIC		0xdf

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

#endif /* _UAPI_LINUX_DELTAFS_H */
