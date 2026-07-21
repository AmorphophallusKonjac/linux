// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P1 ioctl ABI test.
 *
 * Run this as a process with CAP_SYS_ADMIN in the OverlayFS user namespace:
 *
 *   ./p1_ioctl_test MERGED UPPER WORK LOWER [LOWER ...]
 *
 * MERGED is an already-mounted, writable OverlayFS root.  UPPER, WORK, and
 * LOWER are directories used only as fd arguments; P1 does not modify them.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/deltafs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	failures++;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void expect_ioctl(const char *name, int fd, unsigned long cmd,
			 const void *arg, int expected_errno)
{
	int ret;

	errno = 0;
	ret = ioctl(fd, cmd, arg);
	if (ret == -1 && errno == expected_errno) {
		printf("PASS: %s: %s\n", name, strerror(expected_errno));
		return;
	}

	if (ret == -1)
		fail("%s: expected %s, got %s", name, strerror(expected_errno),
		     strerror(errno));
	else
		fail("%s: expected %s, got success", name, strerror(expected_errno));
}

static void init_request(struct deltafs_ioc_switch_v1 *req, int upper_fd,
			 int work_fd, int nr_lower, const int *lower_fds)
{
	int i;

	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = 1; /* P1 initializes every mount to 1. */
	req->upper_fd = upper_fd;
	req->work_fd = work_fd;
	req->nr_lower = nr_lower;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		req->lower_fds[i] = -1;
	for (i = 0; i < nr_lower && i < DELTAFS_V1_MAX_LOWERS; i++)
		req->lower_fds[i] = lower_fds[i];
}

static int open_directory(const char *path)
{
	int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr, "open directory %s: %s\n", path, strerror(errno));
	return fd;
}

static void test_permission(int root_fd,
			    const struct deltafs_ioc_switch_v1 *req)
{
	pid_t child;
	int status;

	if (geteuid() != 0) {
		printf("SKIP: permission test requires starting as root\n");
		return;
	}

	child = fork();
	if (child < 0) {
		fail("fork for permission test: %s", strerror(errno));
		return;
	}
	if (!child) {
		if (setgid(65534) || setuid(65534))
			_exit(2);
		errno = 0;
		if (ioctl(root_fd, DELTAFS_IOC_CHECKPOINT, req) == -1 &&
		    errno == EPERM)
			_exit(0);
		_exit(1);
	}
	if (waitpid(child, &status, 0) != child) {
		fail("waitpid for permission test: %s", strerror(errno));
		return;
	}
	if (WIFEXITED(status) && !WEXITSTATUS(status))
		printf("PASS: missing CAP_SYS_ADMIN: %s\n", strerror(EPERM));
	else
		fail("missing CAP_SYS_ADMIN: expected %s", strerror(EPERM));
}

int main(int argc, char **argv)
{
	struct deltafs_ioc_switch_v1 req;
	int lower_fds[DELTAFS_V1_MAX_LOWERS] = { [0 ... DELTAFS_V1_MAX_LOWERS - 1] = -1 };
	char child_name[64] = "";
	int root_fd = -1, upper_fd = -1, work_fd = -1, regular_fd = -1;
	int child_fd = -1;
	int nr_lower;
	int i;

	if (argc < 5 || argc > 4 + DELTAFS_V1_MAX_LOWERS) {
		fprintf(stderr, "Usage: %s MERGED UPPER WORK LOWER [LOWER ...]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	nr_lower = argc - 4;
	root_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0) {
		fprintf(stderr, "open merged root %s: %s\n", argv[1], strerror(errno));
		goto out;
	}
	upper_fd = open_directory(argv[2]);
	work_fd = open_directory(argv[3]);
	if (upper_fd < 0 || work_fd < 0)
		goto out;
	for (i = 0; i < nr_lower; i++) {
		lower_fds[i] = open_directory(argv[4 + i]);
		if (lower_fds[i] < 0)
			goto out;
	}

	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);

	/* Permission is checked before the request itself.  An unprivileged run
	 * can therefore only exercise this boundary; run as root for the rest. */
	errno = 0;
	if (ioctl(root_fd, DELTAFS_IOC_CHECKPOINT, &req) == -1 && errno == EPERM) {
		printf("PASS: missing CAP_SYS_ADMIN: %s\n", strerror(EPERM));
		puts("Run this test with CAP_SYS_ADMIN to exercise the full P1 ABI matrix");
		goto out;
	}

	/* These two requests are valid in P1, whose deliberate terminal result is
	 * -EOPNOTSUPP until state construction and commit are implemented. */
	expect_ioctl("valid checkpoint", root_fd, DELTAFS_IOC_CHECKPOINT, &req,
		     EOPNOTSUPP);
	expect_ioctl("valid restore", root_fd, DELTAFS_IOC_RESTORE, &req,
		     EOPNOTSUPP);
	/* ovl_deltafs_ioctl() returns -ENOIOCTLCMD internally.  vfs_ioctl()
	 * translates that internal "not handled" sentinel to ENOTTY before the
	 * ioctl(2) caller can observe it. */
	expect_ioctl("unknown command", root_fd, _IO(DELTAFS_IOC_MAGIC, 0x7f),
		     &req, ENOTTY);
	expect_ioctl("bad user pointer", root_fd, DELTAFS_IOC_CHECKPOINT,
		     (const void *)1, EFAULT);

	req.size--;
	expect_ioctl("bad size", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.version++;
	expect_ioctl("bad version", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.flags = 1;
	expect_ioctl("non-zero flags", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.reserved[0] = 1;
	expect_ioctl("non-zero reserved", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.nr_lower = 0;
	expect_ioctl("zero lowers", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.nr_lower = DELTAFS_V1_MAX_LOWERS + 1;
	expect_ioctl("too many lowers", root_fd, DELTAFS_IOC_CHECKPOINT, &req, E2BIG);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.lower_fds[nr_lower] = lower_fds[0];
	expect_ioctl("used lower fd tail", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.expected_generation = 2;
	expect_ioctl("stale generation", root_fd, DELTAFS_IOC_CHECKPOINT, &req, ESTALE);
	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	req.upper_fd = -1;
	expect_ioctl("invalid upper fd", root_fd, DELTAFS_IOC_CHECKPOINT, &req, EBADF);

	regular_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (regular_fd < 0)
		fail("open /dev/null: %s", strerror(errno));
	else {
		init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
		req.upper_fd = regular_fd;
		expect_ioctl("non-directory upper fd", root_fd,
			     DELTAFS_IOC_CHECKPOINT, &req, ENOTDIR);
	}

	/* A child OverlayFS directory has ovl_dir_operations too, but is not the
	 * superblock root and must therefore be rejected by the dispatcher. */
	snprintf(child_name, sizeof(child_name), ".deltafs-p1-%ld", (long)getpid());
	if (mkdirat(root_fd, child_name, 0700)) {
		fail("mkdirat merged child for root-fd test: %s", strerror(errno));
	} else {
		child_fd = openat(root_fd, child_name,
				  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (child_fd < 0)
			fail("open merged child for root-fd test: %s", strerror(errno));
		else
			expect_ioctl("non-root control fd", child_fd,
				     DELTAFS_IOC_CHECKPOINT, &req, ENOTTY);
	}

	init_request(&req, upper_fd, work_fd, nr_lower, lower_fds);
	test_permission(root_fd, &req);

out:
	if (child_fd >= 0)
		close(child_fd);
	if (root_fd >= 0 && child_name[0] && unlinkat(root_fd, child_name,
						     AT_REMOVEDIR))
		fail("remove merged child after root-fd test: %s", strerror(errno));
	if (regular_fd >= 0)
		close(regular_fd);
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		if (lower_fds[i] >= 0)
			close(lower_fds[i]);
	if (work_fd >= 0)
		close(work_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (root_fd >= 0)
		close(root_fd);

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("All P1 ioctl tests passed");
	return EXIT_SUCCESS;
}
