// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "deltafs_v2_common.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int e4_build_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *request,
				int upper_fd, int work_fd,
				uint64_t expected_generation)
{
	if (!request || !expected_generation) {
		errno = EINVAL;
		return -1;
	}
	if (upper_fd < 0 || work_fd < 0) {
		errno = EBADF;
		return -1;
	}
	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = expected_generation;
	request->upper_fd = upper_fd;
	request->work_fd = work_fd;
	return 0;
}

static int open_directory(const char *path, int flags)
{
	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	return open(path, flags | O_DIRECTORY | O_CLOEXEC);
}

int e4_checkpoint_v2(const char *merged, const char *upper, const char *work,
			     uint64_t expected_generation)
{
	struct deltafs_ioc_checkpoint_v2 request;
	int root_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int saved_errno;
	int ret = -1;

	root_fd = open_directory(merged, O_RDONLY);
	if (root_fd < 0)
		goto out;
	upper_fd = open_directory(upper, O_PATH);
	if (upper_fd < 0)
		goto out;
	work_fd = open_directory(work, O_PATH);
	if (work_fd < 0)
		goto out;
	if (e4_build_checkpoint_request(&request, upper_fd, work_fd,
					expected_generation))
		goto out;
	ret = ioctl(root_fd, DELTAFS_IOC_CHECKPOINT, &request);

out:
	saved_errno = errno;
	if (work_fd >= 0)
		close(work_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (root_fd >= 0)
		close(root_fd);
	errno = saved_errno;
	return ret;
}
