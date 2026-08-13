/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_DELTAFS_H
#define _UAPI_LINUX_DELTAFS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define DELTAFS_ABI_VERSION		1
#define DELTAFS_V1_MAX_LOWERS		128
#define DELTAFS_IOC_MAGIC		0xdf

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

#endif /* _UAPI_LINUX_DELTAFS_H */
