// SPDX-License-Identifier: GPL-2.0

#include "../deltafs_v2_common.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
	struct deltafs_ioc_checkpoint_v2 request;
	unsigned int i;

	if (sizeof(request) != 64 ||
	    offsetof(typeof(request), upper_fd) != 24 ||
	    offsetof(typeof(request), work_fd) != 28 ||
	    offsetof(typeof(request), reserved) != 32) {
		fprintf(stderr, "unexpected DeltaFS v2 checkpoint layout\n");
		return 1;
	}

	errno = 0;
	if (!e2_build_checkpoint_request(&request, 10, 11, 0) ||
	    errno != EINVAL)
		return 1;
	errno = 0;
	if (!e2_build_checkpoint_request(&request, -1, 11, 1) ||
	    errno != EBADF)
		return 1;

	memset(&request, 0xa5, sizeof(request));
	if (e2_build_checkpoint_request(&request, 10, 11, 1) ||
	    request.size != sizeof(request) ||
	    request.version != DELTAFS_ABI_VERSION || request.flags ||
	    request.expected_generation != 1 || request.upper_fd != 10 ||
	    request.work_fd != 11)
		return 1;
	for (i = 0; i < 4; i++)
		if (request.reserved[i])
			return 1;

	puts("PASS: E2 DeltaFS v2 checkpoint request layout");
	return 0;
}
