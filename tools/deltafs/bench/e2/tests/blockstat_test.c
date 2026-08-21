// SPDX-License-Identifier: GPL-2.0
#include "../bench_common.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

struct fixture {
	const uint64_t *values;
	size_t count;
	size_t index;
};

static int fixture_read(void *opaque, uint64_t *value)
{
	struct fixture *fixture = opaque;

	if (fixture->index >= fixture->count) {
		errno = EIO;
		return -1;
	}
	*value = fixture->values[fixture->index++];
	return 0;
}

static int fixture_sleep(void *opaque, unsigned int milliseconds)
{
	(void)opaque;
	assert(milliseconds == BENCH_SETTLE_INTERVAL_MS);
	return 0;
}

static void check_parser(void)
{
	uint64_t value;

	assert(!bench_parse_sectors_written("1 2 3 4 5 6 789 8 9\n", &value));
	assert(value == 789);
	assert(!bench_parse_sectors_written("  0\t0 0 0 0 0 18446744073709551615", &value));
	assert(value == UINT64_MAX);
	errno = 0;
	assert(bench_parse_sectors_written("1 2 3 4 5 6", &value) == -1);
	assert(errno == EINVAL);
	errno = 0;
	assert(bench_parse_sectors_written("1 2 3 4 5 6 -1", &value) == -1);
	assert(errno == EINVAL);
	errno = 0;
	assert(bench_parse_sectors_written(
		"1 2 3 4 5 6 18446744073709551616", &value) == -1);
	assert(errno == ERANGE);
}

static void check_settle(void)
{
	const uint64_t values[] = { 1, 2, 2, 2, 2 };
	struct fixture fixture = { .values = values, .count = 5 };
	struct bench_settle_ops ops = {
		.read = fixture_read,
		.sleep_ms = fixture_sleep,
		.opaque = &fixture,
	};
	uint64_t value = 0;
	bool timeout = true;

	assert(!bench_wait_stable_ops(&ops, 3, 4, &value, &timeout));
	assert(value == 2);
	assert(!timeout);
}

static void check_timeout(void)
{
	const uint64_t values[] = { 1, 2, 3, 4 };
	struct fixture fixture = { .values = values, .count = 4 };
	struct bench_settle_ops ops = {
		.read = fixture_read,
		.sleep_ms = fixture_sleep,
		.opaque = &fixture,
	};
	uint64_t value = 0;
	bool timeout = false;

	errno = 0;
	assert(bench_wait_stable_ops(&ops, 3, 3, &value, &timeout) == -1);
	assert(errno == ETIMEDOUT);
	assert(timeout);
	assert(value == 4);
}

int main(void)
{
	check_parser();
	check_settle();
	check_timeout();
	return 0;
}

