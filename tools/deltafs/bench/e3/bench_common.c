// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "bench_common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct sha256_context {
	uint32_t state[8];
	uint64_t total;
	unsigned char block[64];
	size_t used;
};

static uint32_t rotate_right(uint32_t value, unsigned int count)
{
	return (value >> count) | (value << (32U - count));
}

static uint32_t load_be32(const unsigned char *data)
{
	return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
	       ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void store_be32(unsigned char *data, uint32_t value)
{
	data[0] = (unsigned char)(value >> 24);
	data[1] = (unsigned char)(value >> 16);
	data[2] = (unsigned char)(value >> 8);
	data[3] = (unsigned char)value;
}

static void store_le64(unsigned char *data, uint64_t value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		data[i] = (unsigned char)(value >> (i * 8));
}

static void sha256_transform(struct sha256_context *context,
			     const unsigned char block[64])
{
	static const uint32_t constants[64] = {
		0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
		0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
		0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
		0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
		0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
		0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
		0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
		0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
		0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
		0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
		0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
		0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
		0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
		0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
		0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
		0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
	};
	uint32_t schedule[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned int i;

	for (i = 0; i < 16; i++)
		schedule[i] = load_be32(block + i * 4);
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
			      rotate_right(schedule[i - 15], 18) ^
			      (schedule[i - 15] >> 3);
		uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
			      rotate_right(schedule[i - 2], 19) ^
			      (schedule[i - 2] >> 10);

		schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
	}
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	for (i = 0; i < 64; i++) {
		uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
				rotate_right(e, 25);
		uint32_t choice = (e & f) ^ ((~e) & g);
		uint32_t temporary1 = h + sum1 + choice + constants[i] +
				      schedule[i];
		uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
				rotate_right(a, 22);
		uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
		uint32_t temporary2 = sum0 + majority;

		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

static void sha256_init(struct sha256_context *context)
{
	static const uint32_t initial[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
	};

	memcpy(context->state, initial, sizeof(initial));
	context->total = 0;
	context->used = 0;
}

static void sha256_update(struct sha256_context *context, const void *input,
			  size_t length)
{
	const unsigned char *data = input;

	context->total += length;
	while (length) {
		size_t count = sizeof(context->block) - context->used;

		if (count > length)
			count = length;
		memcpy(context->block + context->used, data, count);
		context->used += count;
		data += count;
		length -= count;
		if (context->used == sizeof(context->block)) {
			sha256_transform(context, context->block);
			context->used = 0;
		}
	}
}

static void sha256_final(struct sha256_context *context,
			 unsigned char digest[32])
{
	uint64_t bits = context->total * 8;
	unsigned int i;

	context->block[context->used++] = 0x80;
	if (context->used > 56) {
		memset(context->block + context->used, 0,
		       sizeof(context->block) - context->used);
		sha256_transform(context, context->block);
		context->used = 0;
	}
	memset(context->block + context->used, 0, 56 - context->used);
	for (i = 0; i < 8; i++)
		context->block[63 - i] = (unsigned char)(bits >> (i * 8));
	sha256_transform(context, context->block);
	for (i = 0; i < 8; i++)
		store_be32(digest + i * 4, context->state[i]);
}

static void digest_hex(const unsigned char digest[32], char output[65])
{
	static const char digits[] = "0123456789abcdef";
	unsigned int i;

	for (i = 0; i < 32; i++) {
		output[i * 2] = digits[digest[i] >> 4];
		output[i * 2 + 1] = digits[digest[i] & 15];
	}
	output[64] = '\0';
}

int bench_sha256_buffer(const void *data, size_t length, char output[65])
{
	struct sha256_context context;
	unsigned char digest[32];

	if ((!data && length) || !output) {
		errno = EINVAL;
		return -1;
	}
	sha256_init(&context);
	sha256_update(&context, data, length);
	sha256_final(&context, digest);
	digest_hex(digest, output);
	return 0;
}

int bench_sha256_path(const char *path, char output[65])
{
	struct sha256_context context;
	unsigned char buffer[64 * 1024];
	unsigned char digest[32];
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	sha256_init(&context);
	for (;;) {
		ssize_t count = read(fd, buffer, sizeof(buffer));

		if (count < 0) {
			int saved_errno;

			if (errno == EINTR)
				continue;
			saved_errno = errno;
			close(fd);
			errno = saved_errno;
			return -1;
		}
		if (!count)
			break;
		sha256_update(&context, buffer, (size_t)count);
	}
	if (close(fd))
		return -1;
	sha256_final(&context, digest);
	digest_hex(digest, output);
	return 0;
}

int bench_fill_bytes(uint64_t seed, void *buffer, size_t length)
{
	static const unsigned char prefix[] = "deltafs-e3-v3\0";
	unsigned char input[sizeof(prefix) - 1 + 16];
	unsigned char digest[32];
	struct sha256_context context;
	unsigned char *output = buffer;
	uint64_t counter = 0;

	if (!buffer && length) {
		errno = EINVAL;
		return -1;
	}
	memcpy(input, prefix, sizeof(prefix) - 1);
	store_le64(input + sizeof(prefix) - 1, seed);
	while (length) {
		size_t count = length < sizeof(digest) ? length : sizeof(digest);

		store_le64(input + sizeof(prefix) - 1 + 8, counter++);
		sha256_init(&context);
		sha256_update(&context, input, sizeof(input));
		sha256_final(&context, digest);
		memcpy(output, digest, count);
		output += count;
		length -= count;
	}
	return 0;
}

static int write_all(int fd, const char *data, size_t length)
{
	while (length) {
		ssize_t count = write(fd, data, length);

		if (count < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!count) {
			errno = EIO;
			return -1;
		}
		data += count;
		length -= (size_t)count;
	}
	return 0;
}

int bench_atomic_write(const char *path, const char *data, size_t length)
{
	char *temporary;
	size_t temporary_size;
	int fd = -1;
	int saved_errno;
	int ret = -1;

	if (!path || path[0] != '/' || (!data && length)) {
		errno = EINVAL;
		return -1;
	}
	temporary_size = strlen(path) + 64;
	temporary = malloc(temporary_size);
	if (!temporary)
		return -1;
	if (snprintf(temporary, temporary_size, "%s.tmp.%ld", path,
		     (long)getpid()) >= (int)temporary_size) {
		errno = ENAMETOOLONG;
		goto out;
	}
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		goto out;
	if (write_all(fd, data, length) || fsync(fd))
		goto out;
	if (close(fd)) {
		fd = -1;
		goto out;
	}
	fd = -1;
	if (rename(temporary, path))
		goto out;
	ret = 0;
out:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (ret)
		unlink(temporary);
	free(temporary);
	errno = saved_errno;
	return ret;
}

int bench_json_escape(const char *source, char *destination, size_t size)
{
	static const char hex[] = "0123456789abcdef";
	size_t used = 0;

	if (!source || !destination || !size) {
		errno = EINVAL;
		return -1;
	}
	while (*source) {
		unsigned char value = (unsigned char)*source++;
		char encoded[6];
		size_t count = 1;

		encoded[0] = (char)value;
		if (value == '"' || value == '\\') {
			encoded[0] = '\\';
			encoded[1] = (char)value;
			count = 2;
		} else if (value < 0x20) {
			encoded[0] = '\\';
			encoded[1] = 'u';
			encoded[2] = '0';
			encoded[3] = '0';
			encoded[4] = hex[value >> 4];
			encoded[5] = hex[value & 15];
			count = 6;
		}
		if (count > size - used - 1) {
			errno = ENOSPC;
			return -1;
		}
		memcpy(destination + used, encoded, count);
		used += count;
	}
	destination[used] = '\0';
	return 0;
}

int bench_parse_sectors_written(const char *text, uint64_t *value)
{
	const char *cursor = text;
	unsigned int field;

	if (!text || !value) {
		errno = EINVAL;
		return -1;
	}
	for (field = 1; field <= 7; field++) {
		uint64_t number = 0;
		bool digit = false;

		while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' ||
		       *cursor == '\r' || *cursor == '\f' || *cursor == '\v')
			cursor++;
		if (*cursor == '+' || *cursor == '-') {
			errno = EINVAL;
			return -1;
		}
		while (*cursor >= '0' && *cursor <= '9') {
			unsigned int decimal = (unsigned int)(*cursor - '0');

			digit = true;
			if (number > (UINT64_MAX - decimal) / 10) {
				errno = ERANGE;
				return -1;
			}
			number = number * 10 + decimal;
			cursor++;
		}
		if (!digit || (*cursor && *cursor != ' ' && *cursor != '\t' &&
			       *cursor != '\n' && *cursor != '\r' &&
			       *cursor != '\f' && *cursor != '\v')) {
			errno = EINVAL;
			return -1;
		}
		if (field == 7) {
			*value = number;
			return 0;
		}
	}
	errno = EINVAL;
	return -1;
}

int bench_read_sectors_written(const char *path, uint64_t *value)
{
	char buffer[4096];
	ssize_t count;
	int fd;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	for (;;) {
		count = read(fd, buffer, sizeof(buffer) - 1);
		if (count < 0 && errno == EINTR)
			continue;
		break;
	}
	if (count < 0) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (count == (ssize_t)(sizeof(buffer) - 1)) {
		close(fd);
		errno = EOVERFLOW;
		return -1;
	}
	buffer[count] = '\0';
	if (close(fd))
		return -1;
	return bench_parse_sectors_written(buffer, value);
}

int bench_wait_stable_ops(const struct bench_settle_ops *ops,
			  unsigned int required_stable,
			  unsigned int max_comparisons,
			  uint64_t *value, bool *timed_out)
{
	uint64_t previous;
	unsigned int stable = 0;
	unsigned int comparison;

	if (!ops || !ops->read || !ops->sleep_ms || !required_stable ||
	    !max_comparisons || !value || !timed_out) {
		errno = EINVAL;
		return -1;
	}
	*timed_out = false;
	if (ops->read(ops->opaque, &previous))
		return -1;
	for (comparison = 0; comparison < max_comparisons; comparison++) {
		uint64_t current;

		if (ops->sleep_ms(ops->opaque, BENCH_SETTLE_INTERVAL_MS) ||
		    ops->read(ops->opaque, &current))
			return -1;
		if (current == previous)
			stable++;
		else
			stable = 0;
		previous = current;
		if (stable == required_stable) {
			*value = current;
			return 0;
		}
	}
	*value = previous;
	*timed_out = true;
	errno = ETIMEDOUT;
	return -1;
}

struct path_settle_context {
	const char *path;
};

static int path_settle_read(void *opaque, uint64_t *value)
{
	struct path_settle_context *context = opaque;

	return bench_read_sectors_written(context->path, value);
}

static int path_settle_sleep(void *opaque, unsigned int milliseconds)
{
	struct timespec request = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (long)(milliseconds % 1000) * 1000000L,
	};

	(void)opaque;
	while (nanosleep(&request, &request)) {
		if (errno != EINTR)
			return -1;
	}
	return 0;
}

int bench_wait_for_stable(const char *path, uint64_t *value, bool *timed_out)
{
	struct path_settle_context context = { .path = path };
	struct bench_settle_ops ops = {
		.read = path_settle_read,
		.sleep_ms = path_settle_sleep,
		.opaque = &context,
	};

	return bench_wait_stable_ops(&ops, BENCH_SETTLE_STABLE_COMPARISONS,
				     BENCH_SETTLE_MAX_COMPARISONS,
				     value, timed_out);
}

int bench_summarize_extents(const struct fiemap_extent *extents,
			    size_t extent_count, uint64_t file_size,
			    uint32_t block_size,
			    struct bench_fiemap_summary *summary)
{
	const uint32_t allowed = FIEMAP_EXTENT_LAST | FIEMAP_EXTENT_SHARED |
				 FIEMAP_EXTENT_MERGED;
	uint64_t cursor = 0;
	size_t i;

	if ((!extents && extent_count) || !file_size || !block_size || !summary) {
		errno = EINVAL;
		return -1;
	}
	memset(summary, 0, sizeof(*summary));
	summary->block_size = block_size;
	for (i = 0; i < extent_count; i++) {
		const struct fiemap_extent *extent = &extents[i];
		uint64_t end;

		if (!extent->fe_length || extent->fe_flags & ~allowed ||
		    extent->fe_logical < cursor ||
		    extent->fe_logical > UINT64_MAX - extent->fe_length) {
			errno = EOPNOTSUPP;
			return -1;
		}
		end = extent->fe_logical + extent->fe_length;
		if (end > file_size ||
		    ((extent->fe_flags & FIEMAP_EXTENT_LAST) && i + 1 != extent_count) ||
		    (!(extent->fe_flags & FIEMAP_EXTENT_LAST) && i + 1 == extent_count)) {
			errno = EOPNOTSUPP;
			return -1;
		}
		summary->hole_bytes += extent->fe_logical - cursor;
		if (summary->mapped_bytes > UINT64_MAX - extent->fe_length) {
			errno = EOVERFLOW;
			return -1;
		}
		summary->mapped_bytes += extent->fe_length;
		if (extent->fe_flags & FIEMAP_EXTENT_SHARED)
			summary->shared_bytes += extent->fe_length;
		else
			summary->unshared_bytes += extent->fe_length;
		cursor = end;
	}
	if (!extent_count || cursor > file_size) {
		errno = EOPNOTSUPP;
		return -1;
	}
	summary->hole_bytes += file_size - cursor;
	if (extent_count > UINT32_MAX) {
		errno = EOVERFLOW;
		return -1;
	}
	summary->extent_count = (uint32_t)extent_count;
	return 0;
}

static int append_extents(struct fiemap_extent **all, size_t *count,
			  const struct fiemap_extent *new_extents,
			  size_t new_count)
{
	struct fiemap_extent *resized;

	if (new_count > (SIZE_MAX / sizeof(**all)) - *count) {
		errno = EOVERFLOW;
		return -1;
	}
	resized = realloc(*all, (*count + new_count) * sizeof(**all));
	if (!resized)
		return -1;
	memcpy(resized + *count, new_extents, new_count * sizeof(**all));
	*all = resized;
	*count += new_count;
	return 0;
}

static int write_fiemap_dump(const char *path,
			     const struct fiemap_extent *extents, size_t count,
			     uint64_t file_size, uint32_t block_size,
			     bool valid, int error_number)
{
	size_t capacity = 512 + count * 192;
	char *buffer = malloc(capacity);
	size_t used;
	size_t i;
	int length;
	int ret;

	if (!buffer)
		return -1;
	length = snprintf(buffer, capacity,
		"{\"schema\":%u,\"file_size\":%" PRIu64
		",\"block_size\":%u,\"status\":\"%s\",\"errno\":%d,"
		"\"extents\":[", BENCH_SCHEMA, file_size, block_size,
		valid ? "ok" : "invalid", error_number);
	if (length < 0 || (size_t)length >= capacity) {
		free(buffer);
		errno = EOVERFLOW;
		return -1;
	}
	used = (size_t)length;
	for (i = 0; i < count; i++) {
		length = snprintf(buffer + used, capacity - used,
			"%s{\"logical\":%" PRIu64 ",\"physical\":%" PRIu64
			",\"length\":%" PRIu64 ",\"flags\":%u}",
			i ? "," : "", (uint64_t)extents[i].fe_logical,
			(uint64_t)extents[i].fe_physical,
			(uint64_t)extents[i].fe_length, extents[i].fe_flags);
		if (length < 0 || (size_t)length >= capacity - used) {
			free(buffer);
			errno = EOVERFLOW;
			return -1;
		}
		used += (size_t)length;
	}
	if (capacity - used < 4) {
		free(buffer);
		errno = EOVERFLOW;
		return -1;
	}
	memcpy(buffer + used, "]}\n", 4);
	used += 3;
	ret = bench_atomic_write(path, buffer, used);
	free(buffer);
	return ret;
}

int bench_fiemap_path(const char *path, const char *dump_path,
		      struct bench_fiemap_summary *summary)
{
	const uint32_t batch_count = 64;
	struct fiemap_extent *extents = NULL;
	struct stat file_stat = { 0 };
	uint64_t start = 0;
	size_t extent_count = 0;
	bool last = false;
	int fd = -1;
	int ret = -1;
	int saved_errno;

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &file_stat) || !S_ISREG(file_stat.st_mode) ||
	    file_stat.st_size <= 0 || file_stat.st_blksize <= 0) {
		if (!errno)
			errno = EINVAL;
		goto out;
	}
	while (!last) {
		size_t bytes = sizeof(struct fiemap) +
			       batch_count * sizeof(struct fiemap_extent);
		struct fiemap *map = calloc(1, bytes);
		uint32_t i;

		if (!map)
			goto out;
		map->fm_start = start;
		map->fm_length = FIEMAP_MAX_OFFSET - start;
		map->fm_flags = FIEMAP_FLAG_SYNC;
		map->fm_extent_count = batch_count;
		if (ioctl(fd, FS_IOC_FIEMAP, map)) {
			saved_errno = errno;
			free(map);
			errno = saved_errno;
			goto out;
		}
		if (map->fm_mapped_extents > batch_count ||
		    !map->fm_mapped_extents) {
			free(map);
			errno = EOPNOTSUPP;
			goto out;
		}
		if (append_extents(&extents, &extent_count, map->fm_extents,
				   map->fm_mapped_extents)) {
			saved_errno = errno;
			free(map);
			errno = saved_errno;
			goto out;
		}
		for (i = 0; i < map->fm_mapped_extents; i++)
			if (map->fm_extents[i].fe_flags & FIEMAP_EXTENT_LAST)
				last = true;
		if (!last) {
			struct fiemap_extent *tail =
				&map->fm_extents[map->fm_mapped_extents - 1];

			if (!tail->fe_length ||
			    tail->fe_logical > UINT64_MAX - tail->fe_length) {
				free(map);
				errno = EOPNOTSUPP;
				goto out;
			}
			start = tail->fe_logical + tail->fe_length;
		}
		free(map);
		if (extent_count > 1000000) {
			errno = E2BIG;
			goto out;
		}
	}
	if (bench_summarize_extents(extents, extent_count,
				    (uint64_t)file_stat.st_size,
				    (uint32_t)file_stat.st_blksize, summary))
		goto out;
	if (write_fiemap_dump(dump_path, extents, extent_count,
			      (uint64_t)file_stat.st_size,
			      (uint32_t)file_stat.st_blksize, true, 0))
		goto out;
	ret = 0;
out:
	saved_errno = errno;
	if (ret && dump_path)
		(void)write_fiemap_dump(dump_path, extents, extent_count,
					(uint64_t)(file_stat.st_size > 0 ?
					file_stat.st_size : 0),
					(uint32_t)(file_stat.st_blksize > 0 ?
					file_stat.st_blksize : 0), false,
					saved_errno);
	if (fd >= 0)
		close(fd);
	free(extents);
	errno = saved_errno;
	return ret;
}
