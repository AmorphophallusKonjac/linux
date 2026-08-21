/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DELTAFS_E2_V2_COMMON_H
#define DELTAFS_E2_V2_COMMON_H

#include <linux/deltafs.h>
#include <stdint.h>

int e2_build_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *request,
				int upper_fd, int work_fd,
				uint64_t expected_generation);
int e2_checkpoint_v2(const char *merged, const char *upper, const char *work,
		     uint64_t expected_generation);

#endif /* DELTAFS_E2_V2_COMMON_H */
