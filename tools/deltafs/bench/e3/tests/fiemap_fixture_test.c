// SPDX-License-Identifier: GPL-2.0
#include "../bench_common.h"

#include <assert.h>
#include <errno.h>
#include <linux/fiemap.h>
#include <string.h>

static void check_all_shared(void)
{
	struct fiemap_extent extents[2] = {
		{ .fe_logical = 0, .fe_length = 4096,
		  .fe_flags = FIEMAP_EXTENT_SHARED },
		{ .fe_logical = 4096, .fe_length = 4096,
		  .fe_flags = FIEMAP_EXTENT_SHARED | FIEMAP_EXTENT_LAST },
	};
	struct bench_fiemap_summary summary;

	assert(!bench_summarize_extents(extents, 2, 8192, 4096, &summary));
	assert(summary.mapped_bytes == 8192);
	assert(summary.shared_bytes == 8192);
	assert(summary.unshared_bytes == 0);
	assert(summary.hole_bytes == 0);
	assert(summary.extent_count == 2);
}

static void check_mixed_and_holes(void)
{
	struct fiemap_extent extents[2] = {
		{ .fe_logical = 4096, .fe_length = 4096,
		  .fe_flags = FIEMAP_EXTENT_SHARED },
		{ .fe_logical = 12288, .fe_length = 4096,
		  .fe_flags = FIEMAP_EXTENT_MERGED | FIEMAP_EXTENT_LAST },
	};
	struct bench_fiemap_summary summary;

	assert(!bench_summarize_extents(extents, 2, 20480, 4096, &summary));
	assert(summary.mapped_bytes == 8192);
	assert(summary.shared_bytes == 4096);
	assert(summary.unshared_bytes == 4096);
	assert(summary.hole_bytes == 12288);
}

static void check_invalid(void)
{
	struct bench_fiemap_summary summary;
	struct fiemap_extent unknown = {
		.fe_length = 4096,
		.fe_flags = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_LAST,
	};
	struct fiemap_extent no_last = { .fe_length = 4096 };
	struct fiemap_extent overlap[2] = {
		{ .fe_logical = 0, .fe_length = 4096 },
		{ .fe_logical = 2048, .fe_length = 4096,
		  .fe_flags = FIEMAP_EXTENT_LAST },
	};

	errno = 0;
	assert(bench_summarize_extents(&unknown, 1, 4096, 4096, &summary) == -1);
	assert(errno == EOPNOTSUPP);
	errno = 0;
	assert(bench_summarize_extents(&no_last, 1, 4096, 4096, &summary) == -1);
	assert(errno == EOPNOTSUPP);
	errno = 0;
	assert(bench_summarize_extents(overlap, 2, 8192, 4096, &summary) == -1);
	assert(errno == EOPNOTSUPP);
}

int main(void)
{
	check_all_shared();
	check_mixed_and_holes();
	check_invalid();
	return 0;
}

