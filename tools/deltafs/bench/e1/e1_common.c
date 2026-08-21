// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include "e1_common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define E1_SPEC_MAX_BYTES (1024U * 1024U)

struct json_reader {
	const char *cursor;
	const char *end;
};

static void skip_space(struct json_reader *reader)
{
	while (reader->cursor < reader->end &&
	       isspace((unsigned char)*reader->cursor))
		reader->cursor++;
}

static int consume(struct json_reader *reader, char expected)
{
	skip_space(reader);
	if (reader->cursor == reader->end || *reader->cursor != expected) {
		errno = EINVAL;
		return -1;
	}
	reader->cursor++;
	return 0;
}

static int append_char(char **value, size_t *length, size_t *capacity, char c)
{
	char *next;

	if (*length + 1 >= *capacity) {
		size_t new_capacity = *capacity ? *capacity * 2 : 32;

		if (new_capacity > E1_SPEC_MAX_BYTES) {
			errno = E2BIG;
			return -1;
		}
		next = realloc(*value, new_capacity);
		if (!next)
			return -1;
		*value = next;
		*capacity = new_capacity;
	}
	(*value)[(*length)++] = c;
	return 0;
}

static int parse_string(struct json_reader *reader, char **result)
{
	char *value = NULL;
	size_t length = 0;
	size_t capacity = 0;

	if (consume(reader, '"'))
		return -1;
	while (reader->cursor < reader->end) {
		unsigned char c = (unsigned char)*reader->cursor++;

		if (c == '"') {
			if (append_char(&value, &length, &capacity, '\0'))
				goto fail;
			*result = value;
			return 0;
		}
		if (c < 0x20) {
			errno = EINVAL;
			goto fail;
		}
		if (c == '\\') {
			if (reader->cursor == reader->end) {
				errno = EINVAL;
				goto fail;
			}
			c = (unsigned char)*reader->cursor++;
			switch (c) {
			case '"':
			case '\\':
			case '/':
				break;
			case 'b':
				c = '\b';
				break;
			case 'f':
				c = '\f';
				break;
			case 'n':
				c = '\n';
				break;
			case 'r':
				c = '\r';
				break;
			case 't':
				c = '\t';
				break;
			default:
				errno = EINVAL;
				goto fail;
			}
		}
		if (append_char(&value, &length, &capacity, (char)c))
			goto fail;
	}
	errno = EINVAL;
fail:
	free(value);
	return -1;
}

static int parse_u64(struct json_reader *reader, uint64_t *value)
{
	char *end;
	unsigned long long parsed;

	skip_space(reader);
	if (reader->cursor == reader->end ||
	    !isdigit((unsigned char)*reader->cursor)) {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	parsed = strtoull(reader->cursor, &end, 10);
	if (errno || end > reader->end) {
		errno = EINVAL;
		return -1;
	}
	reader->cursor = end;
	*value = (uint64_t)parsed;
	return 0;
}

static int parse_int(struct json_reader *reader, int *value)
{
	uint64_t parsed;

	if (parse_u64(reader, &parsed))
		return -1;
	if (parsed > INT_MAX) {
		errno = ERANGE;
		return -1;
	}
	*value = (int)parsed;
	return 0;
}

static int append_lower_prefix(struct e1_spec *spec, char *path)
{
	char **next;

	if (spec->nr_lower_prefix == SIZE_MAX / sizeof(*spec->lower_prefix)) {
		errno = E2BIG;
		return -1;
	}
	next = realloc(spec->lower_prefix,
		       (spec->nr_lower_prefix + 1) * sizeof(*spec->lower_prefix));
	if (!next)
		return -1;
	spec->lower_prefix = next;
	spec->lower_prefix[spec->nr_lower_prefix++] = path;
	return 0;
}

static int parse_lower_prefix(struct json_reader *reader, struct e1_spec *spec)
{
	bool first = true;

	if (consume(reader, '['))
		return -1;
	for (;;) {
		char *path;

		skip_space(reader);
		if (reader->cursor < reader->end && *reader->cursor == ']') {
			reader->cursor++;
			return 0;
		}
		if (!first && consume(reader, ','))
			return -1;
		if (parse_string(reader, &path))
			return -1;
		if (append_lower_prefix(spec, path)) {
			free(path);
			return -1;
		}
		first = false;
	}
}

static int set_string_once(char **field, char *value)
{
	if (*field) {
		free(value);
		errno = EINVAL;
		return -1;
	}
	*field = value;
	return 0;
}

static int parse_field(struct json_reader *reader, struct e1_spec *spec,
		       unsigned int *seen, const char *key)
{
	uint64_t number;
	char *value;

	if (!strcmp(key, "schema")) {
		if (*seen & (1U << 0) || parse_u64(reader, &number) ||
		    number > UINT_MAX) {
			errno = EINVAL;
			return -1;
		}
		spec->schema = (unsigned int)number;
		*seen |= 1U << 0;
		return 0;
	}
	if (!strcmp(key, "operation")) {
		if (parse_string(reader, &value) ||
		    set_string_once(&spec->operation, value))
			return -1;
		*seen |= 1U << 1;
		return 0;
	}
	if (!strcmp(key, "cpu")) {
		if (*seen & (1U << 2) || parse_int(reader, &spec->cpu)) {
			errno = EINVAL;
			return -1;
		}
		*seen |= 1U << 2;
		return 0;
	}
	if (!strcmp(key, "source_depth")) {
		if (*seen & (1U << 3) || parse_u64(reader, &number) ||
		    !number || number > UINT_MAX) {
			errno = EINVAL;
			return -1;
		}
		spec->source_depth = (unsigned int)number;
		*seen |= 1U << 3;
		return 0;
	}
	if (!strcmp(key, "expected_generation")) {
		if (*seen & (1U << 4) ||
		    parse_u64(reader, &spec->expected_generation)) {
			errno = EINVAL;
			return -1;
		}
		*seen |= 1U << 4;
		return 0;
	}
	if (!strcmp(key, "keep_bottom")) {
		if (*seen & (1U << 5) || parse_u64(reader, &number) ||
		    number > UINT_MAX) {
			errno = EINVAL;
			return -1;
		}
		spec->keep_bottom = (unsigned int)number;
		*seen |= 1U << 5;
		return 0;
	}
	if (!strcmp(key, "merged") || !strcmp(key, "upper") ||
	    !strcmp(key, "work")) {
		char **field = !strcmp(key, "merged") ? &spec->merged :
			       !strcmp(key, "upper") ? &spec->upper :
			       &spec->work;

		if (parse_string(reader, &value) || set_string_once(field, value))
			return -1;
		*seen |= !strcmp(key, "merged") ? 1U << 6 :
			 !strcmp(key, "upper") ? 1U << 7 : 1U << 8;
		return 0;
	}
	if (!strcmp(key, "lower_prefix")) {
		if (*seen & (1U << 9) || parse_lower_prefix(reader, spec)) {
			errno = EINVAL;
			return -1;
		}
		*seen |= 1U << 9;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

static int parse_spec(char *data, size_t length, struct e1_spec *spec)
{
	struct json_reader reader = { .cursor = data, .end = data + length };
	unsigned int seen = 0;
	bool first = true;

	if (consume(&reader, '{'))
		return -1;
	for (;;) {
		char *key;

		skip_space(&reader);
		if (reader.cursor < reader.end && *reader.cursor == '}') {
			reader.cursor++;
			break;
		}
		if (!first && consume(&reader, ','))
			return -1;
		if (parse_string(&reader, &key))
			return -1;
		if (consume(&reader, ':') || parse_field(&reader, spec, &seen, key)) {
			free(key);
			return -1;
		}
		free(key);
		first = false;
	}
	skip_space(&reader);
	if (reader.cursor != reader.end || seen != 0x3ffU || spec->schema != 2 ||
	    (!spec->operation ||
	     (strcmp(spec->operation, "checkpoint") &&
	      strcmp(spec->operation, "restore"))) ||
	    spec->cpu < 0 || !spec->expected_generation ||
	    spec->source_depth > DELTAFS_V2_MAX_LOWERS ||
	    !spec->merged[0] || !spec->upper[0] || !spec->work[0] ||
	    (!strcmp(spec->operation, "checkpoint") &&
	     (spec->keep_bottom || spec->nr_lower_prefix)) ||
	    (!strcmp(spec->operation, "restore") &&
	     (spec->keep_bottom > spec->source_depth ||
	      spec->nr_lower_prefix >
		DELTAFS_V2_MAX_LOWERS - spec->keep_bottom ||
	      (!spec->keep_bottom && !spec->nr_lower_prefix)))) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int e1_validate_checkpoint_source_depth(unsigned int source_depth)
{
	if (!source_depth) {
		errno = EINVAL;
		return -1;
	}
	if (source_depth >= DELTAFS_V2_MAX_LOWERS) {
		errno = E2BIG;
		return -1;
	}
	return 0;
}

int e1_build_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *req,
				int upper_fd, int work_fd,
				uint64_t expected_generation)
{
	if (!req || !expected_generation) {
		errno = EINVAL;
		return -1;
	}
	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = expected_generation;
	req->upper_fd = upper_fd;
	req->work_fd = work_fd;
	return 0;
}

int e1_build_restore_request(struct deltafs_ioc_restore_v2 *req,
			     int upper_fd, int work_fd,
			     const int *lower_fds,
			     unsigned int nr_lower_prefix,
			     unsigned int keep_bottom,
			     uint64_t expected_generation)
{
	unsigned int i;

	if (!req || !expected_generation ||
	    keep_bottom > DELTAFS_V2_MAX_LOWERS ||
	    (!nr_lower_prefix && !keep_bottom)) {
		errno = EINVAL;
		return -1;
	}
	if (nr_lower_prefix > DELTAFS_V2_MAX_LOWERS - keep_bottom) {
		errno = E2BIG;
		return -1;
	}
	if (nr_lower_prefix && !lower_fds) {
		errno = EINVAL;
		return -1;
	}
	memset(req, 0, sizeof(*req));
	req->size = sizeof(*req);
	req->version = DELTAFS_ABI_VERSION;
	req->expected_generation = expected_generation;
	req->keep_bottom = keep_bottom;
	req->nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE + nr_lower_prefix;
	for (i = 0; i < DELTAFS_V2_MAX_RESTORE_FDS; i++)
		req->fds[i] = -1;
	req->fds[DELTAFS_V2_RESTORE_UPPER_FD] = upper_fd;
	req->fds[DELTAFS_V2_RESTORE_WORK_FD] = work_fd;
	for (i = 0; i < nr_lower_prefix; i++)
		req->fds[DELTAFS_V2_RESTORE_LOWER_BASE + i] = lower_fds[i];
	return 0;
}

int e1_read_spec(const char *path, struct e1_spec *spec)
{
	struct stat st;
	char *data = NULL;
	ssize_t count;
	size_t offset = 0;
	int fd = -1;
	int saved_errno;

	memset(spec, 0, sizeof(*spec));
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	if (fstat(fd, &st))
		goto fail;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    st.st_size > E1_SPEC_MAX_BYTES) {
		errno = EINVAL;
		goto fail;
	}
	data = malloc((size_t)st.st_size + 1);
	if (!data)
		goto fail;
	while (offset < (size_t)st.st_size) {
		count = read(fd, data + offset, (size_t)st.st_size - offset);
		if (count < 0) {
			if (errno == EINTR)
				continue;
			goto fail;
		}
		if (!count) {
			errno = EIO;
			goto fail;
		}
		offset += (size_t)count;
	}
	data[offset] = '\0';
	if (close(fd)) {
		fd = -1;
		goto fail;
	}
	fd = -1;
	if (parse_spec(data, offset, spec))
		goto fail;
	free(data);
	return 0;

fail:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	free(data);
	e1_free_spec(spec);
	errno = saved_errno;
	return -1;
}

void e1_free_spec(struct e1_spec *spec)
{
	size_t i;

	free(spec->operation);
	free(spec->merged);
	free(spec->upper);
	free(spec->work);
	for (i = 0; i < spec->nr_lower_prefix; i++)
		free(spec->lower_prefix[i]);
	free(spec->lower_prefix);
	memset(spec, 0, sizeof(*spec));
}

static int write_all(int fd, const char *data, size_t length)
{
	while (length) {
		ssize_t written = write(fd, data, length);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!written) {
			errno = EIO;
			return -1;
		}
		data += written;
		length -= (size_t)written;
	}
	return 0;
}

int e1_atomic_write(const char *path, const char *data, size_t length)
{
	char *copy = NULL;
	char *slash;
	char *temporary = NULL;
	const char *directory;
	int directory_fd = -1;
	int fd = -1;
	int saved_errno;
	int ret = -1;

	if (!path || path[0] != '/') {
		errno = EINVAL;
		return -1;
	}
	copy = strdup(path);
	if (!copy)
		goto out;
	slash = strrchr(copy, '/');
	if (!slash || !slash[1]) {
		errno = EINVAL;
		goto out;
	}
	*slash = '\0';
	directory = copy[0] ? copy : "/";
	if (asprintf(&temporary, "%s.tmp.%ld", path, (long)getpid()) < 0) {
		temporary = NULL;
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
	directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (directory_fd < 0 || fsync(directory_fd))
		goto out;
	ret = 0;
out:
	saved_errno = errno;
	if (fd >= 0)
		close(fd);
	if (directory_fd >= 0)
		close(directory_fd);
	if (ret && temporary)
		unlink(temporary);
	free(temporary);
	free(copy);
	errno = saved_errno;
	return ret;
}
