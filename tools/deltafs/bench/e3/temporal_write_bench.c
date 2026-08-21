// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "bench_common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

struct measurement {
	uint64_t open_ns;
	uint64_t pwrite_only_ns;
	uint64_t fsync_ns;
	uint64_t close_ns;
	uint64_t edit_e2e_ns;
	int cpu_before;
	int cpu_after;
	long major_faults;
	int error_number;
	const char *status;
	const char *reason;
};

static uint64_t elapsed_ns(const struct timespec *before,
			   const struct timespec *after, bool *backwards)
{
	if (after->tv_sec < before->tv_sec ||
	    (after->tv_sec == before->tv_sec && after->tv_nsec < before->tv_nsec)) {
		*backwards = true;
		return 0;
	}
	return (uint64_t)(after->tv_sec - before->tv_sec) * 1000000000ULL +
	       (uint64_t)(after->tv_nsec - before->tv_nsec);
}

static int raw_clock(struct timespec *value)
{
	return clock_gettime(CLOCK_MONOTONIC_RAW, value);
}

static int write_result(const char *path, const struct measurement *measurement)
{
	char buffer[2048];
	int length;

	length = snprintf(buffer, sizeof(buffer),
		"{\"schema\":1,\"open_ns\":%" PRIu64
		",\"pwrite_only_ns\":%" PRIu64
		",\"fsync_ns\":%" PRIu64
		",\"close_ns\":%" PRIu64
		",\"edit_e2e_ns\":%" PRIu64
		",\"cpu_before\":%d,\"cpu_after\":%d"
		",\"major_faults_delta\":%ld,\"errno\":%d"
		",\"status\":\"%s\",\"invalid_reason\":%s%s%s}\n",
		measurement->open_ns, measurement->pwrite_only_ns,
		measurement->fsync_ns, measurement->close_ns,
		measurement->edit_e2e_ns, measurement->cpu_before,
		measurement->cpu_after, measurement->major_faults,
		measurement->error_number, measurement->status,
		measurement->reason ? "\"" : "null",
		measurement->reason ? measurement->reason : "",
		measurement->reason ? "\"" : "");
	if (length < 0 || (size_t)length >= sizeof(buffer)) {
		errno = EOVERFLOW;
		return -1;
	}
	return bench_atomic_write(path, buffer, (size_t)length);
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	if (!text || !text[0] || text[0] == '-') {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	parsed = strtoull(text, &end, 10);
	if (errno || *end) {
		if (!errno)
			errno = EINVAL;
		return -1;
	}
	*value = (uint64_t)parsed;
	return 0;
}

static void store_le64(unsigned char output[8], uint64_t value)
{
	unsigned int index;

	for (index = 0; index < 8; index++)
		output[index] = (unsigned char)(value >> (index * 8));
}

static int fill_e3_bytes(uint64_t seed, unsigned char *output, size_t length)
{
	static const unsigned char prefix[] = "deltafs-e3-v1\0";
	unsigned char input[sizeof(prefix) - 1 + 8];
	char digest[BENCH_SHA256_HEX_SIZE];

	memcpy(input, prefix, sizeof(prefix) - 1);
	store_le64(input + sizeof(prefix) - 1, seed);
	if (bench_sha256_buffer(input, sizeof(input), digest))
		return -1;
	while (length) {
		size_t count = length < 64 ? length : 64;

		memcpy(output, digest, count);
		output += count;
		length -= count;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *target;
	const char *result_path;
	uint64_t offset;
	uint64_t payload_seed;
	uint64_t file_size;
	uint64_t write_bytes;
	unsigned char *payload = NULL;
	struct measurement measurement = {
		.cpu_before = -1, .cpu_after = -1, .status = "failed",
	};
	struct rusage usage_before;
	struct rusage usage_after;
	struct timespec before;
	struct timespec after;
	bool backwards = false;
	int fd = -1;
	int saved_errno = 0;
	ssize_t written;

	if (argc != 9 || strcmp(argv[1], "reopen")) {
		fprintf(stderr, "Usage: %s reopen MERGED TARGET OFFSET SEED SIZE BYTES OUT\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	(void)argv[2];
	target = argv[3];
	result_path = argv[argc - 1];
	if (parse_u64(argv[4], &offset) || parse_u64(argv[5], &payload_seed) ||
	    parse_u64(argv[6], &file_size) || parse_u64(argv[7], &write_bytes) ||
	    !file_size || !write_bytes || write_bytes > SIZE_MAX ||
	    offset > file_size || write_bytes > file_size - offset) {
		fprintf(stderr, "%s: invalid write geometry\n", argv[0]);
		return EXIT_FAILURE;
	}
	payload = malloc((size_t)write_bytes);
	if (!payload || fill_e3_bytes(payload_seed, payload, (size_t)write_bytes)) {
		free(payload);
		return EXIT_FAILURE;
	}
	if (getrusage(RUSAGE_SELF, &usage_before) ||
	    (measurement.cpu_before = sched_getcpu()) < 0)
		goto out;
	if (raw_clock(&before))
		goto out;
	fd = open(target, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		saved_errno = errno;
		goto out;
	}
	if (raw_clock(&after))
		goto out;
	measurement.open_ns = elapsed_ns(&before, &after, &backwards);
	if (fd < 0)
		goto out;
	if (raw_clock(&before))
		goto out;
	written = pwrite(fd, payload, (size_t)write_bytes, (off_t)offset);
	if (raw_clock(&after))
		goto out;
	measurement.pwrite_only_ns = elapsed_ns(&before, &after, &backwards);
	if (written != (ssize_t)write_bytes) {
		saved_errno = written < 0 ? errno : EIO;
		goto out;
	}
	if (raw_clock(&before))
		goto out;
	if (fsync(fd)) {
		saved_errno = errno;
		goto out;
	}
	if (raw_clock(&after))
		goto out;
	measurement.fsync_ns = elapsed_ns(&before, &after, &backwards);
	if (raw_clock(&before))
		goto out;
	if (close(fd)) {
		saved_errno = errno;
		fd = -1;
		goto out;
	}
	fd = -1;
	if (raw_clock(&after))
		goto out;
	measurement.close_ns = elapsed_ns(&before, &after, &backwards);
	measurement.edit_e2e_ns = measurement.open_ns + measurement.pwrite_only_ns +
				   measurement.fsync_ns + measurement.close_ns;
	if (getrusage(RUSAGE_SELF, &usage_after) ||
	    (measurement.cpu_after = sched_getcpu()) < 0)
		goto out;
	measurement.major_faults = usage_after.ru_majflt - usage_before.ru_majflt;
	measurement.error_number = 0;
	if (backwards) {
		measurement.status = "invalid";
		measurement.reason = "clock_backwards";
	} else if (measurement.cpu_before != measurement.cpu_after) {
		measurement.status = "invalid";
		measurement.reason = "cpu_migration";
	} else if (measurement.major_faults) {
		measurement.status = "invalid";
		measurement.reason = "major_fault";
	} else {
		measurement.status = "ok";
	}
	out:
	if (saved_errno) {
		measurement.error_number = saved_errno;
		measurement.status = "failed";
		measurement.reason = "syscall_failed";
	}
	if (fd >= 0)
		close(fd);
	free(payload);
	if (write_result(result_path, &measurement))
		return EXIT_FAILURE;
	return !strcmp(measurement.status, "ok") ? EXIT_SUCCESS : EXIT_FAILURE;
}
