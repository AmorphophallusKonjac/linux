// SPDX-License-Identifier: GPL-2.0
/*
 * Confirm the exact DeltaFS runtime generation without adding a query UAPI.
 * A stale request must fail before fd decoding with ESTALE, while the current
 * generation reaches the deliberately invalid upper fd and returns EBADF.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/deltafs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void init_request(struct deltafs_ioc_switch_v1 *request,
			 uint64_t generation)
{
	unsigned int i;

	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->upper_fd = -1;
	request->work_fd = -1;
	request->nr_lower = 1;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		request->lower_fds[i] = -1;
}

static int expect_errno(int fd, struct deltafs_ioc_switch_v1 *request,
			int expected, const char *stage)
{
	errno = 0;
	if (ioctl(fd, DELTAFS_IOC_RESTORE, request) < 0 &&
	    errno == expected)
		return 0;
	if (errno)
		fprintf(stderr, "FAIL: %s: expected %s, got %s\n", stage,
			strerror(expected), strerror(errno));
	else
		fprintf(stderr, "FAIL: %s: expected %s, got success\n", stage,
			strerror(expected));
	return -1;
}

int main(int argc, char **argv)
{
	struct deltafs_ioc_switch_v1 request;
	char *end;
	uint64_t generation;
	unsigned long long parsed;
	int fd;
	int ret = EXIT_FAILURE;

	if (argc != 3) {
		fprintf(stderr, "Usage: %s MERGED EXPECTED_GENERATION\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	errno = 0;
	parsed = strtoull(argv[2], &end, 10);
	if (errno || !argv[2][0] || *end || !parsed ||
	    parsed > UINT64_MAX) {
		fprintf(stderr, "FAIL: invalid generation '%s'\n", argv[2]);
		return EXIT_FAILURE;
	}
	generation = (uint64_t)parsed;
	fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open merged root: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	init_request(&request, generation - 1);
	if (expect_errno(fd, &request, ESTALE, "stale generation"))
		goto out;
	request.expected_generation = generation;
	if (expect_errno(fd, &request, EBADF, "current generation"))
		goto out;
	printf("PASS: kernel generation is exactly %" PRIu64 "\n", generation);
	ret = EXIT_SUCCESS;
out:
	if (close(fd)) {
		fprintf(stderr, "FAIL: close merged root: %s\n", strerror(errno));
		ret = EXIT_FAILURE;
	}
	return ret;
}
