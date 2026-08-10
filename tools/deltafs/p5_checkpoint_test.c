// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P5 checkpoint and multi-commit test.
 *
 * Run only in the project QEMU/KVM guest through p5_checkpoint_test.sh.
 * The program performs three commits on one OverlayFS mount:
 *
 *   generation 1 --checkpoint--> generation 2
 *   generation 2 ----restore---> generation 3
 *   generation 3 --checkpoint--> generation 4
 *
 * This exercises checkpoint trap reuse, increasing layer depth, historical
 * restore, multiple simultaneously retired views, and physical isolation of
 * every frozen/retired upper directory.
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
#include <unistd.h>

#define BASE_NAME "deltafs-p5-base-only"
#define STATE_NAME "deltafs-p5-state"
#define SNAPSHOT_ONLY_NAME "deltafs-p5-snapshot-only"
#define BRANCH_ONLY_NAME "deltafs-p5-branch-only"
#define RESTORE_ONLY_NAME "deltafs-p5-restore-only"
#define FINAL_ONLY_NAME "deltafs-p5-final-only"

#define BASE_PAYLOAD "deltafs-p5-base\n"
#define SNAPSHOT_PAYLOAD "deltafs-p5-snapshot\n"
#define SNAPSHOT_ONLY_PAYLOAD "deltafs-p5-snapshot-only\n"
#define BRANCH_PAYLOAD "deltafs-p5-branch\n"
#define BRANCH_ONLY_PAYLOAD "deltafs-p5-branch-only\n"
#define RESTORE_PAYLOAD "deltafs-p5-restore\n"
#define RESTORE_ONLY_PAYLOAD "deltafs-p5-restore-only\n"
#define FINAL_PAYLOAD "deltafs-p5-final\n"
#define FINAL_ONLY_PAYLOAD "deltafs-p5-final-only\n"

struct branch {
	int dir_fd;
	int upper_fd;
	int work_fd;
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

static int open_dir(const char *path, int flags)
{
	int fd = open(path, flags | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		fail("open directory %s: %s", path, strerror(errno));
	return fd;
}

static int mkdir_open_at(int parent_fd, const char *name, mode_t mode)
{
	int fd;

	if (mkdirat(parent_fd, name, mode))
		fail("mkdir %s: %s", name, strerror(errno));
	fd = openat(parent_fd, name, O_PATH | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0)
		fail("open directory %s: %s", name, strerror(errno));
	return fd;
}

static struct branch create_branch(int scratch_fd, const char *name)
{
	struct branch branch = {
		.dir_fd = -1,
		.upper_fd = -1,
		.work_fd = -1,
	};

	branch.dir_fd = mkdir_open_at(scratch_fd, name, 0700);
	branch.upper_fd = mkdir_open_at(branch.dir_fd, "upper", 0755);
	branch.work_fd = mkdir_open_at(branch.dir_fd, "work", 0700);
	return branch;
}

static void close_fd(int fd, const char *what)
{
	if (fd >= 0 && close(fd))
		fail("close %s: %s", what, strerror(errno));
}

static void close_branch(struct branch *branch, const char *name)
{
	close_fd(branch->work_fd, name);
	close_fd(branch->upper_fd, name);
	close_fd(branch->dir_fd, name);
	branch->work_fd = -1;
	branch->upper_fd = -1;
	branch->dir_fd = -1;
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

static void write_file_at(int dir_fd, const char *name, const char *payload)
{
	int fd = openat(dir_fd, name,
			O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (fd < 0)
		fail("open %s for write: %s", name, strerror(errno));
	write_all(fd, payload, strlen(payload));
	if (fsync(fd))
		fail("fsync %s: %s", name, strerror(errno));
	close_fd(fd, name);
}

static size_t read_file_at(int dir_fd, const char *name, char *buf,
			   size_t size)
{
	size_t done = 0;
	int fd = openat(dir_fd, name, O_RDONLY | O_CLOEXEC);

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
	close_fd(fd, name);
	return done;
}

static void expect_contents(int dir_fd, const char *name,
			    const char *expected, const char *stage)
{
	char buf[256];
	size_t expected_len = strlen(expected);
	size_t len = read_file_at(dir_fd, name, buf, sizeof(buf));

	if (len != expected_len || memcmp(buf, expected, expected_len))
		fail("%s: unexpected contents for %s", stage, name);
}

static void expect_missing(int dir_fd, const char *name, const char *stage)
{
	struct stat st;

	errno = 0;
	if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
		if (errno == ENOENT)
			return;
		fail("%s: stat %s: %s", stage, name, strerror(errno));
	}
	fail("%s: %s unexpectedly exists", stage, name);
}

static struct stat file_stat_at(int dir_fd, const char *name,
				const char *stage)
{
	struct stat st;

	if (fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW))
		fail("%s: stat %s: %s", stage, name, strerror(errno));
	return st;
}

static void expect_metadata_unchanged(int dir_fd, const char *name,
				      const struct stat *before,
				      const char *stage)
{
	struct stat after = file_stat_at(dir_fd, name, stage);

	if (after.st_dev != before->st_dev || after.st_ino != before->st_ino ||
	    after.st_mode != before->st_mode || after.st_size != before->st_size ||
	    after.st_mtim.tv_sec != before->st_mtim.tv_sec ||
	    after.st_mtim.tv_nsec != before->st_mtim.tv_nsec)
		fail("%s: metadata changed for frozen file %s", stage, name);
}

static void expect_renamed_directory(int fd, int parent_fd, const char *name,
				     const char *stage)
{
	struct stat from_fd;
	struct stat from_name;

	if (fstat(fd, &from_fd))
		fail("%s: fstat renamed directory: %s", stage, strerror(errno));
	if (fstatat(parent_fd, name, &from_name, AT_SYMLINK_NOFOLLOW))
		fail("%s: stat renamed directory %s: %s", stage, name,
		     strerror(errno));
	if (from_fd.st_dev != from_name.st_dev ||
	    from_fd.st_ino != from_name.st_ino)
		fail("%s: renamed directory identity changed", stage);
}

static void init_request(struct deltafs_ioc_switch_v1 *req, int upper_fd,
			 int work_fd, const int *lower_fds,
			 unsigned int nr_lower, uint64_t generation)
{
	unsigned int i;

	if (!nr_lower || nr_lower > DELTAFS_V1_MAX_LOWERS)
		fail("invalid lower count %u", nr_lower);
	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = generation;
	req->upper_fd = upper_fd;
	req->work_fd = work_fd;
	req->nr_lower = nr_lower;
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		req->lower_fds[i] = -1;
	for (i = 0; i < nr_lower; i++)
		req->lower_fds[i] = lower_fds[i];
}

static void switch_view(const char *merged, unsigned long cmd,
			struct deltafs_ioc_switch_v1 *req, const char *stage)
{
	int root_fd = open_dir(merged, O_RDONLY);

	if (syncfs(root_fd))
		fail("%s: syncfs: %s", stage, strerror(errno));
	if (ioctl(root_fd, cmd, req))
		fail("%s: ioctl: %s", stage, strerror(errno));
	close_fd(root_fd, stage);
	pass("%s", stage);
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
	struct stat base_stat;
	struct stat checkpoint_a_stat;
	struct stat checkpoint_b_stat;
	struct stat generation_2_stat;
	struct branch generation_2;
	struct branch generation_3;
	struct branch generation_4;
	int lower_fds[3];
	int scratch_fd;
	int active_upper_fd;
	int active_lower_fd;
	int root_fd;

	if (argc != 5) {
		fprintf(stderr,
			"Usage: %s MERGED SCRATCH ACTIVE_UPPER ACTIVE_LOWER\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	scratch_fd = open_dir(argv[2], O_PATH);
	active_upper_fd = open_dir(argv[3], O_PATH);
	active_lower_fd = open_dir(argv[4], O_PATH);

	root_fd = open_dir(argv[1], O_RDONLY);
	expect_contents(root_fd, BASE_NAME, BASE_PAYLOAD, "initial lower");
	base_stat = file_stat_at(active_lower_fd, BASE_NAME, "base baseline");
	write_file_at(root_fd, STATE_NAME, SNAPSHOT_PAYLOAD);
	write_file_at(root_fd, SNAPSHOT_ONLY_NAME, SNAPSHOT_ONLY_PAYLOAD);
	expect_missing(root_fd, BRANCH_ONLY_NAME, "initial branch lookup");
	if (syncfs(root_fd))
		fail("initial syncfs: %s", strerror(errno));
	checkpoint_a_stat = file_stat_at(active_upper_fd, STATE_NAME,
					 "checkpoint A baseline");
	close_fd(root_fd, "initial merged root");
	pass("initial checkpoint contents prepared and synced");

	if (renameat(AT_FDCWD, argv[3], scratch_fd, "checkpoint-a"))
		fail("rename active upper to checkpoint-a: %s", strerror(errno));
	expect_renamed_directory(active_upper_fd, scratch_fd, "checkpoint-a",
				 "checkpoint A rename");
	pass("active upper renamed to immutable checkpoint A");

	generation_2 = create_branch(scratch_fd, "generation-2");
	lower_fds[0] = active_upper_fd;
	lower_fds[1] = active_lower_fd;
	init_request(&req, generation_2.upper_fd, generation_2.work_fd,
		     lower_fds, 2, 1);
	switch_view(argv[1], DELTAFS_IOC_CHECKPOINT, &req,
		    "checkpoint committed generation 1 -> 2");

	root_fd = open_dir(argv[1], O_RDONLY);
	expect_contents(root_fd, STATE_NAME, SNAPSHOT_PAYLOAD,
			"checkpoint view");
	expect_contents(root_fd, SNAPSHOT_ONLY_NAME, SNAPSHOT_ONLY_PAYLOAD,
			"checkpoint view");
	expect_contents(root_fd, BASE_NAME, BASE_PAYLOAD, "checkpoint view");
	expect_missing(generation_2.upper_fd, STATE_NAME,
		       "fresh generation-2 upper");
	write_file_at(root_fd, STATE_NAME, BRANCH_PAYLOAD);
	write_file_at(root_fd, BRANCH_ONLY_NAME, BRANCH_ONLY_PAYLOAD);
	if (syncfs(root_fd))
		fail("generation-2 syncfs: %s", strerror(errno));
	close_fd(root_fd, "generation-2 merged root");

	expect_contents(generation_2.upper_fd, STATE_NAME, BRANCH_PAYLOAD,
			"generation-2 physical upper");
	expect_contents(generation_2.upper_fd, BRANCH_ONLY_NAME,
			BRANCH_ONLY_PAYLOAD, "generation-2 physical upper");
	generation_2_stat = file_stat_at(generation_2.upper_fd, STATE_NAME,
					 "generation-2 baseline");
	expect_contents(active_upper_fd, STATE_NAME, SNAPSHOT_PAYLOAD,
			"frozen checkpoint A");
	expect_missing(active_upper_fd, BRANCH_ONLY_NAME, "frozen checkpoint A");
	expect_metadata_unchanged(active_upper_fd, STATE_NAME,
				  &checkpoint_a_stat, "frozen checkpoint A");
	expect_contents(active_lower_fd, BASE_NAME, BASE_PAYLOAD,
			"base lower after checkpoint");
	pass("generation-2 writes use the fresh upper and preserve checkpoint A");

	generation_3 = create_branch(scratch_fd, "generation-3");
	init_request(&req, generation_3.upper_fd, generation_3.work_fd,
		     lower_fds, 2, 2);
	switch_view(argv[1], DELTAFS_IOC_RESTORE, &req,
		    "historical restore committed generation 2 -> 3");

	root_fd = open_dir(argv[1], O_RDONLY);
	expect_contents(root_fd, STATE_NAME, SNAPSHOT_PAYLOAD,
			"restored checkpoint A");
	expect_contents(root_fd, SNAPSHOT_ONLY_NAME, SNAPSHOT_ONLY_PAYLOAD,
			"restored checkpoint A");
	expect_contents(root_fd, BASE_NAME, BASE_PAYLOAD,
			"restored checkpoint A");
	expect_missing(root_fd, BRANCH_ONLY_NAME, "restored checkpoint A");
	expect_missing(generation_3.upper_fd, STATE_NAME,
		       "fresh generation-3 upper");
	write_file_at(root_fd, STATE_NAME, RESTORE_PAYLOAD);
	write_file_at(root_fd, RESTORE_ONLY_NAME, RESTORE_ONLY_PAYLOAD);
	expect_missing(root_fd, FINAL_ONLY_NAME, "generation-3 negative lookup");
	if (syncfs(root_fd))
		fail("generation-3 syncfs: %s", strerror(errno));
	close_fd(root_fd, "generation-3 merged root");

	expect_contents(generation_3.upper_fd, STATE_NAME, RESTORE_PAYLOAD,
			"generation-3 physical upper");
	expect_contents(generation_3.upper_fd, RESTORE_ONLY_NAME,
			RESTORE_ONLY_PAYLOAD, "generation-3 physical upper");
	expect_contents(generation_2.upper_fd, STATE_NAME, BRANCH_PAYLOAD,
			"retired generation-2 upper");
	expect_contents(generation_2.upper_fd, BRANCH_ONLY_NAME,
			BRANCH_ONLY_PAYLOAD, "retired generation-2 upper");
	expect_metadata_unchanged(generation_2.upper_fd, STATE_NAME,
				  &generation_2_stat,
				  "retired generation-2 upper");
	expect_contents(active_upper_fd, STATE_NAME, SNAPSHOT_PAYLOAD,
			"checkpoint A after restore branch");
	expect_metadata_unchanged(active_upper_fd, STATE_NAME,
				  &checkpoint_a_stat,
				  "checkpoint A after restore branch");
	checkpoint_b_stat = file_stat_at(generation_3.upper_fd, STATE_NAME,
					 "checkpoint B baseline");
	pass("restore created an isolated generation-3 branch");

	if (renameat(generation_3.dir_fd, "upper", scratch_fd,
		     "checkpoint-b"))
		fail("rename generation-3 upper to checkpoint-b: %s",
		     strerror(errno));
	expect_renamed_directory(generation_3.upper_fd, scratch_fd,
				 "checkpoint-b", "checkpoint B rename");
	pass("generation-3 upper renamed to immutable checkpoint B");

	generation_4 = create_branch(scratch_fd, "generation-4");
	lower_fds[0] = generation_3.upper_fd;
	lower_fds[1] = active_upper_fd;
	lower_fds[2] = active_lower_fd;
	init_request(&req, generation_4.upper_fd, generation_4.work_fd,
		     lower_fds, 3, 3);
	switch_view(argv[1], DELTAFS_IOC_CHECKPOINT, &req,
		    "second checkpoint committed generation 3 -> 4");

	root_fd = open_dir(argv[1], O_RDONLY);
	expect_contents(root_fd, STATE_NAME, RESTORE_PAYLOAD,
			"checkpoint B view");
	expect_contents(root_fd, RESTORE_ONLY_NAME, RESTORE_ONLY_PAYLOAD,
			"checkpoint B view");
	expect_contents(root_fd, SNAPSHOT_ONLY_NAME, SNAPSHOT_ONLY_PAYLOAD,
			"checkpoint B view");
	expect_contents(root_fd, BASE_NAME, BASE_PAYLOAD, "checkpoint B view");
	expect_missing(root_fd, BRANCH_ONLY_NAME, "checkpoint B view");
	expect_missing(generation_4.upper_fd, STATE_NAME,
		       "fresh generation-4 upper");
	write_file_at(root_fd, STATE_NAME, FINAL_PAYLOAD);
	write_file_at(root_fd, FINAL_ONLY_NAME, FINAL_ONLY_PAYLOAD);
	if (syncfs(root_fd))
		fail("generation-4 syncfs: %s", strerror(errno));
	close_fd(root_fd, "generation-4 merged root");

	expect_contents(generation_4.upper_fd, STATE_NAME, FINAL_PAYLOAD,
			"generation-4 physical upper");
	expect_contents(generation_4.upper_fd, FINAL_ONLY_NAME,
			FINAL_ONLY_PAYLOAD, "generation-4 physical upper");
	expect_contents(generation_3.upper_fd, STATE_NAME, RESTORE_PAYLOAD,
			"frozen checkpoint B");
	expect_contents(generation_3.upper_fd, RESTORE_ONLY_NAME,
			RESTORE_ONLY_PAYLOAD, "frozen checkpoint B");
	expect_missing(generation_3.upper_fd, FINAL_ONLY_NAME,
		       "frozen checkpoint B");
	expect_metadata_unchanged(generation_3.upper_fd, STATE_NAME,
				  &checkpoint_b_stat, "frozen checkpoint B");
	expect_contents(generation_2.upper_fd, STATE_NAME, BRANCH_PAYLOAD,
			"retired generation-2 after checkpoint B");
	expect_metadata_unchanged(generation_2.upper_fd, STATE_NAME,
				  &generation_2_stat,
				  "retired generation-2 after checkpoint B");
	expect_contents(active_upper_fd, STATE_NAME, SNAPSHOT_PAYLOAD,
			"checkpoint A after checkpoint B");
	expect_metadata_unchanged(active_upper_fd, STATE_NAME,
				  &checkpoint_a_stat,
				  "checkpoint A after checkpoint B");
	expect_contents(active_lower_fd, BASE_NAME, BASE_PAYLOAD,
			"base lower after all commits");
	expect_metadata_unchanged(active_lower_fd, BASE_NAME, &base_stat,
				  "base lower after all commits");
	pass("generation-4 writes preserve all frozen and retired views");

	root_fd = open_dir(argv[1], O_RDONLY);
	req.expected_generation = 3;
	req.upper_fd = -1;
	expect_ioctl_errno(root_fd, DELTAFS_IOC_RESTORE, &req, ESTALE,
			   "old generation after three commits");
	req.expected_generation = 4;
	expect_ioctl_errno(root_fd, DELTAFS_IOC_RESTORE, &req, EBADF,
			   "generation 4 reaches fd validation");
	close_fd(root_fd, "generation probe root");
	pass("global generation was published exactly as 4");

	close_branch(&generation_4, "generation-4 branch");
	close_branch(&generation_3, "generation-3 branch");
	close_branch(&generation_2, "generation-2 branch");
	close_fd(active_lower_fd, "active lower");
	close_fd(active_upper_fd, "checkpoint A");
	close_fd(scratch_fd, "scratch");
	puts("All P5 checkpoint and multi-commit checks passed");
	return EXIT_SUCCESS;
}
