// SPDX-License-Identifier: GPL-2.0

#include <assert.h>
#include <linux/deltafs.h>
#include <stddef.h>
#include <stdio.h>

static_assert(sizeof(struct deltafs_ioc_checkpoint_v2) == 64, "checkpoint size");
static_assert(offsetof(struct deltafs_ioc_checkpoint_v2, upper_fd) == 24,
	      "checkpoint upper_fd offset");
static_assert(offsetof(struct deltafs_ioc_checkpoint_v2, work_fd) == 28,
	      "checkpoint work_fd offset");
static_assert(offsetof(struct deltafs_ioc_checkpoint_v2, reserved) == 32,
	      "checkpoint reserved offset");

static_assert(sizeof(struct deltafs_ioc_restore_v2) == 584, "restore size");
static_assert(offsetof(struct deltafs_ioc_restore_v2, keep_bottom) == 24,
	      "restore keep_bottom offset");
static_assert(offsetof(struct deltafs_ioc_restore_v2, nr_fds) == 28,
	      "restore nr_fds offset");
static_assert(offsetof(struct deltafs_ioc_restore_v2, fds) == 32,
	      "restore fds offset");
static_assert(offsetof(struct deltafs_ioc_restore_v2, reserved) == 552,
	      "restore reserved offset");

static_assert(DELTAFS_V2_RESTORE_UPPER_FD == 0, "restore upper fd index");
static_assert(DELTAFS_V2_RESTORE_WORK_FD == 1, "restore work fd index");
static_assert(DELTAFS_V2_RESTORE_LOWER_BASE == 2, "restore lower fd base");
static_assert(DELTAFS_V2_MAX_RESTORE_FDS == 130, "restore fd capacity");
static_assert(_IOC_SIZE(DELTAFS_IOC_CHECKPOINT) == 64, "checkpoint ioctl size");
static_assert(_IOC_SIZE(DELTAFS_IOC_RESTORE) == 584, "restore ioctl size");

int main(void)
{
	puts("DeltaFS v2 UABI layout checks passed");
	return 0;
}
