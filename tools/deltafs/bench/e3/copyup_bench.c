// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "bench_common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct options {
	bool control;
	const char *merged;
	const char *target;
	const char *upper;
	const char *lower;
	const char *device_stat;
	const char *expected_before;
	const char *expected_after;
	const char *cache_mode;
	const char *fiemap_out;
	const char *out;
	uint64_t file_size;
	uint64_t offset;
	uint64_t write_bytes;
	uint64_t payload_seed;
};

struct result {
	const char *kind;
	const char *status;
	const char *reason;
	int error_number;
	bool settle_timeout;
	uint64_t sectors_before;
	uint64_t sectors_after;
	uint64_t physical_io_bytes;
	char pre_sha256[BENCH_SHA256_HEX_SIZE];
	char post_sha256[BENCH_SHA256_HEX_SIZE];
	char upper_sha256[BENCH_SHA256_HEX_SIZE];
	char lower_sha256[BENCH_SHA256_HEX_SIZE];
	struct bench_fiemap_summary fiemap;
};

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s edit --merged ROOT --target PATH --upper PATH --lower PATH "
		"--device-stat PATH --file-size N --offset N --write-bytes N "
		"--payload-seed N --expected-before HEX --expected-after HEX "
		"--cache-mode warm|cold --fiemap-out PATH --out PATH\n"
		"  %s control --merged PATH --device-stat PATH --out PATH\n",
		program, program);
}

static int parse_u64(const char *text, uint64_t *value)
{
	char *end;
	unsigned long long number;

	if (!text || !*text || *text == '+' || *text == '-') {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	number = strtoull(text, &end, 10);
	if (errno || *end) {
		if (!errno)
			errno = EINVAL;
		return -1;
	}
	*value = (uint64_t)number;
	return 0;
}

static int set_option(struct options *options, const char *name,
		      const char *value)
{
	const char **string = NULL;

	if (!strcmp(name, "--merged"))
		string = &options->merged;
	else if (!strcmp(name, "--target"))
		string = &options->target;
	else if (!strcmp(name, "--upper"))
		string = &options->upper;
	else if (!strcmp(name, "--lower"))
		string = &options->lower;
	else if (!strcmp(name, "--device-stat"))
		string = &options->device_stat;
	else if (!strcmp(name, "--expected-before"))
		string = &options->expected_before;
	else if (!strcmp(name, "--expected-after"))
		string = &options->expected_after;
	else if (!strcmp(name, "--cache-mode"))
		string = &options->cache_mode;
	else if (!strcmp(name, "--fiemap-out"))
		string = &options->fiemap_out;
	else if (!strcmp(name, "--out"))
		string = &options->out;
	else if (!strcmp(name, "--file-size"))
		return parse_u64(value, &options->file_size);
	else if (!strcmp(name, "--offset"))
		return parse_u64(value, &options->offset);
	else if (!strcmp(name, "--write-bytes"))
		return parse_u64(value, &options->write_bytes);
	else if (!strcmp(name, "--payload-seed"))
		return parse_u64(value, &options->payload_seed);
	else {
		errno = EINVAL;
		return -1;
	}
	if (*string) {
		errno = EINVAL;
		return -1;
	}
	*string = value;
	return 0;
}

static bool valid_hex_hash(const char *value)
{
	size_t i;

	if (!value || strlen(value) != 64)
		return false;
	for (i = 0; i < 64; i++)
		if (!((value[i] >= '0' && value[i] <= '9') ||
		      (value[i] >= 'a' && value[i] <= 'f')))
			return false;
	return true;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	int i;

	memset(options, 0, sizeof(*options));
	if (argc < 2 || (strcmp(argv[1], "edit") &&
			 strcmp(argv[1], "control"))) {
		errno = EINVAL;
		return -1;
	}
	options->control = !strcmp(argv[1], "control");
	for (i = 2; i < argc; i += 2) {
		if (i + 1 == argc || set_option(options, argv[i], argv[i + 1]))
			return -1;
	}
	if (!options->merged || !options->device_stat || !options->out ||
	    options->merged[0] != '/' || options->device_stat[0] != '/' ||
	    options->out[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	if (options->control) {
		if (options->target || options->upper || options->lower ||
		    options->expected_before ||
		    options->expected_after || options->cache_mode ||
		    options->fiemap_out || options->file_size || options->offset ||
		    options->write_bytes || options->payload_seed) {
			errno = EINVAL;
			return -1;
		}
		return 0;
	}
	if (!options->target || !options->upper || !options->lower ||
	    !options->expected_before ||
	    !options->expected_after || !options->cache_mode ||
	    !options->fiemap_out || options->target[0] != '/' ||
	    options->upper[0] != '/' ||
	    options->lower[0] != '/' || options->fiemap_out[0] != '/' ||
	    !options->file_size || !options->write_bytes ||
	    options->offset > options->file_size ||
	    options->write_bytes > options->file_size - options->offset ||
	    (strcmp(options->cache_mode, "warm") &&
	     strcmp(options->cache_mode, "cold")) ||
	    !valid_hex_hash(options->expected_before) ||
	    !valid_hex_hash(options->expected_after)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int syncfs_path(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	int ret;
	int saved_errno;

	if (fd < 0)
		return -1;
	ret = syncfs(fd);
	saved_errno = errno;
	if (close(fd) && !ret) {
		ret = -1;
		saved_errno = errno;
	}
	errno = saved_errno;
	return ret;
}

static int advise_cold(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	int ret;

	if (fd < 0)
		return -1;
	ret = posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	if (close(fd) && !ret)
		ret = errno;
	if (ret) {
		errno = ret;
		return -1;
	}
	return 0;
}

static int verify_input_files(const struct options *options,
			      struct result *result)
{
	struct stat lower_stat;
	struct stat merged_stat;
	struct stat upper_stat;

	if (lstat(options->upper, &upper_stat) == 0) {
		errno = EEXIST;
		return -1;
	}
	if (errno != ENOENT)
		return -1;
	if (stat(options->lower, &lower_stat) || stat(options->target, &merged_stat))
		return -1;
	if (!S_ISREG(lower_stat.st_mode) || !S_ISREG(merged_stat.st_mode) ||
	    lower_stat.st_size < 0 || merged_stat.st_size < 0 ||
	    (uint64_t)lower_stat.st_size != options->file_size ||
	    (uint64_t)merged_stat.st_size != options->file_size) {
		errno = EINVAL;
		return -1;
	}
	if (bench_sha256_path(options->lower, result->lower_sha256) ||
	    bench_sha256_path(options->target, result->pre_sha256))
		return -1;
	if (strcmp(result->lower_sha256, options->expected_before) ||
	    strcmp(result->pre_sha256, options->expected_before)) {
		errno = EBADMSG;
		return -1;
	}
	return 0;
}

static int write_payload(const struct options *options)
{
	unsigned char *payload;
	ssize_t written;
	int fd;
	int saved_errno;

	if (options->write_bytes > SIZE_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	payload = malloc((size_t)options->write_bytes);
	if (!payload)
		return -1;
	if (bench_fill_bytes(options->payload_seed, payload,
			     (size_t)options->write_bytes)) {
		free(payload);
		return -1;
	}
	fd = open(options->target, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		free(payload);
		return -1;
	}
	do {
		written = pwrite(fd, payload, (size_t)options->write_bytes,
				 (off_t)options->offset);
	} while (written < 0 && errno == EINTR);
	free(payload);
	if (written < 0 || (uint64_t)written != options->write_bytes || fsync(fd)) {
		if (written >= 0 && (uint64_t)written != options->write_bytes)
			errno = EIO;
		saved_errno = errno;
		close(fd);
		errno = saved_errno;
		return -1;
	}
	return close(fd);
}

static int compute_physical_io(struct result *result)
{
	uint64_t difference;

	if (result->sectors_after < result->sectors_before) {
		errno = ERANGE;
		return -1;
	}
	difference = result->sectors_after - result->sectors_before;
	if (difference > UINT64_MAX / 512) {
		errno = EOVERFLOW;
		return -1;
	}
	result->physical_io_bytes = difference * 512;
	return 0;
}

static int run_control(const struct options *options, struct result *result,
		       const char **stage)
{
	*stage = "settle_before";
	if (bench_wait_for_stable(options->device_stat, &result->sectors_before,
				  &result->settle_timeout))
		return -1;
	*stage = "syncfs";
	if (syncfs_path(options->merged))
		return -1;
	*stage = "settle_after";
	if (bench_wait_for_stable(options->device_stat, &result->sectors_after,
				  &result->settle_timeout))
		return -1;
	*stage = "physical_io";
	return compute_physical_io(result);
}

static int run_edit(const struct options *options, struct result *result,
		    const char **stage)
{
	*stage = "verify_preimage";
	if (verify_input_files(options, result))
		return -1;
	if (!strcmp(options->cache_mode, "cold")) {
		*stage = "cold_cache";
		if (advise_cold(options->lower) || advise_cold(options->target))
			return -1;
	}
	*stage = "settle_before";
	if (bench_wait_for_stable(options->device_stat, &result->sectors_before,
				  &result->settle_timeout))
		return -1;
	*stage = "write";
	if (write_payload(options))
		return -1;
	*stage = "syncfs";
	if (syncfs_path(options->merged))
		return -1;
	*stage = "settle_after";
	if (bench_wait_for_stable(options->device_stat, &result->sectors_after,
				  &result->settle_timeout))
		return -1;
	*stage = "physical_io";
	if (compute_physical_io(result))
		return -1;
	*stage = "fiemap";
	if (bench_fiemap_path(options->upper, options->fiemap_out,
			      &result->fiemap))
		return -1;
	*stage = "verify_postimage";
	if (bench_sha256_path(options->target, result->post_sha256) ||
	    bench_sha256_path(options->upper, result->upper_sha256) ||
	    bench_sha256_path(options->lower, result->lower_sha256))
		return -1;
	if (strcmp(result->post_sha256, options->expected_after) ||
	    strcmp(result->upper_sha256, options->expected_after) ||
	    strcmp(result->post_sha256, result->upper_sha256) ||
	    strcmp(result->lower_sha256, options->expected_before)) {
		errno = EBADMSG;
		return -1;
	}
	return 0;
}

static int write_result(const struct options *options,
			const struct result *result)
{
	char escaped_reason[512];
	char buffer[4096];
	int length;

	if (bench_json_escape(result->reason ? result->reason : "",
			      escaped_reason, sizeof(escaped_reason)))
		return -1;
	length = snprintf(buffer, sizeof(buffer),
		"{\"schema\":1,\"kind\":\"%s\",\"status\":\"%s\","
		"\"errno\":%d,\"invalid_reason\":%s%s%s,"
		"\"settle_timeout\":%s,\"sectors_before\":%" PRIu64 ","
		"\"sectors_after\":%" PRIu64 ",\"physical_io_bytes\":%" PRIu64 ","
		"\"pre_sha256\":\"%s\",\"post_sha256\":\"%s\","
		"\"upper_sha256\":\"%s\",\"lower_sha256\":\"%s\","
		"\"copyup_bytes\":%" PRIu64 ",\"shared_bytes\":%" PRIu64 ","
		"\"allocated_bytes_total\":%" PRIu64 ","
		"\"fiemap_block_size\":%u,\"fiemap_extent_count\":%u}\n",
		result->kind, result->status, result->error_number,
		result->reason ? "\"" : "null", result->reason ? escaped_reason : "",
		result->reason ? "\"" : "",
		result->settle_timeout ? "true" : "false",
		result->sectors_before, result->sectors_after,
		result->physical_io_bytes, result->pre_sha256,
		result->post_sha256, result->upper_sha256,
		result->lower_sha256, result->fiemap.unshared_bytes,
		result->fiemap.shared_bytes, result->fiemap.mapped_bytes,
		result->fiemap.block_size, result->fiemap.extent_count);
	if (length < 0 || (size_t)length >= sizeof(buffer)) {
		errno = EOVERFLOW;
		return -1;
	}
	return bench_atomic_write(options->out, buffer, (size_t)length);
}

int main(int argc, char **argv)
{
	struct options options;
	struct result result = {
		.status = "failed",
	};
	const char *stage = "options";
	char reason[512];
	int ret;

	if (parse_options(argc, argv, &options)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	result.kind = options.control ? "control" : "edit";
	ret = options.control ? run_control(&options, &result, &stage) :
				run_edit(&options, &result, &stage);
	if (ret) {
		result.error_number = errno ? errno : EIO;
		if (snprintf(reason, sizeof(reason), "%s: %s", stage,
			     strerror(result.error_number)) >= (int)sizeof(reason))
			strcpy(reason, "error reason truncated");
		result.reason = reason;
		result.status = result.settle_timeout || result.error_number == EBADMSG ||
				result.error_number == EOPNOTSUPP ? "invalid" : "failed";
	} else {
		result.status = "ok";
	}
	if (write_result(&options, &result)) {
		fprintf(stderr, "copyup_bench: write result: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (ret) {
		fprintf(stderr, "copyup_bench: %s\n", reason);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
