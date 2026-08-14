// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "e2_common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/deltafs.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct measurement {
	int ioctl_ret;
	int ioctl_errno;
	int cpu_before;
	int cpu_after;
	long major_faults;
	uint64_t latency_ns;
	bool clock_backwards;
	bool ioctl_attempted;
};

static void close_fd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static int validate_paths(const struct e2_spec *spec, const char *result_path)
{
	size_t i;

	if (result_path[0] != '/' || spec->merged[0] != '/' ||
	    spec->upper[0] != '/' || spec->work[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < spec->nr_lowers; i++) {
		if (!spec->lowers[i][0] || spec->lowers[i][0] != '/') {
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

static int pin_cpu(int cpu)
{
	cpu_set_t *set;
	size_t set_size;
	int ret;

	if (cpu < 0) {
		errno = EINVAL;
		return -1;
	}
	set_size = CPU_ALLOC_SIZE(cpu + 1);
	set = CPU_ALLOC(cpu + 1);
	if (!set)
		return -1;
	CPU_ZERO_S(set_size, set);
	CPU_SET_S(cpu, set_size, set);
	ret = sched_setaffinity(0, set_size, set);
	CPU_FREE(set);
	return ret;
}

static int open_switch_paths(const struct e2_spec *spec, int *merged_fd,
			     int *upper_fd, int *work_fd, int *lower_fds)
{
	size_t i;

	*merged_fd = open(spec->merged,
			  O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	*upper_fd = open(spec->upper,
			 O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	*work_fd = open(spec->work,
			O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (*merged_fd < 0 || *upper_fd < 0 || *work_fd < 0)
		return -1;
	for (i = 0; i < spec->nr_lowers; i++) {
		lower_fds[i] = open(spec->lowers[i],
				    O_PATH | O_DIRECTORY | O_CLOEXEC |
				    O_NOFOLLOW);
		if (lower_fds[i] < 0)
			return -1;
	}
	return 0;
}

static int run_timed_ioctl(int merged_fd, unsigned long command,
			   struct deltafs_ioc_switch_v1 *request,
			   struct measurement *measurement)
{
	struct rusage usage_before;
	struct rusage usage_after;
	struct timespec before;
	struct timespec after;
	volatile unsigned char touch;
	int saved_errno;

	memset(measurement, 0, sizeof(*measurement));
	touch = ((volatile unsigned char *)request)[0];
	touch ^= ((volatile unsigned char *)request)[sizeof(*request) - 1];
	(void)touch;
	if (getrusage(RUSAGE_SELF, &usage_before))
		return -1;
	measurement->cpu_before = sched_getcpu();
	if (measurement->cpu_before < 0)
		return -1;
	errno = 0;
	measurement->ioctl_attempted = true;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &before))
		return -1;
	measurement->ioctl_ret = ioctl(merged_fd, command, request);
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &after))
		return -1;
	saved_errno = errno;
	measurement->ioctl_errno = measurement->ioctl_ret < 0 ? saved_errno : 0;
	measurement->cpu_after = sched_getcpu();
	if (measurement->cpu_after < 0 ||
	    getrusage(RUSAGE_SELF, &usage_after))
		return -1;
	measurement->major_faults = usage_after.ru_majflt - usage_before.ru_majflt;
	if (after.tv_sec < before.tv_sec ||
	    (after.tv_sec == before.tv_sec && after.tv_nsec < before.tv_nsec)) {
		measurement->clock_backwards = true;
	} else {
		measurement->latency_ns =
			(uint64_t)(after.tv_sec - before.tv_sec) * 1000000000ULL +
			(uint64_t)(after.tv_nsec - before.tv_nsec);
	}
	return 0;
}

static const char *measurement_status(const struct measurement *measurement,
				      const char **reason)
{
	if (measurement->ioctl_ret < 0) {
		*reason = "ioctl_failed";
		return "failed";
	}
	if (measurement->clock_backwards) {
		*reason = "clock_backwards";
		return "invalid";
	}
	if (measurement->cpu_before != measurement->cpu_after) {
		*reason = "cpu_migration";
		return "invalid";
	}
	if (measurement->major_faults) {
		*reason = "major_fault";
		return "invalid";
	}
	*reason = NULL;
	return "ok";
}

static int write_result(const char *path, const struct measurement *measurement,
			const char *status, const char *reason)
{
	char buffer[2048];
	int length;

	length = snprintf(buffer, sizeof(buffer),
		"{\"schema\":1,\"ioctl_attempted\":%s,\"ioctl_ret\":%d,"
		"\"errno\":%d,\"ioctl_latency_ns\":%" PRIu64 ","
		"\"cpu_before\":%d,\"cpu_after\":%d,"
		"\"major_faults\":%ld,\"status\":\"%s\","
		"\"invalid_reason\":%s%s%s}\n",
		measurement->ioctl_attempted ? "true" : "false",
		measurement->ioctl_ret, measurement->ioctl_errno,
		measurement->latency_ns, measurement->cpu_before,
		measurement->cpu_after, measurement->major_faults, status,
		reason ? "\"" : "null", reason ? reason : "",
		reason ? "\"" : "");
	if (length < 0 || (size_t)length >= sizeof(buffer)) {
		errno = EOVERFLOW;
		return -1;
	}
	return e2_atomic_write(path, buffer, (size_t)length);
}

int main(int argc, char **argv)
{
	struct deltafs_ioc_switch_v1 request;
	struct measurement measurement = {
		.ioctl_ret = -1,
		.cpu_before = -1,
		.cpu_after = -1,
	};
	struct e2_spec spec;
	int lower_fds[DELTAFS_V1_MAX_LOWERS];
	const char *reason = NULL;
	const char *status;
	unsigned long command;
	int merged_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int ret = EXIT_FAILURE;
	size_t i;

	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		lower_fds[i] = -1;
	if (argc != 3 || argv[1][0] != '/' || argv[2][0] != '/') {
		fprintf(stderr, "Usage: %s ABSOLUTE_SPEC ABSOLUTE_RESULT\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	if (e2_read_spec(argv[1], &spec)) {
		fprintf(stderr, "switch_once: read spec: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (validate_paths(&spec, argv[2]) || pin_cpu(spec.cpu)) {
		fprintf(stderr, "switch_once: validate environment: %s\n",
			strerror(errno));
		goto out;
	}
	if (spec.nr_lowers > DELTAFS_V1_MAX_LOWERS) {
		int build_ret;

		errno = 0;
		build_ret = e2_build_request(&request, -1, -1, NULL,
				       (unsigned int)spec.nr_lowers,
				       spec.expected_generation);
		if (build_ret != -1 || errno != E2BIG) {
			measurement.ioctl_errno = errno;
			status = "failed";
			reason = "preflight_failed";
		} else {
			measurement.ioctl_errno = E2BIG;
			status = "expected_reject";
			reason = NULL;
		}
		if (write_result(argv[2], &measurement, status, reason))
			goto out;
		ret = !strcmp(status, "expected_reject") ? EXIT_SUCCESS :
			EXIT_FAILURE;
		goto out;
	}
	if (open_switch_paths(&spec, &merged_fd, &upper_fd, &work_fd,
			      lower_fds)) {
		fprintf(stderr, "switch_once: open switch paths: %s\n",
			strerror(errno));
		goto out;
	}
	if (e2_build_request(&request, upper_fd, work_fd, lower_fds,
			     (unsigned int)spec.nr_lowers,
			     spec.expected_generation)) {
		fprintf(stderr, "switch_once: build request: %s\n",
			strerror(errno));
		goto out;
	}
	command = !strcmp(spec.operation, "checkpoint") ?
		  DELTAFS_IOC_CHECKPOINT : DELTAFS_IOC_RESTORE;
	if (run_timed_ioctl(merged_fd, command, &request, &measurement)) {
		fprintf(stderr, "switch_once: measure ioctl: %s\n",
			strerror(errno));
		goto out;
	}
	status = measurement_status(&measurement, &reason);
	close_fd(&merged_fd);
	close_fd(&upper_fd);
	close_fd(&work_fd);
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		close_fd(&lower_fds[i]);
	if (write_result(argv[2], &measurement, status, reason)) {
		fprintf(stderr, "switch_once: write result: %s\n", strerror(errno));
		goto out;
	}
	ret = !strcmp(status, "failed") ? EXIT_FAILURE : EXIT_SUCCESS;
out:
	close_fd(&merged_fd);
	close_fd(&upper_fd);
	close_fd(&work_fd);
	for (i = 0; i < DELTAFS_V1_MAX_LOWERS; i++)
		close_fd(&lower_fds[i]);
	e2_free_spec(&spec);
	return ret;
}
