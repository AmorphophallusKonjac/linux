/* SPDX-License-Identifier: GPL-2.0 */
#ifndef DELTAFS_E3_BENCH_COMMON_H
#define DELTAFS_E3_BENCH_COMMON_H

#include <linux/fiemap.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BENCH_SHA256_HEX_SIZE 65
#define BENCH_SCHEMA 3U
#define BENCH_SETTLE_INTERVAL_MS 100U
#define BENCH_SETTLE_STABLE_COMPARISONS 3U
#define BENCH_SETTLE_MAX_COMPARISONS 100U

struct bench_fiemap_summary {
	uint64_t mapped_bytes;
	uint64_t shared_bytes;
	uint64_t unshared_bytes;
	uint64_t hole_bytes;
	uint32_t block_size;
	uint32_t extent_count;
};

struct bench_settle_ops {
	int (*read)(void *opaque, uint64_t *value);
	int (*sleep_ms)(void *opaque, unsigned int milliseconds);
	void *opaque;
};

int bench_atomic_write(const char *path, const char *data, size_t length);
int bench_json_escape(const char *source, char *destination, size_t size);
int bench_sha256_buffer(const void *data, size_t length,
			char output[BENCH_SHA256_HEX_SIZE]);
int bench_sha256_path(const char *path,
		      char output[BENCH_SHA256_HEX_SIZE]);
int bench_fill_bytes(uint64_t seed, void *buffer, size_t length);

int bench_parse_sectors_written(const char *text, uint64_t *value);
int bench_read_sectors_written(const char *path, uint64_t *value);
int bench_wait_stable_ops(const struct bench_settle_ops *ops,
			  unsigned int required_stable,
			  unsigned int max_comparisons,
			  uint64_t *value, bool *timed_out);
int bench_wait_for_stable(const char *path, uint64_t *value,
			  bool *timed_out);

int bench_summarize_extents(const struct fiemap_extent *extents,
			    size_t extent_count, uint64_t file_size,
			    uint32_t block_size,
			    struct bench_fiemap_summary *summary);
int bench_fiemap_path(const char *path, const char *dump_path,
		      struct bench_fiemap_summary *summary);

#endif
