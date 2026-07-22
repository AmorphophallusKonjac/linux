// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS P3 generation=1 cache protocol regression test.
 *
 * The caller provides an already-mounted writable OverlayFS root and its top
 * lower directory.  This test never changes the DeltaFS generation.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_ITERATIONS 128
#define MAX_ITERATIONS 100000
#define FIXTURE_NAME "lower-positive.txt"
#define ORIGINAL_PAYLOAD "deltafs-p3-lower-original\n"
#define APPEND_PAYLOAD "deltafs-p3-upper-append\n"

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}

static void write_all(int fd, const char *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = write(fd, buf + done, len - done);

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

static size_t read_file_at(int dirfd, const char *path, char *buf, size_t size)
{
	size_t done = 0;
	int fd;

	fd = openat(dirfd, path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		fail("open %s: %s", path, strerror(errno));

	while (done < size) {
		ssize_t ret = read(fd, buf + done, size - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fail("read %s: %s", path, strerror(errno));
		}
		if (!ret)
			break;
		done += ret;
	}
	if (close(fd))
		fail("close %s: %s", path, strerror(errno));
	return done;
}

static void expect_contents(int dirfd, const char *path, const char *expected,
			    const char *stage)
{
	char buf[512];
	size_t expected_len = strlen(expected);
	size_t len = read_file_at(dirfd, path, buf, sizeof(buf));

	if (len != expected_len || memcmp(buf, expected, expected_len))
		fail("%s: unexpected contents for %s", stage, path);
}

static void expect_missing(int dirfd, const char *path, const char *stage)
{
	struct stat st;

	errno = 0;
	if (fstatat(dirfd, path, &st, AT_SYMLINK_NOFOLLOW) == -1 &&
	    errno == ENOENT)
		return;
	if (errno)
		fail("%s: stat %s returned %s instead of ENOENT", stage, path,
		     strerror(errno));
	fail("%s: %s unexpectedly exists", stage, path);
}

static void make_name(char *buf, size_t size, const char *kind)
{
	int len = snprintf(buf, size, ".deltafs-p3-%s-%ld", kind,
			   (long)getpid());

	if (len < 0 || (size_t)len >= size)
		fail("could not construct %s test name", kind);
}

static void create_file_at(int dirfd, const char *path, const char *payload)
{
	int fd = openat(dirfd, path,
			O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

	if (fd < 0)
		fail("create %s: %s", path, strerror(errno));
	write_all(fd, payload, strlen(payload));
	if (close(fd))
		fail("close %s: %s", path, strerror(errno));
}

static bool dir_has_name(DIR *dir, const char *name)
{
	struct dirent *entry;

	rewinddir(dir);
	errno = 0;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, name))
			return true;
	}
	if (errno)
		fail("readdir while looking for %s: %s", name, strerror(errno));
	return false;
}

static void test_positive_cache(int merged_fd, int iterations)
{
	struct stat first, current;
	int i;

	if (fstatat(merged_fd, FIXTURE_NAME, &first, 0))
		fail("initial stat %s: %s", FIXTURE_NAME, strerror(errno));
	if (!S_ISREG(first.st_mode))
		fail("%s is not a regular file", FIXTURE_NAME);

	for (i = 0; i < iterations; i++) {
		if (fstatat(merged_fd, FIXTURE_NAME, &current, 0))
			fail("positive stat iteration %d: %s", i, strerror(errno));
		if (current.st_dev != first.st_dev ||
		    current.st_ino != first.st_ino ||
		    current.st_size != first.st_size)
			fail("positive inode identity changed at iteration %d", i);
		expect_contents(merged_fd, FIXTURE_NAME, ORIGINAL_PAYLOAD,
				"positive cache");
	}
	printf("PASS: positive dentry/inode cache (%d iterations)\n", iterations);
}

static void test_negative_cache(int merged_fd, int iterations)
{
	static const char payload[] = "created-after-negative-lookup\n";
	char name[128];
	int i;

	make_name(name, sizeof(name), "negative");
	unlinkat(merged_fd, name, 0);

	for (i = 0; i < iterations; i++)
		expect_missing(merged_fd, name, "negative lookup");

	create_file_at(merged_fd, name, payload);
	expect_contents(merged_fd, name, payload, "create after negative lookup");
	if (unlinkat(merged_fd, name, 0))
		fail("unlink %s: %s", name, strerror(errno));

	for (i = 0; i < iterations; i++)
		expect_missing(merged_fd, name, "post-unlink lookup");

	printf("PASS: negative lookup/create/unlink (%d iterations)\n",
	       iterations);
}

static void test_root_readdir_version(int merged_fd)
{
	static const char payload[] = "root-version\n";
	char old_name[128], new_name[128];
	DIR *dir;
	int fd;

	make_name(old_name, sizeof(old_name), "root-old");
	make_name(new_name, sizeof(new_name), "root-new");
	unlinkat(merged_fd, old_name, 0);
	unlinkat(merged_fd, new_name, 0);

	fd = dup(merged_fd);
	if (fd < 0)
		fail("dup merged root: %s", strerror(errno));
	dir = fdopendir(fd);
	if (!dir) {
		close(fd);
		fail("fdopendir merged root: %s", strerror(errno));
	}

	if (dir_has_name(dir, old_name))
		fail("root readdir fixture %s already exists", old_name);
	create_file_at(merged_fd, old_name, payload);
	if (!dir_has_name(dir, old_name))
		fail("root readdir did not observe create");

	if (renameat(merged_fd, old_name, merged_fd, new_name))
		fail("rename root version fixture: %s", strerror(errno));
	if (dir_has_name(dir, old_name))
		fail("root readdir retained old name after rename");
	if (!dir_has_name(dir, new_name))
		fail("root readdir did not observe renamed entry");

	if (unlinkat(merged_fd, new_name, 0))
		fail("unlink root version fixture: %s", strerror(errno));
	if (dir_has_name(dir, new_name))
		fail("root readdir retained entry after unlink");
	if (closedir(dir))
		fail("closedir merged root: %s", strerror(errno));

	puts("PASS: root readdir version invalidation");
}

static void test_copy_up_and_alias(int merged_fd, int lower_fd)
{
	static const char merged_payload[] = ORIGINAL_PAYLOAD APPEND_PAYLOAD;
	char link_name[128];
	struct stat source, alias;
	int fd;

	expect_contents(lower_fd, FIXTURE_NAME, ORIGINAL_PAYLOAD,
			"lower before copy-up");

	fd = openat(merged_fd, FIXTURE_NAME,
		    O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd < 0)
		fail("open %s for copy-up: %s", FIXTURE_NAME, strerror(errno));
	write_all(fd, APPEND_PAYLOAD, strlen(APPEND_PAYLOAD));
	if (close(fd))
		fail("close copied-up %s: %s", FIXTURE_NAME, strerror(errno));

	expect_contents(merged_fd, FIXTURE_NAME, merged_payload,
			"merged after copy-up");
	expect_contents(lower_fd, FIXTURE_NAME, ORIGINAL_PAYLOAD,
			"lower after copy-up");

	make_name(link_name, sizeof(link_name), "alias");
	unlinkat(merged_fd, link_name, 0);
	if (linkat(merged_fd, FIXTURE_NAME, merged_fd, link_name, 0))
		fail("create overlay hardlink: %s", strerror(errno));
	if (fstatat(merged_fd, FIXTURE_NAME, &source, 0) ||
	    fstatat(merged_fd, link_name, &alias, 0))
		fail("stat overlay hardlink pair: %s", strerror(errno));
	if (source.st_dev != alias.st_dev || source.st_ino != alias.st_ino)
		fail("overlay hardlink aliases have different inode identities");
	if (unlinkat(merged_fd, link_name, 0))
		fail("unlink overlay hardlink: %s", strerror(errno));

	puts("PASS: copy-up preserves lower and current-generation aliasing");
}

static int parse_iterations(const char *arg)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(arg, &end, 10);
	if (errno || *end || value < 1 || value > MAX_ITERATIONS)
		return -1;
	return value;
}

int main(int argc, char **argv)
{
	int iterations = DEFAULT_ITERATIONS;
	int merged_fd, lower_fd;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "Usage: %s MERGED TOP_LOWER [ITERATIONS]\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 4) {
		iterations = parse_iterations(argv[3]);
		if (iterations < 0) {
			fprintf(stderr, "ITERATIONS must be in [1, %d]\n",
				MAX_ITERATIONS);
			return EXIT_FAILURE;
		}
	}

	merged_fd = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (merged_fd < 0)
		fail("open merged directory %s: %s", argv[1], strerror(errno));
	lower_fd = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (lower_fd < 0)
		fail("open lower directory %s: %s", argv[2], strerror(errno));

	test_positive_cache(merged_fd, iterations);
	test_negative_cache(merged_fd, iterations);
	test_root_readdir_version(merged_fd);
	test_copy_up_and_alias(merged_fd, lower_fd);

	if (close(lower_fd))
		fail("close lower directory: %s", strerror(errno));
	if (close(merged_fd))
		fail("close merged directory: %s", strerror(errno));

	puts("All P3 generation=1 cache tests passed");
	return EXIT_SUCCESS;
}

