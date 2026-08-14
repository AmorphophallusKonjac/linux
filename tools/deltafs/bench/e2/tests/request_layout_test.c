// SPDX-License-Identifier: GPL-2.0

#include "../e2_common.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int check_zeroed_reserved(const struct deltafs_ioc_switch_v1 *request)
{
	unsigned int i;

	if (request->reserved0)
		return -1;
	for (i = 0; i < sizeof(request->reserved) / sizeof(request->reserved[0]);
	     i++) {
		if (request->reserved[i])
			return -1;
	}
	return 0;
}

int main(void)
{
	struct deltafs_ioc_switch_v1 request;
	int lowers[DELTAFS_V1_MAX_LOWERS];
	unsigned int i;

	if (sizeof(request) != 584 || offsetof(typeof(request), lower_fds) != 40) {
		fprintf(stderr, "unexpected request layout: size=%zu lower_fds=%zu\n",
			sizeof(request), offsetof(typeof(request), lower_fds));
		return 1;
	}
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		lowers[i] = 1000 + (int)i;
	errno = 0;
	if (!e2_build_request(&request, 1, 2, lowers, 0, 1) ||
	    errno != EINVAL)
		return 1;
	errno = 0;
	if (!e2_build_request(&request, 1, 2, lowers, 1, 0) ||
	    errno != EINVAL)
		return 1;
	memset(&request, 0xa5, sizeof(request));
	if (e2_build_request(&request, 10, 11, lowers, 1, 7) ||
	    request.size != sizeof(request) ||
	    request.version != DELTAFS_ABI_VERSION || request.flags ||
	    request.expected_generation != 7 || request.upper_fd != 10 ||
	    request.work_fd != 11 || request.nr_lower != 1 ||
	    request.lower_fds[0] != 1000 || request.lower_fds[1] != -1 ||
	    request.lower_fds[DELTAFS_V1_MAX_LOWERS - 1] != -1 ||
	    check_zeroed_reserved(&request))
		return 1;
	memset(&request, 0xa5, sizeof(request));
	if (e2_build_request(&request, 10, 11, lowers,
			     DELTAFS_V1_MAX_LOWERS, 9) ||
	    request.lower_fds[0] != 1000 ||
	    request.lower_fds[DELTAFS_V1_MAX_LOWERS - 1] !=
		1000 + DELTAFS_V1_MAX_LOWERS - 1 ||
	    check_zeroed_reserved(&request))
		return 1;
	errno = 0;
	if (!e2_build_request(&request, 10, 11, lowers,
			      DELTAFS_V1_MAX_LOWERS + 1, 9) ||
	    errno != E2BIG)
		return 1;
	puts("PASS: E2 request layout and 0/1/128/129 lower boundaries");
	return 0;
}
