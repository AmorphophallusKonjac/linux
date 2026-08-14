/* SPDX-License-Identifier: GPL-2.0 */
#ifndef E2_COMMON_H
#define E2_COMMON_H

#include <linux/deltafs.h>
#include <stddef.h>
#include <stdint.h>

struct e2_spec {
	unsigned int schema;
	char *operation;
	int cpu;
	uint64_t expected_generation;
	char *merged;
	char *upper;
	char *work;
	char **lowers;
	size_t nr_lowers;
};

int e2_build_request(struct deltafs_ioc_switch_v1 *req, int upper_fd,
		     int work_fd, const int *lower_fds,
		     unsigned int nr_lower, uint64_t expected_generation);
int e2_read_spec(const char *path, struct e2_spec *spec);
void e2_free_spec(struct e2_spec *spec);
int e2_atomic_write(const char *path, const char *data, size_t length);

#endif /* E2_COMMON_H */
