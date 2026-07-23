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
#include <time.h>
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

/*
 * A controller always supplies a fresh, empty branch per build request.  A
 * successful P4 restore builds the overlay internal "work/" directory inside the
 * work base and leaves it behind after the state is freed; reusing that work
 * base for the next request would trip validate_empty (-ENOTEMPTY).  This helper
 * creates a unique empty upper/work/lower subtree next to @sibling and returns
 * its open O_PATH fds.  The caller owns the allocated fd array.
 */
static int make_open_dir(int parent, const char *name)
{
	if (mkdirat(parent, name, 0700))
		return -errno;
	return openat(parent, name, O_PATH | O_DIRECTORY | O_CLOEXEC);
}

/* Create a unique empty branch under the parent of @sibling.  Returns 0 on
 * success and fills *sub (the branch dir), *upper, *work and lower[i].  The
 * caller frees the lower array and closes every fd. */
static int fresh_branch(int sibling, int nr_lower, int *sub, int *upper,
			int *work, int **lower_out)
{
	int parent, subfd = -1, up = -1, wd = -1, *lowers = NULL;
	char name[48];
	unsigned int seq = (unsigned int)getpid();
	int i, err = 0;

	*sub = *upper = *work = -1;
	*lower_out = NULL;

	parent = openat(sibling, "..", O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (parent < 0)
		return -errno;

	for (;;) {
		snprintf(name, sizeof(name), "p1-branch-%u-%u",
			 seq, (unsigned)time(NULL));
		if (mkdirat(parent, name, 0700) == 0)
			break;
		if (errno != EEXIST) {
			err = -errno;
			close(parent);
			return err;
		}
		seq++;
	}

	subfd = openat(parent, name, O_PATH | O_DIRECTORY | O_CLOEXEC);
	close(parent);
	if (subfd < 0) {
		err = subfd;
		goto fail;
	}
	up = make_open_dir(subfd, "upper");
	if (up < 0) {
		err = up;
		goto fail;
	}
	wd = make_open_dir(subfd, "work");
	if (wd < 0) {
		err = wd;
		goto fail;
	}

	lowers = calloc(nr_lower, sizeof(*lowers));
	if (!lowers) {
		err = -ENOMEM;
		goto fail;
	}
	for (i = 0; i < nr_lower; i++) {
		char lname[24];

		snprintf(lname, sizeof(lname), "lower%d", i);
		lowers[i] = make_open_dir(subfd, lname);
		if (lowers[i] < 0) {
			err = lowers[i];
			goto fail;
		}
	}

	*sub = subfd;
	*upper = up;
	*work = wd;
	*lower_out = lowers;
	return 0;

fail:
	if (lowers) {
		int j;

		for (j = 0; j < nr_lower; j++)
			if (lowers[j] >= 0)
				close(lowers[j]);
		free(lowers);
	}
	if (wd >= 0)
		close(wd);
	if (up >= 0)
		close(up);
	if (subfd >= 0)
		close(subfd);
	return err;
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

	/* A restore whose upper/work/lower are fresh empty backing directories is
	 * a valid P4 build: it constructs a complete state, frees it, and returns
	 * -EOPNOTSUPP.  A successful build leaves the overlay internal "work/"
	 * inside the work base, so the next build-validating request must use its
	 * own fresh branch, or validate_empty would see -ENOTEMPTY. */
	{
		int sub = -1, up = -1, wd = -1, *lowers = NULL;

		if (fresh_branch(work_fd, nr_lower, &sub, &up, &wd, &lowers)) {
			fail("could not create a fresh restore branch");
		} else {
			init_request(&req, up, wd, nr_lower, lowers);
			expect_ioctl("valid restore", root_fd, DELTAFS_IOC_RESTORE,
				     &req, EOPNOTSUPP);
			for (i = 0; i < nr_lower; i++)
				if (lowers[i] >= 0)
					close(lowers[i]);
			free(lowers);
			if (wd >= 0)
				close(wd);
			if (up >= 0)
				close(up);
			if (sub >= 0)
				close(sub);
		}
	}

	/* The same request shape under CHECKPOINT fails command validation:
	 * checkpoint requires the lowers to match the active overlay layers, which
	 * these fresh directories do not, so it returns -EINVAL. */
	{
		int sub = -1, up = -1, wd = -1, *lowers = NULL;

		if (fresh_branch(work_fd, nr_lower, &sub, &up, &wd, &lowers)) {
			fail("could not create a fresh checkpoint branch");
		} else {
			init_request(&req, up, wd, nr_lower, lowers);
			expect_ioctl("checkpoint with non-matching lowers", root_fd,
				     DELTAFS_IOC_CHECKPOINT, &req, EINVAL);
			for (i = 0; i < nr_lower; i++)
				if (lowers[i] >= 0)
					close(lowers[i]);
			free(lowers);
			if (wd >= 0)
				close(wd);
			if (up >= 0)
				close(up);
			if (sub >= 0)
				close(sub);
		}
	}
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
