// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P5 first-commit test.
 *
 * Run only in the project QEMU/KVM guest through p5_commit_test.sh.  The
 * program performs one generation-1 restore whose target view is visibly
 * different from the mounted view.  It verifies positive/negative lookup,
 * root binding, root readdir version invalidation, physical copy-up targets,
 * and exact generation publication.
 *
 * The same-control-fd readdir around the successful ioctl is an intentional
 * white-box exception to the v1 no-directory-fd-across-switch contract.  It
 * exists only to exercise the root inode version protocol.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/deltafs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHANGED_NAME "deltafs-p5-changed"
#define OLD_ONLY_NAME "deltafs-p5-old-only"
#define TARGET_ONLY_NAME "deltafs-p5-target-only"
#define POST_SWITCH_NAME "deltafs-p5-post-switch"

#define ACTIVE_PAYLOAD "deltafs-p5-active\n"
#define OLD_ONLY_PAYLOAD "deltafs-p5-old-only\n"
#define TARGET_PAYLOAD "deltafs-p5-target\n"
#define TARGET_ONLY_PAYLOAD "deltafs-p5-target-only\n"
#define BRANCH_PAYLOAD "deltafs-p5-branch\n"
#define POST_SWITCH_PAYLOAD "deltafs-p5-post-switch\n"

struct linux_dirent64 {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

struct root_listing {
	bool changed;
	bool old_only;
	bool target_only;
	bool post_switch;
};

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("FAIL: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void pass(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("PASS: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

static void write_all(int fd, const char *data, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = write(fd, data + done, len - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fail("write: %s", strerror(errno));
		}
		if (!ret)
			fail("write returned zero");
		done += ret;
	}
}

static void write_file_at(int dirfd, const char *name, const char *payload)
{
	int fd = openat(dirfd, name,
			O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd < 0)
		fail("open %s for write: %s", name, strerror(errno));
	write_all(fd, payload, strlen(payload));
	if (fsync(fd))
		fail("fsync %s: %s", name, strerror(errno));
	if (close(fd))
		fail("close %s: %s", name, strerror(errno));
}

static size_t read_file_at(int dirfd, const char *name, char *buf, size_t size)
{
	size_t done = 0;
	int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		fail("open %s for read: %s", name, strerror(errno));
	while (done < size) {
		ssize_t ret = read(fd, buf + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fail("read %s: %s", name, strerror(errno));
		}
		if (!ret)
			break;
		done += ret;
	}
	if (close(fd))
		fail("close %s: %s", name, strerror(errno));
	return done;
}

static void expect_contents(int dirfd, const char *name, const char *expected,
			    const char *stage)
{
	char buf[256];
	size_t expected_len = strlen(expected);
	size_t len = read_file_at(dirfd, name, buf, sizeof(buf));

	if (len != expected_len || memcmp(buf, expected, expected_len))
		fail("%s: unexpected contents for %s", stage, name);
}

static void expect_missing(int dirfd, const char *name, const char *stage)
{
	struct stat st;

	errno = 0;
	if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		if (errno == ENOENT)
			return;
		fail("%s: stat %s: %s", stage, name, strerror(errno));
	}
	fail("%s: %s unexpectedly exists", stage, name);
}

static int mkdir_open_at(int parent, const char *name, mode_t mode)
{
	int fd;

	if (mkdirat(parent, name, mode))
		fail("mkdir %s: %s", name, strerror(errno));
	fd = openat(parent, name, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		fail("open directory %s: %s", name, strerror(errno));
	return fd;
}

static int make_unique_subdir(int scratch_fd, char *name, size_t size)
{
	unsigned int seq;

	for (seq = 0; seq < 10000; seq++) {
		int len = snprintf(name, size, "p5-branch-%ld-%u",
				   (long)getpid(), seq);

		if (len < 0 || (size_t)len >= size)
			fail("could not construct branch name");
		if (!mkdirat(scratch_fd, name, 0700))
			return openat(scratch_fd, name,
				      O_PATH | O_DIRECTORY | O_CLOEXEC);
		if (errno != EEXIST)
			fail("mkdir branch %s: %s", name, strerror(errno));
	}
	fail("could not allocate a unique P5 branch");
	return -1;
}

static void read_root_listing(int fd, struct root_listing *listing)
{
	char buf[4096];

	memset(listing, 0, sizeof(*listing));
	for (;;) {
		long len = syscall(SYS_getdents64, fd, buf, sizeof(buf));
		long pos = 0;

		if (len < 0) {
			if (errno == EINTR)
				continue;
			fail("getdents64: %s", strerror(errno));
		}
		if (!len)
			return;

		while (pos < len) {
			struct linux_dirent64 *entry =
				(struct linux_dirent64 *)(buf + pos);

			if (entry->d_reclen < sizeof(*entry) ||
			    pos + entry->d_reclen > len)
				fail("malformed getdents64 record");
			if (!strcmp(entry->d_name, CHANGED_NAME))
				listing->changed = true;
			else if (!strcmp(entry->d_name, OLD_ONLY_NAME))
				listing->old_only = true;
			else if (!strcmp(entry->d_name, TARGET_ONLY_NAME))
				listing->target_only = true;
			else if (!strcmp(entry->d_name, POST_SWITCH_NAME))
				listing->post_switch = true;
			pos += entry->d_reclen;
		}
	}
}

static void expect_old_listing(const struct root_listing *listing)
{
	if (!listing->changed || !listing->old_only || listing->target_only)
		fail("pre-commit root readdir does not show the active view");
}

static void expect_target_listing(const struct root_listing *listing,
				  const char *stage)
{
	if (!listing->changed || listing->old_only || !listing->target_only)
		fail("%s: root readdir does not show the target view", stage);
}

static void init_request(struct deltafs_ioc_switch_v1 *req, int upper_fd,
			 int work_fd, int lower_fd, uint64_t generation)
{
	unsigned int i;

	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = generation;
	req->upper_fd = upper_fd;
	req->work_fd = work_fd;
	req->nr_lower = 1;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		req->lower_fds[i] = -1;
	req->lower_fds[0] = lower_fd;
}

static void expect_ioctl_errno(int fd, unsigned long cmd,
			       struct deltafs_ioc_switch_v1 *req,
			       int expected, const char *stage)
{
	int ret;

	errno = 0;
	ret = ioctl(fd, cmd, req);
	if (ret < 0 && errno == expected)
		return;
	if (ret < 0)
		fail("%s: expected %s, got %s", stage, strerror(expected),
		     strerror(errno));
	fail("%s: expected %s, got success", stage, strerror(expected));
}

int main(int argc, char **argv)
{
	struct deltafs_ioc_switch_v1 req;
	struct root_listing listing;
	struct stat root_stat, upper_stat;
	char branch_name[64];
	int root_fd, scratch_fd, active_upper_fd;
	int branch_fd, upper_fd, work_fd, target_fd;
	int post_root_fd, generation_fd;
	int i;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s MERGED SCRATCH ACTIVE_UPPER\n", argv[0]);
		return EXIT_FAILURE;
	}

	root_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (root_fd < 0)
		fail("open merged root: %s", strerror(errno));
	scratch_fd = open(argv[2], O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (scratch_fd < 0)
		fail("open scratch: %s", strerror(errno));
	active_upper_fd = open(argv[3], O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (active_upper_fd < 0)
		fail("open active upper: %s", strerror(errno));

	branch_fd = make_unique_subdir(scratch_fd, branch_name,
				       sizeof(branch_name));
	if (branch_fd < 0)
		fail("open unique branch %s: %s", branch_name, strerror(errno));
	upper_fd = mkdir_open_at(branch_fd, "upper", 0700);
	work_fd = mkdir_open_at(branch_fd, "work", 0700);
	target_fd = mkdir_open_at(branch_fd, "target-lower", 0755);
	if (fchmodat(branch_fd, "upper", 0711, 0))
		fail("chmod fresh upper: %s", strerror(errno));

	write_file_at(target_fd, CHANGED_NAME, TARGET_PAYLOAD);
	write_file_at(target_fd, TARGET_ONLY_NAME, TARGET_ONLY_PAYLOAD);
	write_file_at(root_fd, CHANGED_NAME, ACTIVE_PAYLOAD);
	write_file_at(root_fd, OLD_ONLY_NAME, OLD_ONLY_PAYLOAD);
	expect_missing(root_fd, TARGET_ONLY_NAME, "initial negative lookup");

	for (i = 0; i < 8; i++) {
		expect_contents(root_fd, CHANGED_NAME, ACTIVE_PAYLOAD,
				"positive-cache prewarm");
		expect_contents(root_fd, OLD_ONLY_NAME, OLD_ONLY_PAYLOAD,
				"stale-positive prewarm");
		expect_missing(root_fd, TARGET_ONLY_NAME,
			       "negative-cache prewarm");
	}
	pass("positive and negative lookup paths prewarmed");

	read_root_listing(root_fd, &listing);
	expect_old_listing(&listing);
	if (lseek(root_fd, 0, SEEK_SET) < 0)
		fail("rewind root before commit: %s", strerror(errno));
	pass("root readdir cache prewarmed on the control fd");

	if (syncfs(root_fd))
		fail("syncfs before restore: %s", strerror(errno));
	init_request(&req, upper_fd, work_fd, target_fd, 1);
	if (ioctl(root_fd, DELTAFS_IOC_RESTORE, &req))
		fail("first restore commit: %s", strerror(errno));
	pass("first restore committed generation 1 -> 2");

	/* Intentional white-box cross-switch use of the control fd. */
	read_root_listing(root_fd, &listing);
	expect_target_listing(&listing, "same-fd version invalidation");
	pass("root readdir version discarded the pre-commit cache");
	if (close(root_fd))
		fail("close commit control fd: %s", strerror(errno));

	post_root_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (post_root_fd < 0)
		fail("reopen merged root: %s", strerror(errno));
	read_root_listing(post_root_fd, &listing);
	expect_target_listing(&listing, "contract readdir reopen");
	pass("reopened root readdir shows only the target view");

	expect_contents(post_root_fd, CHANGED_NAME, TARGET_PAYLOAD,
			"changed positive after restore");
	expect_missing(post_root_fd, OLD_ONLY_NAME,
		       "removed positive after restore");
	expect_contents(post_root_fd, TARGET_ONLY_NAME, TARGET_ONLY_PAYLOAD,
			"old negative after restore");
	pass("positive, removed-positive, and negative lookups show target view");

	if (fstat(post_root_fd, &root_stat))
		fail("stat committed root: %s", strerror(errno));
	if (fstatat(branch_fd, "upper", &upper_stat, AT_SYMLINK_NOFOLLOW))
		fail("stat fresh upper: %s", strerror(errno));
	if ((root_stat.st_mode & 07777) != (upper_stat.st_mode & 07777))
		fail("committed root mode does not match fresh upper");
	pass("root attributes resolve through the fresh upper");

	write_file_at(post_root_fd, CHANGED_NAME, BRANCH_PAYLOAD);
	write_file_at(post_root_fd, POST_SWITCH_NAME, POST_SWITCH_PAYLOAD);
	expect_contents(post_root_fd, CHANGED_NAME, BRANCH_PAYLOAD,
			"merged branch write");
	expect_contents(upper_fd, CHANGED_NAME, BRANCH_PAYLOAD,
			"fresh upper copy-up");
	expect_contents(upper_fd, POST_SWITCH_NAME, POST_SWITCH_PAYLOAD,
			"fresh upper create");
	expect_contents(target_fd, CHANGED_NAME, TARGET_PAYLOAD,
			"immutable target lower");
	expect_missing(target_fd, POST_SWITCH_NAME, "target lower isolation");
	expect_contents(active_upper_fd, CHANGED_NAME, ACTIVE_PAYLOAD,
			"retired active upper");
	expect_contents(active_upper_fd, OLD_ONLY_NAME, OLD_ONLY_PAYLOAD,
			"retired old-only file");
	expect_missing(active_upper_fd, POST_SWITCH_NAME,
		       "retired upper isolation");
	pass("post-commit writes use fresh upper and preserve old backing views");

	if (syncfs(post_root_fd))
		fail("syncfs committed view: %s", strerror(errno));
	if (close(post_root_fd))
		fail("close post-commit root: %s", strerror(errno));

	generation_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (generation_fd < 0)
		fail("open generation control fd: %s", strerror(errno));
	req.expected_generation = 1;
	expect_ioctl_errno(generation_fd, DELTAFS_IOC_RESTORE, &req, ESTALE,
			   "old generation after commit");
	req.expected_generation = 2;
	req.upper_fd = -1;
	expect_ioctl_errno(generation_fd, DELTAFS_IOC_RESTORE, &req, EBADF,
			   "new generation reaches fd validation");
	pass("global generation was published exactly as 2");

	close(generation_fd);
	close(target_fd);
	close(work_fd);
	close(upper_fd);
	close(branch_fd);
	close(active_upper_fd);
	close(scratch_fd);
	printf("All P5 first-commit checks passed (branch %s)\n", branch_name);
	return EXIT_SUCCESS;
}
