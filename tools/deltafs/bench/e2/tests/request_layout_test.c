// SPDX-License-Identifier: GPL-2.0

#include "../e2_common.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int checkpoint_reserved_is_zero(const struct deltafs_ioc_checkpoint_v2 *request)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (request->reserved[i])
			return 0;
	}
	return 1;
}

static int restore_reserved_is_zero(const struct deltafs_ioc_restore_v2 *request)
{
	unsigned int i;

	for (i = 0; i < 4; i++) {
		if (request->reserved[i])
			return 0;
	}
	return 1;
}

int main(void)
{
	struct deltafs_ioc_checkpoint_v2 checkpoint;
	struct deltafs_ioc_restore_v2 restore;
	int lowers[DELTAFS_V2_MAX_LOWERS];
	unsigned int i;

	if (sizeof(checkpoint) != 64 ||
	    offsetof(typeof(checkpoint), upper_fd) != 24 ||
	    offsetof(typeof(checkpoint), reserved) != 32 ||
	    sizeof(restore) != 584 ||
	    offsetof(typeof(restore), keep_bottom) != 24 ||
	    offsetof(typeof(restore), fds) != 32 ||
	    offsetof(typeof(restore), reserved) != 552) {
		fprintf(stderr, "unexpected DeltaFS v2 request layout\n");
		return 1;
	}
	for (i = 0; i < DELTAFS_V2_MAX_LOWERS; i++)
		lowers[i] = 1000 + (int)i;

	errno = 0;
	if (!e2_validate_checkpoint_source_depth(0) || errno != EINVAL)
		return 1;
	if (e2_validate_checkpoint_source_depth(127))
		return 1;
	errno = 0;
	if (!e2_validate_checkpoint_source_depth(128) || errno != E2BIG)
		return 1;

	errno = 0;
	if (!e2_build_checkpoint_request(&checkpoint, 10, 11, 0) ||
	    errno != EINVAL)
		return 1;
	memset(&checkpoint, 0xa5, sizeof(checkpoint));
	if (e2_build_checkpoint_request(&checkpoint, 10, 11, 7) ||
	    checkpoint.size != sizeof(checkpoint) ||
	    checkpoint.version != DELTAFS_ABI_VERSION || checkpoint.flags ||
	    checkpoint.expected_generation != 7 || checkpoint.upper_fd != 10 ||
	    checkpoint.work_fd != 11 ||
	    !checkpoint_reserved_is_zero(&checkpoint))
		return 1;

	errno = 0;
	if (!e2_build_restore_request(&restore, 10, 11, NULL, 0, 0, 1) ||
	    errno != EINVAL)
		return 1;
	errno = 0;
	if (!e2_build_restore_request(&restore, 10, 11, lowers, 1, 0, 0) ||
	    errno != EINVAL)
		return 1;
	memset(&restore, 0xa5, sizeof(restore));
	if (e2_build_restore_request(&restore, 10, 11, NULL, 0, 1, 7) ||
	    restore.size != sizeof(restore) ||
	    restore.version != DELTAFS_ABI_VERSION || restore.flags ||
	    restore.expected_generation != 7 || restore.keep_bottom != 1 ||
	    restore.nr_fds != DELTAFS_V2_RESTORE_LOWER_BASE ||
	    restore.fds[DELTAFS_V2_RESTORE_UPPER_FD] != 10 ||
	    restore.fds[DELTAFS_V2_RESTORE_WORK_FD] != 11 ||
	    restore.fds[DELTAFS_V2_RESTORE_LOWER_BASE] != -1 ||
	    restore.fds[DELTAFS_V2_MAX_RESTORE_FDS - 1] != -1 ||
	    !restore_reserved_is_zero(&restore))
		return 1;

	memset(&restore, 0xa5, sizeof(restore));
	if (e2_build_restore_request(&restore, 10, 11, lowers,
				     DELTAFS_V2_MAX_LOWERS, 0, 9) ||
	    restore.nr_fds != DELTAFS_V2_MAX_RESTORE_FDS ||
	    restore.fds[DELTAFS_V2_RESTORE_LOWER_BASE] != 1000 ||
	    restore.fds[DELTAFS_V2_MAX_RESTORE_FDS - 1] !=
		1000 + DELTAFS_V2_MAX_LOWERS - 1 ||
	    !restore_reserved_is_zero(&restore))
		return 1;
	errno = 0;
	if (!e2_build_restore_request(&restore, 10, 11, lowers,
				      DELTAFS_V2_MAX_LOWERS, 1, 9) ||
	    errno != E2BIG)
		return 1;
	errno = 0;
	if (!e2_build_restore_request(&restore, 10, 11, NULL, 0,
				      DELTAFS_V2_MAX_LOWERS + 1, 9) ||
	    errno != EINVAL)
		return 1;

	puts("PASS: E2 v2 request layouts and 0/1/128/129 lower boundaries");
	return 0;
}
