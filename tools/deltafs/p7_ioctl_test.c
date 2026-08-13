// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P7 native ioctl negative acceptance helper.
 *
 * The fixed DeltaFS v1 request intentionally contains no userspace pointers.
 * This prototype only accepts native userspace callers; 32-bit userspace is
 * explicitly outside the supported and tested v1 contract.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/deltafs.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define P7_BASELINE_NAME	"p7-abi-baseline"
#define P7_BASELINE_PAYLOAD	"p7-abi-baseline\n"

/* Keep an explicit assertion for the fixed native ABI layout. */
_Static_assert(sizeof(struct deltafs_ioc_switch_v1) == 584,
	       "DeltaFS v1 ioctl request layout changed");

static int failf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("FAIL: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	return -1;
}

static int join_path(char *result, size_t result_size, const char *parent,
		     const char *name)
{
	int length;

	length = snprintf(result, result_size, "%s/%s", parent, name);
	if (length < 0 || (size_t)length >= result_size) {
		errno = ENAMETOOLONG;
		return failf("path is too long under %s", parent);
	}
	return 0;
}

static int create_dir(const char *parent, const char *name, char *path,
		      size_t path_size)
{
	if (join_path(path, path_size, parent, name))
		return -1;
	if (mkdir(path, 0700))
		return failf("mkdir %s: %s", path, strerror(errno));
	return 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
	size_t done = 0;

	while (done < length) {
		ssize_t written = write(fd, buffer + done, length - done);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		done += (size_t)written;
	}
	return 0;
}

static int create_file(const char *parent, const char *name,
		       const char *contents, char *path, size_t path_size)
{
	int fd;

	if (join_path(path, path_size, parent, name))
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		return failf("create %s: %s", path, strerror(errno));
	if (write_all(fd, contents, strlen(contents))) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return failf("write %s: %s", path, strerror(errno));
	}
	if (close(fd))
		return failf("close %s: %s", path, strerror(errno));
	return 0;
}

static int open_control(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		failf("open merged root %s: %s", path, strerror(errno));
	return fd;
}

static int open_opath_dir(const char *path)
{
	int fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		failf("open O_PATH directory %s: %s", path, strerror(errno));
	return fd;
}

static int open_read_dir(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		failf("open ordinary directory %s: %s", path, strerror(errno));
	return fd;
}

static void init_request(struct deltafs_ioc_switch_v1 *request,
			 uint64_t generation, int upper_fd, int work_fd,
			 unsigned int nr_lower, const int *lower_fds)
{
	unsigned int i;

	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->upper_fd = upper_fd;
	request->work_fd = work_fd;
	request->nr_lower = nr_lower;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		request->lower_fds[i] = -1;
	for (i = 0; lower_fds && i < nr_lower &&
	     i < DELTAFS_V1_MAX_LOWERS; i++)
		request->lower_fds[i] = lower_fds[i];
}

static int expect_ioctl_errno(const char *stage, int fd, unsigned long command,
			      const void *argument, int expected_errno)
{
	int ret;

	errno = 0;
	ret = ioctl(fd, command, argument);
	if (ret == -1 && errno == expected_errno) {
		printf("PASS: %s: %s\n", stage, strerror(expected_errno));
		return 0;
	}
	if (ret == -1)
		return failf("%s: expected %s, got %s", stage,
			     strerror(expected_errno), strerror(errno));
	return failf("%s: expected %s, got success", stage,
		     strerror(expected_errno));
}

/*
 * There is deliberately no GET_STATE ioctl.  A stale request has to fail
 * before fd decoding, while a current request reaches the invalid upper fd.
 */
static int assert_generation(int root_fd, uint64_t generation,
			     const char *stage)
{
	struct deltafs_ioc_switch_v1 request;

	init_request(&request, generation + 1, -1, -1, 1, NULL);
	if (expect_ioctl_errno(stage, root_fd, DELTAFS_IOC_RESTORE, &request,
			       ESTALE))
		return -1;
	init_request(&request, generation, -1, -1, 1, NULL);
	return expect_ioctl_errno(stage, root_fd, DELTAFS_IOC_RESTORE, &request,
				  EBADF);
}

static int expect_unchanged(const char *stage, int root_fd, int call_fd,
			    unsigned long command, const void *argument,
			    int expected_errno, uint64_t generation)
{
	if (expect_ioctl_errno(stage, call_fd, command, argument, expected_errno))
		return -1;
	return assert_generation(root_fd, generation, stage);
}

static int expect_contents_at(int dir_fd, const char *name,
			      const char *expected, const char *stage)
{
	char contents[128];
	size_t expected_length = strlen(expected);
	ssize_t length;
	int fd;

	if (expected_length >= sizeof(contents))
		return failf("internal expected payload is too long");
	fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return failf("%s: open %s: %s", stage, name, strerror(errno));
	length = read(fd, contents, sizeof(contents));
	if (length < 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return failf("%s: read %s: %s", stage, name, strerror(errno));
	}
	if (close(fd))
		return failf("%s: close %s: %s", stage, name, strerror(errno));
	if ((size_t)length != expected_length ||
	    memcmp(contents, expected, expected_length))
		return failf("%s: unexpected contents in %s", stage, name);
	printf("PASS: %s: %s remains visible\n", stage, name);
	return 0;
}

static int send_fd(int socket_fd, int fd)
{
	char byte = 'x';
	char control[CMSG_SPACE(sizeof(fd))];
	struct cmsghdr *cmsg;
	struct iovec iov = {
		.iov_base = &byte,
		.iov_len = sizeof(byte),
	};
	struct msghdr message = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};

	memset(control, 0, sizeof(control));
	cmsg = CMSG_FIRSTHDR(&message);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
	return sendmsg(socket_fd, &message, 0) == (ssize_t)sizeof(byte) ? 0 : -1;
}

static int receive_fd(int socket_fd)
{
	char byte;
	char control[CMSG_SPACE(sizeof(int))];
	struct cmsghdr *cmsg;
	struct iovec iov = {
		.iov_base = &byte,
		.iov_len = sizeof(byte),
	};
	struct msghdr message = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};
	int received = -1;

	memset(control, 0, sizeof(control));
	if (recvmsg(socket_fd, &message, 0) != (ssize_t)sizeof(byte))
		return -1;
	cmsg = CMSG_FIRSTHDR(&message);
	if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
	    cmsg->cmsg_type != SCM_RIGHTS ||
	    cmsg->cmsg_len != CMSG_LEN(sizeof(received))) {
		errno = EPROTO;
		return -1;
	}
	memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
	return received;
}

/*
 * An fd received from a child after CLONE_NEWNS has a distinct vfsmount even
 * when it resolves to the same backing superblock.  It must be rejected in
 * combination with the parent's upper/work mount pair.
 */
static int open_other_mount_namespace(const char *path)
{
	int sockets[2] = { -1, -1 };
	pid_t child;
	int status;
	int fd = -1;

	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sockets))
		return failf("socketpair for mount-namespace fd: %s", strerror(errno));
	child = fork();
	if (child < 0) {
		close(sockets[0]);
		close(sockets[1]);
		return failf("fork for mount-namespace fd: %s", strerror(errno));
	}
	if (!child) {
		int child_fd;

		close(sockets[0]);
		if (unshare(CLONE_NEWNS))
			_exit(1);
		child_fd = open(path, O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (child_fd < 0)
			_exit(2);
		if (send_fd(sockets[1], child_fd))
			_exit(3);
		close(child_fd);
		close(sockets[1]);
		_exit(0);
	}
	close(sockets[1]);
	fd = receive_fd(sockets[0]);
	if (fd < 0)
		failf("receive mount-namespace fd: %s", strerror(errno));
	close(sockets[0]);
	if (waitpid(child, &status, 0) != child) {
		if (fd >= 0)
			close(fd);
		return failf("wait for mount-namespace child: %s", strerror(errno));
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		if (fd >= 0)
			close(fd);
		return failf("mount-namespace child failed (%d)", status);
	}
	return fd;
}

static int expect_permission_denied(int root_fd,
				  const struct deltafs_ioc_switch_v1 *request)
{
	pid_t child;
	int status;

	child = fork();
	if (child < 0)
		return failf("fork for permission test: %s", strerror(errno));
	if (!child) {
		if (setgid(65534) || setuid(65534))
			_exit(2);
		errno = 0;
		if (ioctl(root_fd, DELTAFS_IOC_CHECKPOINT, request) == -1 &&
		    errno == EPERM)
			_exit(0);
		_exit(1);
	}
	if (waitpid(child, &status, 0) != child)
		return failf("wait for permission test: %s", strerror(errno));
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return failf("missing CAP_SYS_ADMIN did not return EPERM");
	puts("PASS: missing CAP_SYS_ADMIN: Operation not permitted");
	return 0;
}

static int parse_generation(const char *argument, uint64_t *generation)
{
	char *end;
	unsigned long long value;

	errno = 0;
	value = strtoull(argument, &end, 10);
	if (errno || !argument[0] || *end || !value || value == ULLONG_MAX)
		return failf("invalid generation '%s'", argument);
	*generation = (uint64_t)value;
	return 0;
}

static int run_negative(const char *merged, const char *case_root,
			uint64_t generation, const char *active_upper_path,
			const char *base_path, const char *extra_path)
{
	struct deltafs_ioc_switch_v1 request;
	char upper_path[PATH_MAX];
	char work_path[PATH_MAX];
	char lower_path[PATH_MAX];
	char nonpath_path[PATH_MAX];
	char regular_path[PATH_MAX];
	char cross_ns_path[PATH_MAX];
	char max_lower_path[PATH_MAX];
	int max_lowers[DELTAFS_V1_MAX_LOWERS];
	int duplicate[2];
	int reversed[2];
	int current_upper[1];
	int normal_lower[1];
	int root_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int lower_fd = -1;
	int nonpath_fd = -1;
	int regular_fd = -1;
	int cross_ns_fd = -1;
	int active_upper_fd = -1;
	int base_fd = -1;
	int extra_fd = -1;
	int child_fd = -1;
	int closed_fd = -1;
	unsigned int i;
	int ret = EXIT_FAILURE;

	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		max_lowers[i] = -1;
	if (create_dir(case_root, "upper", upper_path, sizeof(upper_path)) ||
	    create_dir(case_root, "work", work_path, sizeof(work_path)) ||
	    create_dir(case_root, "lower", lower_path, sizeof(lower_path)) ||
	    create_dir(case_root, "nonpath", nonpath_path, sizeof(nonpath_path)) ||
	    create_dir(case_root, "cross-ns", cross_ns_path,
		       sizeof(cross_ns_path)) ||
	    create_file(case_root, "regular", "regular\n", regular_path,
			sizeof(regular_path)))
		goto out;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++) {
		char name[32];

		if (snprintf(name, sizeof(name), "lower-%02u", i) >=
		    (int)sizeof(name)) {
			failf("internal lower name overflow");
			goto out;
		}
		if (create_dir(case_root, name, max_lower_path,
			       sizeof(max_lower_path)))
			goto out;
		max_lowers[i] = open_opath_dir(max_lower_path);
		if (max_lowers[i] < 0)
			goto out;
	}

	root_fd = open_control(merged);
	upper_fd = open_opath_dir(upper_path);
	work_fd = open_opath_dir(work_path);
	lower_fd = open_opath_dir(lower_path);
	nonpath_fd = open_read_dir(nonpath_path);
	regular_fd = open(regular_path, O_PATH | O_CLOEXEC);
	if (regular_fd < 0)
		failf("open O_PATH regular file %s: %s", regular_path,
		      strerror(errno));
	active_upper_fd = open_opath_dir(active_upper_path);
	base_fd = open_opath_dir(base_path);
	extra_fd = open_opath_dir(extra_path);
	if (root_fd < 0 || upper_fd < 0 || work_fd < 0 || lower_fd < 0 ||
	    nonpath_fd < 0 || regular_fd < 0 || active_upper_fd < 0 ||
	    base_fd < 0 || extra_fd < 0)
		goto out;
	cross_ns_fd = open_other_mount_namespace(cross_ns_path);
	if (cross_ns_fd < 0)
		goto out;
	child_fd = openat(root_fd, "p7-abi-child",
			  O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (child_fd < 0) {
		failf("open lower child p7-abi-child: %s", strerror(errno));
		goto out;
	}
	normal_lower[0] = lower_fd;

	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	if (expect_unchanged("unknown DeltaFS command", root_fd, root_fd,
			     _IO(DELTAFS_IOC_MAGIC, 0x7f), &request, ENOTTY,
			     generation) ||
	    expect_unchanged("bad userspace pointer", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, (const void *)1, EFAULT, generation))
		goto out;

	request.size--;
	if (expect_unchanged("bad request size", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	request.version++;
	if (expect_unchanged("bad ABI version", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	request.flags = 1;
	if (expect_unchanged("non-zero flags", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	request.reserved0 = 1;
	if (expect_unchanged("non-zero reserved0", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	request.reserved[0] = 1;
	if (expect_unchanged("non-zero reserved array", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 0, NULL);
	if (expect_unchanged("zero lowers", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd,
		     DELTAFS_V1_MAX_LOWERS + 1, NULL);
	if (expect_unchanged("129 lowers", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, E2BIG, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	request.lower_fds[1] = lower_fd;
	if (expect_unchanged("used lower-fd tail", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation + 1, upper_fd, work_fd, 1,
		     normal_lower);
	if (expect_unchanged("stale generation", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, ESTALE, generation))
		goto out;

	init_request(&request, generation, -1, work_fd, 1, normal_lower);
	if (expect_unchanged("closed upper fd", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EBADF, generation))
		goto out;
	init_request(&request, generation, regular_fd, work_fd, 1, normal_lower);
	if (expect_unchanged("non-directory upper fd", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, ENOTDIR, generation))
		goto out;
	init_request(&request, generation, nonpath_fd, work_fd, 1, normal_lower);
	if (expect_unchanged("directory without O_PATH", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, upper_fd, 1, normal_lower);
	if (expect_unchanged("overlapping upper and work", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	duplicate[0] = lower_fd;
	duplicate[1] = lower_fd;
	init_request(&request, generation, upper_fd, work_fd, 2, duplicate);
	if (expect_unchanged("duplicate lower", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd, 1, &extra_fd);
	if (expect_unchanged("different backing superblock", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EXDEV, generation))
		goto out;
	init_request(&request, generation, upper_fd, cross_ns_fd, 1,
		     normal_lower);
	if (expect_unchanged("cross-mount-namespace work fd", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	current_upper[0] = active_upper_fd;
	init_request(&request, generation, upper_fd, work_fd, 1, current_upper);
	if (expect_unchanged("restore contains current upper", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EINVAL, generation))
		goto out;
	reversed[0] = base_fd;
	reversed[1] = active_upper_fd;
	init_request(&request, generation, upper_fd, work_fd, 2, reversed);
	if (expect_unchanged("checkpoint current-chain order", root_fd, root_fd,
			     DELTAFS_IOC_CHECKPOINT, &request, EINVAL, generation))
		goto out;
	init_request(&request, generation, upper_fd, work_fd,
		     DELTAFS_V1_MAX_LOWERS, max_lowers);
	if (expect_unchanged("exactly 128 lowers", root_fd, root_fd,
			     DELTAFS_IOC_CHECKPOINT, &request, EINVAL, generation))
		goto out;

	init_request(&request, generation, upper_fd, work_fd, 1, normal_lower);
	if (expect_unchanged("non-root control fd", root_fd, child_fd,
			     DELTAFS_IOC_CHECKPOINT, &request, ENOTTY, generation) ||
	    expect_permission_denied(root_fd, &request) ||
	    assert_generation(root_fd, generation, "permission failure"))
		goto out;
	if (expect_contents_at(root_fd, P7_BASELINE_NAME, P7_BASELINE_PAYLOAD,
			       "negative ABI matrix"))
		goto out;

	/* No descriptor-allocating operation occurs between close and ioctl, so
	 * the lowest available duplicate is stable even with a small RLIMIT_NOFILE. */
	closed_fd = fcntl(upper_fd, F_DUPFD_CLOEXEC, 0);
	if (closed_fd < 0) {
		failf("make a closed descriptor: %s", strerror(errno));
		goto out;
	}
	if (close(closed_fd)) {
		failf("make a closed descriptor: %s", strerror(errno));
		closed_fd = -1;
		goto out;
	}
	init_request(&request, generation, closed_fd, work_fd, 1, normal_lower);
	if (expect_unchanged("actually closed O_PATH fd", root_fd, root_fd,
			     DELTAFS_IOC_RESTORE, &request, EBADF, generation))
		goto out;

	puts("All P7 native ABI-negative checks passed");
	ret = EXIT_SUCCESS;
out:
	if (child_fd >= 0)
		close(child_fd);
	if (cross_ns_fd >= 0)
		close(cross_ns_fd);
	if (extra_fd >= 0)
		close(extra_fd);
	if (base_fd >= 0)
		close(base_fd);
	if (active_upper_fd >= 0)
		close(active_upper_fd);
	if (regular_fd >= 0)
		close(regular_fd);
	if (nonpath_fd >= 0)
		close(nonpath_fd);
	if (lower_fd >= 0)
		close(lower_fd);
	if (work_fd >= 0)
		close(work_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (root_fd >= 0)
		close(root_fd);
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		if (max_lowers[i] >= 0)
			close(max_lowers[i]);
	return ret;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s negative MERGED CASE_ROOT GENERATION ACTIVE_UPPER BASE EXTRA_DIR\n",
		program);
}

int main(int argc, char **argv)
{
	uint64_t generation = 0;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "negative")) {
		if (argc != 8) {
			usage(argv[0]);
			return EXIT_FAILURE;
		}
		if (parse_generation(argv[4], &generation))
			return EXIT_FAILURE;
		return run_negative(argv[2], argv[3], generation, argv[5], argv[6],
				    argv[7]);
	}
	usage(argv[0]);
	return EXIT_FAILURE;
}
