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
	unsigned int source_depth;
	uint64_t expected_generation;
	unsigned int keep_bottom;
	char *merged;
	char *upper;
	char *work;
	char **lower_prefix;
	size_t nr_lower_prefix;
};

int e2_validate_checkpoint_source_depth(unsigned int source_depth);
int e2_build_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *req,
				int upper_fd, int work_fd,
				uint64_t expected_generation);
int e2_build_restore_request(struct deltafs_ioc_restore_v2 *req,
			     int upper_fd, int work_fd,
			     const int *lower_fds,
			     unsigned int nr_lower_prefix,
			     unsigned int keep_bottom,
			     uint64_t expected_generation);
int e2_read_spec(const char *path, struct e2_spec *spec);
void e2_free_spec(struct e2_spec *spec);
int e2_atomic_write(const char *path, const char *data, size_t length);

#endif /* E2_COMMON_H */
