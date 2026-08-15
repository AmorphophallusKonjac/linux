// SPDX-License-Identifier: GPL-2.0
/* Native DeltaFS v2 checkpoint/restore smoke helper. */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/deltafs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static const char *program_name;

static void usage(void)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s checkpoint MERGED GENERATION UPPER WORK\n"
		"  %s restore MERGED GENERATION KEEP_BOTTOM UPPER WORK [LOWER ...]\n"
		"  %s probe-generation MERGED GENERATION\n"
		"  %s negative MERGED GENERATION UPPER WORK\n",
		program_name, program_name, program_name, program_name);
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || !text[0] || *end || !parsed || parsed == ULLONG_MAX) {
		fprintf(stderr, "%s: invalid generation '%s'\n", program_name, text);
		return -1;
	}
	*value = (uint64_t)parsed;
	return 0;
}

static int parse_uint(const char *text, unsigned int *value)
{
	char *end;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(text, &end, 10);
	if (errno || !text[0] || *end || parsed > UINT_MAX) {
		fprintf(stderr, "%s: invalid unsigned integer '%s'\n",
			program_name, text);
		return -1;
	}
	*value = (unsigned int)parsed;
	return 0;
}

static int open_dir(const char *path, int flags)
{
	int fd = open(path, flags | O_DIRECTORY | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr, "%s: open %s: %s\n", program_name, path,
			strerror(errno));
	return fd;
}

static int run_checkpoint(int argc, char **argv)
{
	struct deltafs_ioc_checkpoint_v2 request = {
		.size = sizeof(request),
		.version = DELTAFS_ABI_VERSION,
	};
	uint64_t generation;
	int root_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int ret = EXIT_FAILURE;

	if (argc != 6 || parse_u64(argv[3], &generation)) {
		usage();
		return EXIT_FAILURE;
	}
	root_fd = open_dir(argv[2], O_RDONLY);
	upper_fd = open_dir(argv[4], O_PATH);
	work_fd = open_dir(argv[5], O_PATH);
	if (root_fd < 0 || upper_fd < 0 || work_fd < 0)
		goto out;

	request.expected_generation = generation;
	request.upper_fd = upper_fd;
	request.work_fd = work_fd;
	if (ioctl(root_fd, DELTAFS_IOC_CHECKPOINT, &request)) {
		fprintf(stderr, "%s: checkpoint ioctl: %s\n", program_name,
			strerror(errno));
		goto out;
	}
	printf("checkpoint: generation %llu -> %llu\n",
	       (unsigned long long)generation,
	       (unsigned long long)(generation + 1));
	ret = EXIT_SUCCESS;

out:
	if (work_fd >= 0)
		close(work_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (root_fd >= 0)
		close(root_fd);
	return ret;
}

static void init_restore(struct deltafs_ioc_restore_v2 *request,
			 uint64_t generation, unsigned int keep_bottom)
{
	unsigned int i;

	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->keep_bottom = keep_bottom;
	for (i = 0; i < DELTAFS_V2_MAX_RESTORE_FDS; i++)
		request->fds[i] = -1;
}

static int run_restore(int argc, char **argv)
{
	struct deltafs_ioc_restore_v2 request;
	unsigned int keep_bottom;
	unsigned int nr_lower;
	uint64_t generation;
	int opened[DELTAFS_V2_MAX_RESTORE_FDS];
	unsigned int nr_opened = 0;
	unsigned int i;
	int root_fd = -1;
	int ret = EXIT_FAILURE;

	if (argc < 7 || parse_u64(argv[3], &generation) ||
	    parse_uint(argv[4], &keep_bottom)) {
		usage();
		return EXIT_FAILURE;
	}
	nr_lower = (unsigned int)argc - 7;
	if (nr_lower > DELTAFS_V2_MAX_LOWERS) {
		fprintf(stderr, "%s: too many lower prefix paths\n", program_name);
		return EXIT_FAILURE;
	}

	root_fd = open_dir(argv[2], O_RDONLY);
	if (root_fd < 0)
		goto out;
	init_restore(&request, generation, keep_bottom);
	for (i = 0; i < nr_lower + DELTAFS_V2_RESTORE_LOWER_BASE; i++) {
		const char *path = i == DELTAFS_V2_RESTORE_UPPER_FD ? argv[5] :
			i == DELTAFS_V2_RESTORE_WORK_FD ? argv[6] :
			argv[7 + i - DELTAFS_V2_RESTORE_LOWER_BASE];
		int fd = open_dir(path, O_PATH);

		if (fd < 0)
			goto out;
		opened[nr_opened++] = fd;
		request.fds[i] = fd;
	}
	request.nr_fds = nr_opened;
	if (ioctl(root_fd, DELTAFS_IOC_RESTORE, &request)) {
		fprintf(stderr, "%s: restore ioctl: %s\n", program_name,
			strerror(errno));
		goto out;
	}
	printf("restore: generation %llu -> %llu (keep_bottom=%u, prefix=%u)\n",
	       (unsigned long long)generation,
	       (unsigned long long)(generation + 1), keep_bottom, nr_lower);
	ret = EXIT_SUCCESS;

out:
	while (nr_opened)
		close(opened[--nr_opened]);
	if (root_fd >= 0)
		close(root_fd);
	return ret;
}

static bool expect_errno(int fd, unsigned long command, const void *request,
			 int expected)
{
	errno = 0;
	return ioctl(fd, command, request) == -1 && errno == expected;
}

static int require_errno(const char *stage, int fd, unsigned long command,
			 const void *request, int expected)
{
	if (expect_errno(fd, command, request, expected)) {
		printf("PASS: %s: %s\n", stage, strerror(expected));
		return 0;
	}
	fprintf(stderr, "%s: %s: expected %s, got %s\n", program_name, stage,
		strerror(expected), errno ? strerror(errno) : "success");
	return -1;
}

static int probe_generation_fd(int root_fd, uint64_t generation)
{
	struct deltafs_ioc_restore_v2 request;

	init_restore(&request, generation + 1, 1);
	request.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	if (!expect_errno(root_fd, DELTAFS_IOC_RESTORE, &request, ESTALE)) {
		fprintf(stderr, "%s: stale generation probe returned %s\n",
			program_name, errno ? strerror(errno) : "success");
		return -1;
	}
	request.expected_generation = generation;
	if (!expect_errno(root_fd, DELTAFS_IOC_RESTORE, &request, EBADF)) {
		fprintf(stderr, "%s: current generation probe returned %s\n",
			program_name, errno ? strerror(errno) : "success");
		return -1;
	}
	return 0;
}

static int run_probe(int argc, char **argv)
{
	uint64_t generation;
	int root_fd;

	if (argc != 4 || parse_u64(argv[3], &generation)) {
		usage();
		return EXIT_FAILURE;
	}
	root_fd = open_dir(argv[2], O_RDONLY);
	if (root_fd < 0)
		return EXIT_FAILURE;

	if (probe_generation_fd(root_fd, generation)) {
		close(root_fd);
		return EXIT_FAILURE;
	}
	close(root_fd);
	printf("generation %llu is current\n", (unsigned long long)generation);
	return EXIT_SUCCESS;
}

static void init_checkpoint(struct deltafs_ioc_checkpoint_v2 *request,
			    uint64_t generation, int upper_fd, int work_fd)
{
	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->upper_fd = upper_fd;
	request->work_fd = work_fd;
}

static int run_negative(int argc, char **argv)
{
	struct deltafs_ioc_checkpoint_v2 checkpoint;
	struct deltafs_ioc_restore_v2 restore;
	uint64_t generation;
	unsigned long old_checkpoint;
	int root_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int ret = EXIT_FAILURE;

	if (argc != 6 || parse_u64(argv[3], &generation)) {
		usage();
		return EXIT_FAILURE;
	}
	root_fd = open_dir(argv[2], O_RDONLY);
	upper_fd = open_dir(argv[4], O_PATH);
	work_fd = open_dir(argv[5], O_PATH);
	if (root_fd < 0 || upper_fd < 0 || work_fd < 0)
		goto out;

	init_checkpoint(&checkpoint, generation, upper_fd, work_fd);
	checkpoint.size--;
	if (require_errno("checkpoint size", root_fd, DELTAFS_IOC_CHECKPOINT,
			  &checkpoint, EINVAL))
		goto out;
	init_checkpoint(&checkpoint, generation, upper_fd, work_fd);
	checkpoint.version = 1;
	if (require_errno("checkpoint version", root_fd, DELTAFS_IOC_CHECKPOINT,
			  &checkpoint, EINVAL))
		goto out;
	init_checkpoint(&checkpoint, generation, upper_fd, work_fd);
	checkpoint.flags = 1;
	if (require_errno("checkpoint flags", root_fd, DELTAFS_IOC_CHECKPOINT,
			  &checkpoint, EINVAL))
		goto out;
	init_checkpoint(&checkpoint, generation, upper_fd, work_fd);
	checkpoint.reserved[0] = 1;
	if (require_errno("checkpoint reserved", root_fd, DELTAFS_IOC_CHECKPOINT,
			  &checkpoint, EINVAL) ||
	    require_errno("checkpoint EFAULT", root_fd, DELTAFS_IOC_CHECKPOINT,
			  (const void *)1, EFAULT))
		goto out;
	init_checkpoint(&checkpoint, generation + 1, upper_fd, work_fd);
	if (require_errno("checkpoint stale generation", root_fd,
			  DELTAFS_IOC_CHECKPOINT, &checkpoint, ESTALE))
		goto out;
	init_checkpoint(&checkpoint, generation, -1, work_fd);
	if (require_errno("checkpoint bad fd", root_fd, DELTAFS_IOC_CHECKPOINT,
			  &checkpoint, EBADF))
		goto out;

	init_restore(&restore, generation, 1);
	old_checkpoint = _IOC(_IOC_WRITE, DELTAFS_IOC_MAGIC, 0x01, 584);
	if (require_errno("v1 checkpoint command", root_fd, old_checkpoint,
			  &restore, ENOTTY))
		goto out;

	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.size--;
	if (require_errno("restore size", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.version = 1;
	if (require_errno("restore version", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.flags = 1;
	if (require_errno("restore flags", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.reserved[0] = 1;
	if (require_errno("restore reserved", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL) ||
	    require_errno("restore EFAULT", root_fd, DELTAFS_IOC_RESTORE,
			  (const void *)1, EFAULT))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE - 1;
	if (require_errno("restore short fd array", root_fd,
			  DELTAFS_IOC_RESTORE, &restore, EINVAL))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_MAX_RESTORE_FDS + 1;
	if (require_errno("restore oversized fd array", root_fd,
			  DELTAFS_IOC_RESTORE, &restore, E2BIG))
		goto out;
	init_restore(&restore, generation, 1);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.fds[DELTAFS_V2_RESTORE_LOWER_BASE] = upper_fd;
	if (require_errno("restore trailing fd", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL))
		goto out;
	init_restore(&restore, generation, UINT_MAX);
	restore.nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE;
	restore.fds[DELTAFS_V2_RESTORE_UPPER_FD] = upper_fd;
	restore.fds[DELTAFS_V2_RESTORE_WORK_FD] = work_fd;
	if (require_errno("restore keep_bottom", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL))
		goto out;
	restore.keep_bottom = 0;
	if (require_errno("restore empty target", root_fd, DELTAFS_IOC_RESTORE,
			  &restore, EINVAL) ||
	    probe_generation_fd(root_fd, generation))
		goto out;

	puts("DeltaFS v2 negative ioctl checks passed");
	ret = EXIT_SUCCESS;

out:
	if (work_fd >= 0)
		close(work_fd);
	if (upper_fd >= 0)
		close(upper_fd);
	if (root_fd >= 0)
		close(root_fd);
	return ret;
}

int main(int argc, char **argv)
{
	program_name = argv[0];
	if (argc < 2) {
		usage();
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "checkpoint"))
		return run_checkpoint(argc, argv);
	if (!strcmp(argv[1], "restore"))
		return run_restore(argc, argv);
	if (!strcmp(argv[1], "probe-generation"))
		return run_probe(argc, argv);
	if (!strcmp(argv[1], "negative"))
		return run_negative(argc, argv);
	usage();
	return EXIT_FAILURE;
}
