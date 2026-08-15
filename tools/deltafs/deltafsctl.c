// SPDX-License-Identifier: GPL-2.0
/*
 * DeltaFS v2 checkpoint/restore controller.
 *
 * The controller deliberately implements only the two runtime state
 * transitions exposed by the DeltaFS UAPI.  Sandbox creation, mounting and
 * workload quiescing belong to the caller.  A durable transaction intent is
 * installed before any backing-tree mutation.  Without a GET_STATE ioctl an
 * interrupted transaction is ambiguous, so a later invocation fails closed.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/deltafs.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DELTAFSCTL_STATE_FORMAT		2
#define DELTAFSCTL_MAX_STATE_SIZE	(1024U * 1024U)
#define DELTAFSCTL_MAX_SNAPSHOTS	1024U
#define DELTAFSCTL_MAX_RETIRED		4096U

#define STATE_FILE		"state.json"
#define STATE_TMP_FILE		".state.json.tmp"
#define TRANSACTION_FILE	"transaction.json"
#define TRANSACTION_TMP_FILE	".transaction.json.tmp"
#define LOCK_FILE		"controller.lock"

enum operation {
	OP_CHECKPOINT,
	OP_RESTORE,
};

struct strvec {
	char **items;
	size_t nr;
	size_t cap;
};

struct snapshot {
	char *id;
	struct strvec lowers;
};

struct controller_state {
	uint64_t generation;
	char *active_branch;
	struct strvec active_lowers;
	struct strvec retired_branches;
	struct snapshot *snapshots;
	size_t nr_snapshots;
	size_t snapshots_cap;
};

struct text {
	char *data;
	size_t len;
	size_t cap;
};

struct json_parser {
	const char *data;
	size_t len;
	size_t pos;
	char error[192];
};

struct controller {
	int root_fd;
	int meta_fd;
	int branches_fd;
	int layers_fd;
	int merged_fd;
	int lock_fd;
};

struct switch_fds {
	int branch_fd;
	int upper_fd;
	int work_fd;
	int lower_fds[DELTAFS_V2_MAX_LOWERS];
	unsigned int nr_lower;
};

static const char *program_name = "deltafsctl";

static void report(const char *prefix, const char *fmt, va_list ap)
{
	fprintf(stderr, "%s: %s: ", program_name, prefix);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

static void error_msg(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	report("error", fmt, ap);
	va_end(ap);
}

static void error_errno(const char *fmt, ...)
{
	int saved_errno = errno;
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: error: ", program_name);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ": %s\n", strerror(saved_errno));
	va_end(ap);
}

static void *xreallocarray(void *ptr, size_t nmemb, size_t size)
{
	if (size && nmemb > SIZE_MAX / size) {
		errno = ENOMEM;
		return NULL;
	}
	return realloc(ptr, nmemb * size);
}

static void strvec_free(struct strvec *vec)
{
	size_t i;

	for (i = 0; i < vec->nr; i++)
		free(vec->items[i]);
	free(vec->items);
	memset(vec, 0, sizeof(*vec));
}

static int strvec_reserve(struct strvec *vec, size_t need)
{
	char **items;
	size_t cap;

	if (need <= vec->cap)
		return 0;
	cap = vec->cap ? vec->cap : 4;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			errno = ENOMEM;
			return -1;
		}
		cap *= 2;
	}
	items = xreallocarray(vec->items, cap, sizeof(*items));
	if (!items)
		return -1;
	vec->items = items;
	vec->cap = cap;
	return 0;
}

static int strvec_push_owned(struct strvec *vec, char *item)
{
	if (strvec_reserve(vec, vec->nr + 1))
		return -1;
	vec->items[vec->nr++] = item;
	return 0;
}

static int strvec_push(struct strvec *vec, const char *item)
{
	char *copy = strdup(item);

	if (!copy)
		return -1;
	if (strvec_push_owned(vec, copy)) {
		free(copy);
		return -1;
	}
	return 0;
}

static int strvec_clone(struct strvec *dst, const struct strvec *src)
{
	size_t i;

	for (i = 0; i < src->nr; i++) {
		if (strvec_push(dst, src->items[i])) {
			strvec_free(dst);
			return -1;
		}
	}
	return 0;
}

static bool strvec_equal(const struct strvec *a, const struct strvec *b)
{
	size_t i;

	if (a->nr != b->nr)
		return false;
	for (i = 0; i < a->nr; i++) {
		if (strcmp(a->items[i], b->items[i]))
			return false;
	}
	return true;
}

static void state_free(struct controller_state *state)
{
	size_t i;

	if (!state)
		return;
	free(state->active_branch);
	strvec_free(&state->active_lowers);
	strvec_free(&state->retired_branches);
	for (i = 0; i < state->nr_snapshots; i++) {
		free(state->snapshots[i].id);
		strvec_free(&state->snapshots[i].lowers);
	}
	free(state->snapshots);
	free(state);
}

static struct snapshot *state_find_snapshot(struct controller_state *state,
					     const char *id)
{
	size_t i;

	for (i = 0; i < state->nr_snapshots; i++) {
		if (!strcmp(state->snapshots[i].id, id))
			return &state->snapshots[i];
	}
	return NULL;
}

static const struct snapshot *
state_find_snapshot_const(const struct controller_state *state, const char *id)
{
	size_t i;

	for (i = 0; i < state->nr_snapshots; i++) {
		if (!strcmp(state->snapshots[i].id, id))
			return &state->snapshots[i];
	}
	return NULL;
}

static size_t state_snapshot_index(const struct controller_state *state,
				   const struct snapshot *snapshot)
{
	size_t i;

	for (i = 0; i < state->nr_snapshots; i++) {
		if (&state->snapshots[i] == snapshot)
			return i;
	}
	return SIZE_MAX;
}

static int state_reserve_snapshots(struct controller_state *state, size_t need)
{
	struct snapshot *snapshots;
	size_t cap;

	if (need <= state->snapshots_cap)
		return 0;
	cap = state->snapshots_cap ? state->snapshots_cap : 4;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			errno = ENOMEM;
			return -1;
		}
		cap *= 2;
	}
	snapshots = xreallocarray(state->snapshots, cap, sizeof(*snapshots));
	if (!snapshots)
		return -1;
	memset(snapshots + state->snapshots_cap, 0,
	       (cap - state->snapshots_cap) * sizeof(*snapshots));
	state->snapshots = snapshots;
	state->snapshots_cap = cap;
	return 0;
}

static int state_add_snapshot_owned(struct controller_state *state, char *id,
				    struct strvec *lowers)
{
	struct snapshot *snapshot;

	if (state->nr_snapshots >= DELTAFSCTL_MAX_SNAPSHOTS) {
		errno = E2BIG;
		return -1;
	}
	if (state_reserve_snapshots(state, state->nr_snapshots + 1))
		return -1;
	snapshot = &state->snapshots[state->nr_snapshots++];
	snapshot->id = id;
	snapshot->lowers = *lowers;
	memset(lowers, 0, sizeof(*lowers));
	return 0;
}

static struct controller_state *state_clone(const struct controller_state *src)
{
	struct controller_state *dst;
	size_t i;

	dst = calloc(1, sizeof(*dst));
	if (!dst)
		return NULL;
	dst->generation = src->generation;
	dst->active_branch = strdup(src->active_branch);
	if (!dst->active_branch ||
	    strvec_clone(&dst->active_lowers, &src->active_lowers) ||
	    strvec_clone(&dst->retired_branches, &src->retired_branches))
		goto out_err;
	for (i = 0; i < src->nr_snapshots; i++) {
		struct strvec lowers = { };
		char *id;

		id = strdup(src->snapshots[i].id);
		if (!id || strvec_clone(&lowers, &src->snapshots[i].lowers)) {
			free(id);
			strvec_free(&lowers);
			goto out_err;
		}
		if (state_add_snapshot_owned(dst, id, &lowers)) {
			free(id);
			strvec_free(&lowers);
			goto out_err;
		}
	}
	return dst;

out_err:
	state_free(dst);
	return NULL;
}

static int text_reserve(struct text *text, size_t extra)
{
	char *data;
	size_t need, cap;

	if (extra > SIZE_MAX - text->len - 1) {
		errno = ENOMEM;
		return -1;
	}
	need = text->len + extra + 1;
	if (need <= text->cap)
		return 0;
	cap = text->cap ? text->cap : 256;
	while (cap < need) {
		if (cap > SIZE_MAX / 2) {
			cap = need;
			break;
		}
		cap *= 2;
	}
	data = realloc(text->data, cap);
	if (!data)
		return -1;
	text->data = data;
	text->cap = cap;
	return 0;
}

static int text_append_n(struct text *text, const char *data, size_t len)
{
	if (text_reserve(text, len))
		return -1;
	memcpy(text->data + text->len, data, len);
	text->len += len;
	text->data[text->len] = '\0';
	return 0;
}

static int text_append(struct text *text, const char *data)
{
	return text_append_n(text, data, strlen(data));
}

static int text_appendf(struct text *text, const char *fmt, ...)
{
	va_list ap, copy;
	int len;

	va_start(ap, fmt);
	va_copy(copy, ap);
	len = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (len < 0 || text_reserve(text, (size_t)len)) {
		va_end(ap);
		return -1;
	}
	vsnprintf(text->data + text->len, text->cap - text->len, fmt, ap);
	va_end(ap);
	text->len += (size_t)len;
	return 0;
}

static void text_free(struct text *text)
{
	free(text->data);
	memset(text, 0, sizeof(*text));
}

static void json_error(struct json_parser *parser, const char *fmt, ...)
{
	va_list ap;
	size_t used;

	if (parser->error[0])
		return;
	used = (size_t)snprintf(parser->error, sizeof(parser->error),
				"byte %zu: ", parser->pos);
	if (used >= sizeof(parser->error))
		return;
	va_start(ap, fmt);
	vsnprintf(parser->error + used, sizeof(parser->error) - used, fmt, ap);
	va_end(ap);
}

static void json_skip_ws(struct json_parser *parser)
{
	while (parser->pos < parser->len) {
		char ch = parser->data[parser->pos];

		if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
			break;
		parser->pos++;
	}
}

static bool json_take(struct json_parser *parser, char expected)
{
	json_skip_ws(parser);
	if (parser->pos < parser->len &&
	    parser->data[parser->pos] == expected) {
		parser->pos++;
		return true;
	}
	return false;
}

static int json_expect(struct json_parser *parser, char expected)
{
	if (json_take(parser, expected))
		return 0;
	json_error(parser, "expected '%c'", expected);
	return -1;
}

static int json_string(struct json_parser *parser, char **result)
{
	size_t start, len;
	char *value;

	json_skip_ws(parser);
	if (parser->pos >= parser->len || parser->data[parser->pos] != '"') {
		json_error(parser, "expected string");
		return -1;
	}
	parser->pos++;
	start = parser->pos;
	while (parser->pos < parser->len) {
		unsigned char ch = (unsigned char)parser->data[parser->pos];

		if (ch == '"')
			break;
		if (ch == '\\') {
			json_error(parser,
				   "escaped strings are not permitted in state metadata");
			return -1;
		}
		if (ch < 0x20 || ch >= 0x7f) {
			json_error(parser, "state strings must be printable ASCII");
			return -1;
		}
		parser->pos++;
	}
	if (parser->pos == parser->len) {
		json_error(parser, "unterminated string");
		return -1;
	}
	len = parser->pos - start;
	if (len > PATH_MAX) {
		json_error(parser, "string exceeds PATH_MAX");
		return -1;
	}
	value = strndup(parser->data + start, len);
	if (!value)
		return -1;
	parser->pos++;
	*result = value;
	return 0;
}

static int json_u64(struct json_parser *parser, uint64_t *result)
{
	uint64_t value = 0;
	size_t start;

	json_skip_ws(parser);
	start = parser->pos;
	if (start >= parser->len || parser->data[start] < '0' ||
	    parser->data[start] > '9') {
		json_error(parser, "expected unsigned integer");
		return -1;
	}
	if (parser->data[start] == '0' && start + 1 < parser->len &&
	    parser->data[start + 1] >= '0' && parser->data[start + 1] <= '9') {
		json_error(parser, "leading zero in integer");
		return -1;
	}
	while (parser->pos < parser->len) {
		unsigned int digit;
		char ch = parser->data[parser->pos];

		if (ch < '0' || ch > '9')
			break;
		digit = (unsigned int)(ch - '0');
		if (value > (UINT64_MAX - digit) / 10) {
			json_error(parser, "integer overflow");
			return -1;
		}
		value = value * 10 + digit;
		parser->pos++;
	}
	*result = value;
	return 0;
}

static int json_string_array(struct json_parser *parser, struct strvec *vec,
			     size_t max_items)
{
	if (json_expect(parser, '['))
		return -1;
	if (json_take(parser, ']'))
		return 0;
	for (;;) {
		char *item = NULL;

		if (vec->nr >= max_items) {
			json_error(parser, "array has more than %zu entries", max_items);
			return -1;
		}
		if (json_string(parser, &item))
			return -1;
		if (strvec_push_owned(vec, item)) {
			free(item);
			return -1;
		}
		if (json_take(parser, ']'))
			return 0;
		if (json_expect(parser, ','))
			return -1;
	}
}

static int json_snapshot(struct json_parser *parser, struct strvec *lowers)
{
	bool seen_lowers = false;

	if (json_expect(parser, '{'))
		return -1;
	if (json_take(parser, '}')) {
		json_error(parser, "snapshot object is missing lowers");
		return -1;
	}
	for (;;) {
		char *key = NULL;
		int err = -1;

		if (json_string(parser, &key) || json_expect(parser, ':'))
			goto out;
		if (!strcmp(key, "lowers")) {
			if (seen_lowers) {
				json_error(parser, "duplicate snapshot lowers");
				goto out;
			}
			seen_lowers = true;
			if (json_string_array(parser, lowers,
					      DELTAFS_V2_MAX_LOWERS))
				goto out;
		} else {
			json_error(parser, "unknown snapshot field '%s'", key);
			goto out;
		}
		err = 0;
out:
		free(key);
		if (err)
			return -1;
		if (json_take(parser, '}'))
			break;
		if (json_expect(parser, ','))
			return -1;
	}
	if (!seen_lowers) {
		json_error(parser, "snapshot object is missing lowers");
		return -1;
	}
	return 0;
}

static int json_snapshots(struct json_parser *parser,
			  struct controller_state *state)
{
	if (json_expect(parser, '{'))
		return -1;
	if (json_take(parser, '}'))
		return 0;
	for (;;) {
		struct strvec lowers = { };
		char *id = NULL;

		if (state->nr_snapshots >= DELTAFSCTL_MAX_SNAPSHOTS) {
			json_error(parser, "too many snapshots");
			return -1;
		}
		if (json_string(parser, &id) || json_expect(parser, ':'))
			goto out_err;
		if (state_find_snapshot(state, id)) {
			json_error(parser, "duplicate snapshot '%s'", id);
			goto out_err;
		}
		if (json_snapshot(parser, &lowers))
			goto out_err;
		if (state_add_snapshot_owned(state, id, &lowers))
			goto out_err;
		id = NULL;
		if (json_take(parser, '}'))
			return 0;
		if (json_expect(parser, ','))
			return -1;
		continue;

out_err:
		free(id);
		strvec_free(&lowers);
		return -1;
	}
}

static struct controller_state *json_parse_state(const char *data, size_t len,
						  char *error, size_t error_len)
{
	struct json_parser parser = {
		.data = data,
		.len = len,
	};
	struct controller_state *state;
	bool seen_format = false;
	bool seen_generation = false;
	bool seen_active_branch = false;
	bool seen_active_lowers = false;
	bool seen_retired = false;
	bool seen_snapshots = false;
	uint64_t format = 0;

	state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;
	if (json_expect(&parser, '{'))
		goto out_err;
	if (json_take(&parser, '}')) {
		json_error(&parser, "empty state object");
		goto out_err;
	}
	for (;;) {
		char *key = NULL;
		int err = -1;

		if (json_string(&parser, &key) || json_expect(&parser, ':'))
			goto field_out;
		if (!strcmp(key, "format")) {
			if (seen_format) {
				json_error(&parser, "duplicate format");
				goto field_out;
			}
			seen_format = true;
			if (json_u64(&parser, &format))
				goto field_out;
		} else if (!strcmp(key, "kernel_generation")) {
			if (seen_generation) {
				json_error(&parser, "duplicate kernel_generation");
				goto field_out;
			}
			seen_generation = true;
			if (json_u64(&parser, &state->generation))
				goto field_out;
		} else if (!strcmp(key, "active_branch")) {
			if (seen_active_branch) {
				json_error(&parser, "duplicate active_branch");
				goto field_out;
			}
			seen_active_branch = true;
			if (json_string(&parser, &state->active_branch))
				goto field_out;
		} else if (!strcmp(key, "active_lowers")) {
			if (seen_active_lowers) {
				json_error(&parser, "duplicate active_lowers");
				goto field_out;
			}
			seen_active_lowers = true;
			if (json_string_array(&parser, &state->active_lowers,
					      DELTAFS_V2_MAX_LOWERS))
				goto field_out;
		} else if (!strcmp(key, "retired_branches")) {
			if (seen_retired) {
				json_error(&parser, "duplicate retired_branches");
				goto field_out;
			}
			seen_retired = true;
			if (json_string_array(&parser, &state->retired_branches,
					      DELTAFSCTL_MAX_RETIRED))
				goto field_out;
		} else if (!strcmp(key, "snapshots")) {
			if (seen_snapshots) {
				json_error(&parser, "duplicate snapshots");
				goto field_out;
			}
			seen_snapshots = true;
			if (json_snapshots(&parser, state))
				goto field_out;
		} else {
			json_error(&parser, "unknown state field '%s'", key);
			goto field_out;
		}
		err = 0;

field_out:
		free(key);
		if (err)
			goto out_err;
		if (json_take(&parser, '}'))
			break;
		if (json_expect(&parser, ','))
			goto out_err;
	}
	json_skip_ws(&parser);
	if (parser.pos != parser.len) {
		json_error(&parser, "trailing content after state object");
		goto out_err;
	}
	if (!seen_format || !seen_generation || !seen_active_branch ||
	    !seen_active_lowers || !seen_retired || !seen_snapshots) {
		json_error(&parser, "state object is missing required fields");
		goto out_err;
	}
	if (format != DELTAFSCTL_STATE_FORMAT) {
		json_error(&parser, "unsupported state format %" PRIu64, format);
		goto out_err;
	}
	return state;

out_err:
	if (error && error_len) {
		if (parser.error[0])
			snprintf(error, error_len, "%s", parser.error);
		else
			snprintf(error, error_len, "%s", strerror(errno));
	}
	state_free(state);
	return NULL;
}

static bool valid_component(const char *name)
{
	size_t i, len = strlen(name);

	if (!len || len > NAME_MAX || !strcmp(name, ".") || !strcmp(name, ".."))
		return false;
	for (i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)name[i];

		if ((ch >= 'a' && ch <= 'z') ||
		    (ch >= 'A' && ch <= 'Z') ||
		    (ch >= '0' && ch <= '9') ||
		    ch == '.' || ch == '_' || ch == '-')
			continue;
		return false;
	}
	return true;
}

static const char *lower_snapshot_id(const char *path)
{
	const char prefix[] = "layers/";
	const char *id;

	if (strncmp(path, prefix, sizeof(prefix) - 1))
		return NULL;
	id = path + sizeof(prefix) - 1;
	if (!valid_component(id) || strchr(id, '/'))
		return NULL;
	return id;
}

static bool valid_lower_path(const char *path)
{
	return !strcmp(path, "base") || lower_snapshot_id(path);
}

static int state_semantic_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "%s: error: invalid %s: ", program_name, STATE_FILE);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	errno = EINVAL;
	return -1;
}

static int validate_lower_chain(const struct strvec *lowers, const char *what)
{
	size_t i, j;

	if (!lowers->nr || lowers->nr > DELTAFS_V2_MAX_LOWERS)
		return state_semantic_error("%s lower count is outside [1, %u]",
					    what, DELTAFS_V2_MAX_LOWERS);
	if (strcmp(lowers->items[lowers->nr - 1], "base"))
		return state_semantic_error("%s lower chain does not end in base",
					    what);
	for (i = 0; i < lowers->nr; i++) {
		if (!valid_lower_path(lowers->items[i]))
			return state_semantic_error("%s contains invalid path '%s'",
						    what, lowers->items[i]);
		for (j = i + 1; j < lowers->nr; j++) {
			if (!strcmp(lowers->items[i], lowers->items[j]))
				return state_semantic_error(
					"%s contains duplicate path '%s'",
					what, lowers->items[i]);
		}
	}
	return 0;
}

static int validate_snapshot_chain(const struct controller_state *state,
				   const struct snapshot *snapshot)
{
	const struct snapshot *parent;
	const char *parent_id;
	char expected[NAME_MAX + sizeof("layers/")];
	struct strvec suffix;
	size_t snapshot_index;
	size_t parent_index;

	if (!valid_component(snapshot->id))
		return state_semantic_error("invalid snapshot id '%s'",
					    snapshot->id);
	if (validate_lower_chain(&snapshot->lowers, snapshot->id))
		return -1;
	if (snprintf(expected, sizeof(expected), "layers/%s", snapshot->id) >=
	    (int)sizeof(expected))
		return state_semantic_error("snapshot id '%s' is too long",
					    snapshot->id);
	if (strcmp(snapshot->lowers.items[0], expected))
		return state_semantic_error(
			"snapshot '%s' does not start with its own layer",
			snapshot->id);
	if (snapshot->lowers.nr == 2 &&
	    !strcmp(snapshot->lowers.items[1], "base"))
		return 0;
	if (snapshot->lowers.nr < 2)
		return state_semantic_error("snapshot '%s' has no parent lower",
					    snapshot->id);
	parent_id = lower_snapshot_id(snapshot->lowers.items[1]);
	if (!parent_id)
		return state_semantic_error(
			"snapshot '%s' has a non-snapshot parent", snapshot->id);
	parent = state_find_snapshot_const(state, parent_id);
	if (!parent)
		return state_semantic_error(
			"snapshot '%s' references unknown parent '%s'",
			snapshot->id, parent_id);
	snapshot_index = state_snapshot_index(state, snapshot);
	parent_index = state_snapshot_index(state, parent);
	if (parent_index == SIZE_MAX || snapshot_index == SIZE_MAX ||
	    parent_index >= snapshot_index)
		return state_semantic_error(
			"snapshot '%s' must appear after parent '%s'",
			snapshot->id, parent_id);
	suffix.items = &snapshot->lowers.items[1];
	suffix.nr = snapshot->lowers.nr - 1;
	suffix.cap = suffix.nr;
	if (!strvec_equal(&suffix, &parent->lowers))
		return state_semantic_error(
			"snapshot '%s' does not contain parent '%s' chain verbatim",
			snapshot->id, parent_id);
	return 0;
}

static int validate_state(const struct controller_state *state)
{
	const struct snapshot *active_snapshot;
	const char *active_snapshot_id;
	char expected_branch[64];
	size_t i, j;

	if (!state->generation)
		return state_semantic_error("kernel_generation must be nonzero");
	if (snprintf(expected_branch, sizeof(expected_branch), "g%" PRIu64,
		     state->generation) >= (int)sizeof(expected_branch))
		return state_semantic_error("kernel_generation is too large");
	if (!valid_component(state->active_branch) ||
	    strcmp(state->active_branch, expected_branch))
		return state_semantic_error(
			"active_branch must be '%s' for generation %" PRIu64,
			expected_branch, state->generation);
	if (validate_lower_chain(&state->active_lowers, "active"))
		return -1;
	if (state->retired_branches.nr > DELTAFSCTL_MAX_RETIRED)
		return state_semantic_error("too many retired branches");
	for (i = 0; i < state->retired_branches.nr; i++) {
		const char *branch = state->retired_branches.items[i];

		if (!valid_component(branch) ||
		    branch[0] != 'g' || !branch[1])
			return state_semantic_error("invalid retired branch '%s'",
						    branch);
		if (!strcmp(branch, state->active_branch))
			return state_semantic_error(
				"active branch also appears in retired_branches");
		for (j = i + 1; j < state->retired_branches.nr; j++) {
			if (!strcmp(branch, state->retired_branches.items[j]))
				return state_semantic_error(
					"duplicate retired branch '%s'", branch);
		}
	}
	for (i = 0; i < state->nr_snapshots; i++) {
		if (validate_snapshot_chain(state, &state->snapshots[i]))
			return -1;
	}
	if (state->active_lowers.nr == 1 &&
	    !strcmp(state->active_lowers.items[0], "base"))
		return 0;
	active_snapshot_id = lower_snapshot_id(state->active_lowers.items[0]);
	if (!active_snapshot_id)
		return state_semantic_error(
			"active lower chain does not start with a snapshot");
	active_snapshot = state_find_snapshot_const(state, active_snapshot_id);
	if (!active_snapshot ||
	    !strvec_equal(&state->active_lowers, &active_snapshot->lowers))
		return state_semantic_error(
			"active lower chain is not an exact committed snapshot");
	return 0;
}

static int text_json_string(struct text *text, const char *value)
{
	/*
	 * Every persisted string has already passed the printable-ASCII and
	 * component/path validators, so it cannot require JSON escaping.
	 */
	return text_append(text, "\"") || text_append(text, value) ||
	       text_append(text, "\"");
}

static int serialize_string_array(struct text *text, const struct strvec *vec,
				  unsigned int indent)
{
	size_t i;

	if (text_append(text, "["))
		return -1;
	for (i = 0; i < vec->nr; i++) {
		if (i && text_append(text, ", "))
			return -1;
		if (text_json_string(text, vec->items[i]))
			return -1;
	}
	if (text_append(text, "]"))
		return -1;
	(void)indent;
	return 0;
}

static int serialize_state(const struct controller_state *state,
			   struct text *text)
{
	size_t i;

	if (text_appendf(text,
			 "{\n"
			 "  \"format\": %u,\n"
			 "  \"kernel_generation\": %" PRIu64 ",\n"
			 "  \"active_branch\": ",
			 DELTAFSCTL_STATE_FORMAT, state->generation) ||
	    text_json_string(text, state->active_branch) ||
	    text_append(text, ",\n  \"active_lowers\": ") ||
	    serialize_string_array(text, &state->active_lowers, 2) ||
	    text_append(text, ",\n  \"retired_branches\": ") ||
	    serialize_string_array(text, &state->retired_branches, 2) ||
	    text_append(text, ",\n  \"snapshots\": {"))
		return -1;
	for (i = 0; i < state->nr_snapshots; i++) {
		const struct snapshot *snapshot = &state->snapshots[i];

		if (text_append(text, i ? ",\n    " : "\n    ") ||
		    text_json_string(text, snapshot->id) ||
		    text_append(text, ": {\"lowers\": ") ||
		    serialize_string_array(text, &snapshot->lowers, 4) ||
		    text_append(text, "}"))
			return -1;
	}
	if (state->nr_snapshots && text_append(text, "\n  "))
		return -1;
	if (text_append(text, "}\n}\n"))
		return -1;
	if (text->len > DELTAFSCTL_MAX_STATE_SIZE) {
		errno = E2BIG;
		return -1;
	}
	return 0;
}

static const char *operation_name(enum operation operation)
{
	return operation == OP_CHECKPOINT ? "checkpoint" : "restore";
}

static int serialize_transaction(enum operation operation,
				 uint64_t from_generation,
				 const char *from_branch,
				 const char *new_branch,
				 const char *snapshot_id,
				 const struct strvec *target_lowers,
				 const struct strvec *prefix_lowers,
				 size_t keep_bottom,
				 struct text *text)
{
	if (text_appendf(text,
			 "{\n"
			 "  \"format\": %u,\n"
			 "  \"operation\": \"%s\",\n"
			 "  \"phase\": \"prepared\",\n"
			 "  \"from_generation\": %" PRIu64 ",\n"
			 "  \"to_generation\": %" PRIu64 ",\n"
			 "  \"from_branch\": ",
			 DELTAFSCTL_STATE_FORMAT, operation_name(operation),
			 from_generation, from_generation + 1) ||
	    text_json_string(text, from_branch) ||
	    text_append(text, ",\n  \"new_branch\": ") ||
	    text_json_string(text, new_branch) ||
	    text_append(text, ",\n  \"snapshot\": ") ||
	    text_json_string(text, snapshot_id) ||
	    text_appendf(text, ",\n  \"keep_bottom\": %zu,\n"
			      "  \"new_lower_prefix\": ", keep_bottom) ||
	    serialize_string_array(text, prefix_lowers, 2) ||
	    text_append(text, ",\n  \"target_lowers\": ") ||
	    serialize_string_array(text, target_lowers, 2) ||
	    text_append(text, "\n}\n"))
		return -1;
	return 0;
}

static int build_target_lowers(enum operation operation,
			       const struct controller_state *state,
			       const char *snapshot_id, struct strvec *target)
{
	const struct snapshot *snapshot;
	char layer[NAME_MAX + sizeof("layers/")];
	size_t i;

	if (operation == OP_RESTORE) {
		snapshot = state_find_snapshot_const(state, snapshot_id);
		if (!snapshot) {
			error_msg("unknown checkpoint '%s'", snapshot_id);
			errno = ENOENT;
			return -1;
		}
		return strvec_clone(target, &snapshot->lowers);
	}

	if (state_find_snapshot_const(state, snapshot_id)) {
		error_msg("checkpoint '%s' already exists", snapshot_id);
		errno = EEXIST;
		return -1;
	}
	if (state->active_lowers.nr >= DELTAFS_V2_MAX_LOWERS) {
		error_msg("checkpoint would exceed the %u-lower v2 limit",
			  DELTAFS_V2_MAX_LOWERS);
		errno = E2BIG;
		return -1;
	}
	if (snprintf(layer, sizeof(layer), "layers/%s", snapshot_id) >=
	    (int)sizeof(layer)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (strvec_push(target, layer))
		return -1;
	for (i = 0; i < state->active_lowers.nr; i++) {
		if (strvec_push(target, state->active_lowers.items[i]))
			return -1;
	}
	return 0;
}

/* Return the number of lower layers shared verbatim at the bottom. */
static size_t longest_common_suffix(const struct strvec *active,
				    const struct strvec *target)
{
	size_t shared = 0;

	while (shared < active->nr && shared < target->nr &&
	       !strcmp(active->items[active->nr - 1 - shared],
		       target->items[target->nr - 1 - shared]))
		shared++;
	return shared;
}

static int build_restore_prefix(const struct strvec *target,
				size_t keep_bottom, struct strvec *prefix)
{
	size_t i, nr_prefix;

	if (keep_bottom > target->nr) {
		errno = EINVAL;
		return -1;
	}
	nr_prefix = target->nr - keep_bottom;
	for (i = 0; i < nr_prefix; i++) {
		if (strvec_push(prefix, target->items[i]))
			return -1;
	}
	return 0;
}

static int apply_transition(enum operation operation,
			    struct controller_state *state,
			    const char *snapshot_id,
			    const char *new_branch,
			    const struct strvec *target_lowers)
{
	struct strvec new_active = { };
	char *active_branch = NULL;

	if (state->retired_branches.nr >= DELTAFSCTL_MAX_RETIRED) {
		errno = E2BIG;
		return -1;
	}
	active_branch = strdup(new_branch);
	if (!active_branch ||
	    strvec_clone(&new_active, target_lowers) ||
	    strvec_push(&state->retired_branches, state->active_branch))
		goto out_err;

	if (operation == OP_CHECKPOINT) {
		struct strvec snapshot_lowers = { };
		char *id;

		id = strdup(snapshot_id);
		if (!id || strvec_clone(&snapshot_lowers, target_lowers)) {
			free(id);
			strvec_free(&snapshot_lowers);
			goto out_err;
		}
		if (state_add_snapshot_owned(state, id, &snapshot_lowers)) {
			free(id);
			strvec_free(&snapshot_lowers);
			goto out_err;
		}
	}
	free(state->active_branch);
	state->active_branch = active_branch;
	active_branch = NULL;
	strvec_free(&state->active_lowers);
	state->active_lowers = new_active;
	memset(&new_active, 0, sizeof(new_active));
	state->generation++;
	return 0;

out_err:
	free(active_branch);
	strvec_free(&new_active);
	return -1;
}

static int close_preserve_errno(int fd)
{
	int saved_errno = errno;
	int ret = 0;

	if (fd >= 0)
		ret = close(fd);
	errno = saved_errno;
	return ret;
}

static int open_dir_at(int parent_fd, const char *name, int extra_flags)
{
	return openat(parent_fd, name,
		      O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW | extra_flags);
}

static bool path_exists_at(int parent_fd, const char *name)
{
	struct stat st;

	if (!fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW))
		return true;
	return errno != ENOENT;
}

static int require_absent_at(int parent_fd, const char *name,
			     const char *description)
{
	struct stat st;

	if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
		if (errno == ENOENT)
			return 0;
		error_errno("inspect %s", description);
		return -1;
	}
	error_msg("%s exists; refusing an ambiguous transaction", description);
	errno = EBUSY;
	return -1;
}

static int write_all(int fd, const char *data, size_t len)
{
	size_t done = 0;

	while (done < len) {
		ssize_t ret = write(fd, data + done, len - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!ret) {
			errno = EIO;
			return -1;
		}
		done += (size_t)ret;
	}
	return 0;
}

static int read_state_file(int meta_fd, struct controller_state **statep)
{
	struct controller_state *state = NULL;
	struct stat st;
	char error[256] = { };
	char *data = NULL;
	size_t done = 0;
	int fd = -1;
	int ret = -1;

	fd = openat(meta_fd, STATE_FILE,
		    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		error_errno("open %s", STATE_FILE);
		goto out;
	}
	if (fstat(fd, &st)) {
		error_errno("stat %s", STATE_FILE);
		goto out;
	}
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
	    (uint64_t)st.st_size > DELTAFSCTL_MAX_STATE_SIZE) {
		error_msg("%s must be a nonempty regular file no larger than %u bytes",
			  STATE_FILE, DELTAFSCTL_MAX_STATE_SIZE);
		errno = EINVAL;
		goto out;
	}
	data = malloc((size_t)st.st_size + 1);
	if (!data)
		goto out;
	while (done < (size_t)st.st_size) {
		ssize_t nr = read(fd, data + done, (size_t)st.st_size - done);

		if (nr < 0) {
			if (errno == EINTR)
				continue;
			error_errno("read %s", STATE_FILE);
			goto out;
		}
		if (!nr) {
			error_msg("%s was truncated while reading", STATE_FILE);
			errno = EIO;
			goto out;
		}
		done += (size_t)nr;
	}
	data[done] = '\0';
	errno = 0;
	state = json_parse_state(data, done, error, sizeof(error));
	if (!state) {
		if (errno == ENOMEM) {
			error_errno("parse %s", STATE_FILE);
		} else if (error[0]) {
			error_msg("parse %s: %s", STATE_FILE, error);
			errno = EINVAL;
		} else {
			error_errno("parse %s", STATE_FILE);
			if (!errno)
				errno = EINVAL;
		}
		goto out;
	}
	if (validate_state(state))
		goto out;
	*statep = state;
	state = NULL;
	ret = 0;
out:
	close_preserve_errno(fd);
	free(data);
	state_free(state);
	return ret;
}

static void controller_init(struct controller *controller)
{
	controller->root_fd = -1;
	controller->meta_fd = -1;
	controller->branches_fd = -1;
	controller->layers_fd = -1;
	controller->merged_fd = -1;
	controller->lock_fd = -1;
}

static void controller_close(struct controller *controller)
{
	if (controller->lock_fd >= 0)
		flock(controller->lock_fd, LOCK_UN);
	close_preserve_errno(controller->merged_fd);
	close_preserve_errno(controller->layers_fd);
	close_preserve_errno(controller->branches_fd);
	close_preserve_errno(controller->lock_fd);
	close_preserve_errno(controller->meta_fd);
	close_preserve_errno(controller->root_fd);
	controller_init(controller);
}

static int controller_open(struct controller *controller,
			   const char *sandbox_root)
{
	struct stat st;

	controller->root_fd =
		open(sandbox_root, O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (controller->root_fd < 0) {
		error_errno("open sandbox root %s", sandbox_root);
		return -1;
	}
	controller->meta_fd = open_dir_at(controller->root_fd, "meta", O_RDONLY);
	if (controller->meta_fd < 0) {
		error_errno("open meta directory");
		return -1;
	}
	controller->lock_fd =
		openat(controller->meta_fd, LOCK_FILE,
		       O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (controller->lock_fd < 0) {
		error_errno("open %s", LOCK_FILE);
		return -1;
	}
	if (fstat(controller->lock_fd, &st)) {
		error_errno("validate %s", LOCK_FILE);
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		errno = EINVAL;
		error_errno("validate %s", LOCK_FILE);
		return -1;
	}
	if (flock(controller->lock_fd, LOCK_EX)) {
		error_errno("lock %s", LOCK_FILE);
		return -1;
	}
	if (require_absent_at(controller->meta_fd, TRANSACTION_FILE,
			      TRANSACTION_FILE) ||
	    require_absent_at(controller->meta_fd, TRANSACTION_TMP_FILE,
			      TRANSACTION_TMP_FILE) ||
	    require_absent_at(controller->meta_fd, STATE_TMP_FILE,
			      STATE_TMP_FILE))
		return -1;

	controller->branches_fd =
		open_dir_at(controller->root_fd, "branches", O_RDONLY);
	if (controller->branches_fd < 0) {
		error_errno("open branches directory");
		return -1;
	}
	controller->layers_fd =
		open_dir_at(controller->root_fd, "layers", O_RDONLY);
	if (controller->layers_fd < 0) {
		error_errno("open layers directory");
		return -1;
	}
	controller->merged_fd =
		open_dir_at(controller->root_fd, "merged", O_RDONLY);
	if (controller->merged_fd < 0) {
		error_errno("open merged root");
		return -1;
	}
	return 0;
}

static int atomic_install_file(int dir_fd, const char *tmp_name,
			       const char *final_name, const struct text *text,
			       bool replace)
{
	int fd = -1;

	fd = openat(dir_fd, tmp_name,
		    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -1;
	if (write_all(fd, text->data, text->len) || fsync(fd))
		goto out_err;
	if (close(fd)) {
		fd = -1;
		goto out_err;
	}
	fd = -1;
	if (!replace && path_exists_at(dir_fd, final_name)) {
		errno = EEXIST;
		return -1;
	}
	if (renameat(dir_fd, tmp_name, dir_fd, final_name))
		return -1;
	if (fsync(dir_fd))
		return -1;
	return 0;

out_err:
	{
		int saved_errno = errno;

		if (fd >= 0)
			close(fd);
		errno = saved_errno;
		/*
		 * Leave the temp file in place.  A later invocation will fail
		 * closed instead of guessing whether storage made progress.
		 */
		return -1;
	}
}

static int remove_transaction(int meta_fd)
{
	if (unlinkat(meta_fd, TRANSACTION_FILE, 0))
		return -1;
	return fsync(meta_fd);
}

static int remove_tree_contents(int dir_fd)
{
	DIR *dir;
	struct dirent *entry;
	int scan_fd;

	scan_fd = dup(dir_fd);
	if (scan_fd < 0)
		return -1;
	dir = fdopendir(scan_fd);
	if (!dir) {
		close(scan_fd);
		return -1;
	}
	errno = 0;
	while ((entry = readdir(dir))) {
		struct stat st;

		if (!strcmp(entry->d_name, ".") ||
		    !strcmp(entry->d_name, ".."))
			continue;
		if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW))
			goto out_err;
		if (S_ISDIR(st.st_mode)) {
			int child_fd = open_dir_at(dir_fd, entry->d_name, O_RDONLY);

			if (child_fd < 0)
				goto out_err;
			if (remove_tree_contents(child_fd)) {
				int saved_errno = errno;

				close(child_fd);
				errno = saved_errno;
				goto out_err;
			}
			if (close(child_fd))
				goto out_err;
			if (unlinkat(dir_fd, entry->d_name, AT_REMOVEDIR))
				goto out_err;
		} else if (unlinkat(dir_fd, entry->d_name, 0)) {
			goto out_err;
		}
		errno = 0;
	}
	if (errno)
		goto out_err;
	if (closedir(dir))
		return -1;
	return 0;

out_err:
	{
		int saved_errno = errno;

		closedir(dir);
		errno = saved_errno;
		return -1;
	}
}

static int remove_tree_at(int parent_fd, const char *name)
{
	int fd;

	fd = open_dir_at(parent_fd, name, O_RDONLY);
	if (fd < 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}
	if (remove_tree_contents(fd)) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (close(fd))
		return -1;
	return unlinkat(parent_fd, name, AT_REMOVEDIR);
}

static int create_fresh_branch(struct controller *controller,
			       const char *branch, bool *created)
{
	int branch_fd = -1;
	int upper_fd = -1;
	int work_fd = -1;
	int ret = -1;

	if (mkdirat(controller->branches_fd, branch, 0700))
		return -1;
	*created = true;
	branch_fd = open_dir_at(controller->branches_fd, branch, O_RDONLY);
	if (branch_fd < 0)
		goto out;
	if (mkdirat(branch_fd, "upper", 0755) ||
	    mkdirat(branch_fd, "work", 0700))
		goto out;
	upper_fd = open_dir_at(branch_fd, "upper", O_RDONLY);
	work_fd = open_dir_at(branch_fd, "work", O_RDONLY);
	if (upper_fd < 0 || work_fd < 0)
		goto out;
	if (fsync(upper_fd) || fsync(work_fd) || fsync(branch_fd) ||
	    fsync(controller->branches_fd))
		goto out;
	ret = 0;
out:
	close_preserve_errno(work_fd);
	close_preserve_errno(upper_fd);
	close_preserve_errno(branch_fd);
	return ret;
}

static void switch_fds_init(struct switch_fds *fds)
{
	unsigned int i;

	fds->branch_fd = -1;
	fds->upper_fd = -1;
	fds->work_fd = -1;
	fds->nr_lower = 0;
	for (i = 0; i < DELTAFS_V2_MAX_LOWERS; i++)
		fds->lower_fds[i] = -1;
}

static void switch_fds_close(struct switch_fds *fds)
{
	unsigned int i;

	for (i = 0; i < DELTAFS_V2_MAX_LOWERS; i++)
		close_preserve_errno(fds->lower_fds[i]);
	close_preserve_errno(fds->work_fd);
	close_preserve_errno(fds->upper_fd);
	close_preserve_errno(fds->branch_fd);
	switch_fds_init(fds);
}

static int open_lower(struct controller *controller, const char *path)
{
	const char *snapshot_id;

	if (!strcmp(path, "base"))
		return open_dir_at(controller->root_fd, "base", O_PATH);
	snapshot_id = lower_snapshot_id(path);
	if (!snapshot_id) {
		errno = EINVAL;
		return -1;
	}
	return open_dir_at(controller->layers_fd, snapshot_id, O_PATH);
}

static int open_switch_fds(struct controller *controller,
			   const char *branch, const struct strvec *lowers,
			   struct switch_fds *fds)
{
	size_t i;

	fds->branch_fd =
		open_dir_at(controller->branches_fd, branch, O_RDONLY);
	if (fds->branch_fd < 0)
		return -1;
	fds->upper_fd = open_dir_at(fds->branch_fd, "upper", O_PATH);
	if (fds->upper_fd < 0)
		return -1;
	fds->work_fd = open_dir_at(fds->branch_fd, "work", O_PATH);
	if (fds->work_fd < 0)
		return -1;
	for (i = 0; i < lowers->nr; i++) {
		fds->lower_fds[i] = open_lower(controller, lowers->items[i]);
		if (fds->lower_fds[i] < 0)
			return -1;
	}
	fds->nr_lower = (unsigned int)lowers->nr;
	return 0;
}

static int validate_layout(struct controller *controller,
			   const struct controller_state *state,
			   enum operation operation, const char *snapshot_id,
			   const char *new_branch,
			   const struct strvec *target_lowers)
{
	int active_fd = -1;
	int fd = -1;
	size_t i;
	int ret = -1;

	active_fd = open_dir_at(controller->branches_fd, state->active_branch,
				O_RDONLY);
	if (active_fd < 0) {
		error_errno("open active branch %s", state->active_branch);
		goto out;
	}
	fd = open_dir_at(active_fd, "upper", O_PATH);
	if (fd < 0) {
		error_errno("open active upper for %s", state->active_branch);
		goto out;
	}
	close(fd);
	fd = open_dir_at(active_fd, "work", O_PATH);
	if (fd < 0) {
		error_errno("open active work for %s", state->active_branch);
		goto out;
	}
	close(fd);
	fd = -1;
	if (path_exists_at(controller->branches_fd, new_branch)) {
		error_msg("new branch %s already exists", new_branch);
		errno = EEXIST;
		goto out;
	}
	if (operation == OP_CHECKPOINT &&
	    path_exists_at(controller->layers_fd, snapshot_id)) {
		error_msg("layer for checkpoint '%s' already exists", snapshot_id);
		errno = EEXIST;
		goto out;
	}
	for (i = 0; i < state->retired_branches.nr; i++) {
		fd = open_dir_at(controller->branches_fd,
				state->retired_branches.items[i], O_PATH);
		if (fd < 0) {
			error_errno("open retired branch %s",
				    state->retired_branches.items[i]);
			goto out;
		}
		close(fd);
		fd = -1;
	}
	for (i = operation == OP_CHECKPOINT ? 1 : 0;
	     i < target_lowers->nr; i++) {
		fd = open_lower(controller, target_lowers->items[i]);
		if (fd < 0) {
			error_errno("open target lower %s",
				    target_lowers->items[i]);
			goto out;
		}
		close(fd);
		fd = -1;
	}
	ret = 0;
out:
	close_preserve_errno(fd);
	close_preserve_errno(active_fd);
	return ret;
}

static void fill_checkpoint_request(struct deltafs_ioc_checkpoint_v2 *request,
				    const struct switch_fds *fds,
				    uint64_t generation)
{
	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->upper_fd = fds->upper_fd;
	request->work_fd = fds->work_fd;
}

static int fill_restore_request(struct deltafs_ioc_restore_v2 *request,
				const struct switch_fds *fds,
				 uint64_t generation, size_t keep_bottom)
{
	unsigned int i;

	if (keep_bottom > UINT_MAX ||
	    fds->nr_lower > DELTAFS_V2_MAX_LOWERS ||
	    fds->nr_lower + keep_bottom > DELTAFS_V2_MAX_LOWERS ||
	    fds->nr_lower + keep_bottom == 0) {
		errno = EINVAL;
		return -1;
	}
	memset(request, 0, sizeof(*request));
	request->size = sizeof(*request);
	request->version = DELTAFS_ABI_VERSION;
	request->expected_generation = generation;
	request->keep_bottom = (unsigned int)keep_bottom;
	request->nr_fds = DELTAFS_V2_RESTORE_LOWER_BASE + fds->nr_lower;
	for (i = 0; i < DELTAFS_V2_MAX_RESTORE_FDS; i++)
		request->fds[i] = -1;
	request->fds[DELTAFS_V2_RESTORE_UPPER_FD] = fds->upper_fd;
	request->fds[DELTAFS_V2_RESTORE_WORK_FD] = fds->work_fd;
	for (i = 0; i < fds->nr_lower; i++)
		request->fds[DELTAFS_V2_RESTORE_LOWER_BASE + i] =
			fds->lower_fds[i];
	return 0;
}

static int rollback_checkpoint_rename(struct controller *controller,
				      int active_branch_fd,
				      const char *snapshot_id)
{
	if (renameat(controller->layers_fd, snapshot_id,
		     active_branch_fd, "upper"))
		return -1;
	if (fsync(controller->layers_fd) || fsync(active_branch_fd))
		return -1;
	return 0;
}

static int cleanup_before_commit(struct controller *controller,
				 enum operation operation,
				 int active_branch_fd,
				 const char *snapshot_id,
				 const char *new_branch,
				 bool renamed, bool fresh_created)
{
	int first_errno = 0;

	if (operation == OP_CHECKPOINT && renamed &&
	    rollback_checkpoint_rename(controller, active_branch_fd,
				       snapshot_id))
		first_errno = errno;
	if (fresh_created) {
		if (remove_tree_at(controller->branches_fd, new_branch)) {
			if (!first_errno)
				first_errno = errno;
		} else if (fsync(controller->branches_fd) && !first_errno) {
			first_errno = errno;
		}
	}
	if (first_errno) {
		errno = first_errno;
		return -1;
	}
	if (remove_transaction(controller->meta_fd))
		return -1;
	return 0;
}

#ifdef DELTAFSCTL_TESTING
static bool test_failpoint(const char *name)
{
	const char *configured = getenv("DELTAFSCTL_TEST_FAILPOINT");

	return configured && !strcmp(configured, name);
}
#endif

static int run_operation(enum operation operation, const char *sandbox_root,
			 const char *snapshot_id)
{
	struct deltafs_ioc_checkpoint_v2 checkpoint_request;
	struct deltafs_ioc_restore_v2 restore_request;
	struct controller_state *state = NULL;
	struct controller_state *next_state = NULL;
	struct controller controller;
	struct switch_fds fds;
	struct strvec target_lowers = { };
	struct strvec prefix_lowers = { };
	struct text state_text = { };
	struct text transaction_text = { };
	char new_branch[64];
	char *old_branch = NULL;
	int active_branch_fd = -1;
	int primary_errno = 0;
	int ret = -1;
	bool transaction_installed = false;
	bool fresh_created = false;
	bool renamed = false;
	bool kernel_committed = false;
	size_t keep_bottom = 0;
	unsigned long command;

	controller_init(&controller);
	switch_fds_init(&fds);
	if (controller_open(&controller, sandbox_root))
		goto out;
	if (read_state_file(controller.meta_fd, &state))
		goto out;
	if (state->generation == UINT64_MAX) {
		error_msg("kernel generation is exhausted");
		errno = EOVERFLOW;
		goto out;
	}
	if (snprintf(new_branch, sizeof(new_branch), "g%" PRIu64,
		     state->generation + 1) >= (int)sizeof(new_branch)) {
		errno = ENAMETOOLONG;
		goto out;
	}
	if (build_target_lowers(operation, state, snapshot_id,
				&target_lowers)) {
		if (errno == ENOMEM)
			error_errno("build target lower chain");
		goto out;
	}
	if (operation == OP_RESTORE) {
		keep_bottom = longest_common_suffix(&state->active_lowers,
						    &target_lowers);
		if (build_restore_prefix(&target_lowers, keep_bottom,
					 &prefix_lowers)) {
			error_msg("build restore lower prefix");
			goto out;
		}
	}
	if (validate_layout(&controller, state, operation, snapshot_id,
			    new_branch, &target_lowers))
		goto out;

	old_branch = strdup(state->active_branch);
	next_state = state_clone(state);
	if (!old_branch || !next_state ||
	    apply_transition(operation, next_state, snapshot_id, new_branch,
			     &target_lowers) ||
	    validate_state(next_state) ||
	    serialize_state(next_state, &state_text) ||
	    serialize_transaction(operation, state->generation,
				  state->active_branch, new_branch, snapshot_id,
				  &target_lowers, &prefix_lowers, keep_bottom,
				  &transaction_text)) {
		if (errno == ENOMEM)
			error_errno("preallocate controller transaction");
		goto out;
	}

	active_branch_fd =
		open_dir_at(controller.branches_fd, old_branch, O_RDONLY);
	if (active_branch_fd < 0) {
		error_errno("open active branch %s", old_branch);
		goto out;
	}
	if (syncfs(controller.merged_fd)) {
		error_errno("sync merged filesystem");
		goto out;
	}

	/*
	 * Intent precedes every backing-tree mutation, including fresh branch
	 * creation.  A crash can therefore never leave an unjournaled gN+1.
	 */
	if (atomic_install_file(controller.meta_fd, TRANSACTION_TMP_FILE,
				TRANSACTION_FILE, &transaction_text, false)) {
		error_errno("persist %s", TRANSACTION_FILE);
		goto out;
	}
	transaction_installed = true;

	if (create_fresh_branch(&controller, new_branch, &fresh_created)) {
		primary_errno = errno;
		error_errno("create fresh branch %s", new_branch);
		goto rollback;
	}
	if (operation == OP_CHECKPOINT) {
		if (renameat(active_branch_fd, "upper",
			     controller.layers_fd, snapshot_id)) {
			primary_errno = errno;
			error_errno("freeze active upper as layers/%s",
				    snapshot_id);
			goto rollback;
		}
		renamed = true;
		if (fsync(active_branch_fd) || fsync(controller.layers_fd)) {
			primary_errno = errno;
			error_errno("persist checkpoint layer rename");
			goto rollback;
		}
	}

	if (open_switch_fds(&controller, new_branch, &prefix_lowers, &fds)) {
		primary_errno = errno;
		error_errno("open switch paths");
		goto rollback;
	}
	if (operation == OP_CHECKPOINT)
		fill_checkpoint_request(&checkpoint_request, &fds,
					state->generation);
	else if (fill_restore_request(&restore_request, &fds,
				      state->generation, keep_bottom)) {
		primary_errno = errno;
		goto rollback;
	}

#ifdef DELTAFSCTL_TESTING
	if (test_failpoint("before_ioctl")) {
		primary_errno = EIO;
		error_msg("injected failure before ioctl");
		goto rollback;
	}
#endif

	command = operation == OP_CHECKPOINT ? DELTAFS_IOC_CHECKPOINT :
		DELTAFS_IOC_RESTORE;
	if (ioctl(controller.merged_fd, command,
		  operation == OP_CHECKPOINT ? (void *)&checkpoint_request :
					       (void *)&restore_request)) {
		primary_errno = errno;
		error_errno("%s ioctl", operation_name(operation));
		goto rollback;
	}
	kernel_committed = true;

#ifdef DELTAFSCTL_TESTING
	if (test_failpoint("after_ioctl"))
		_exit(200);
#endif

	/*
	 * Past this point rollback is forbidden: the kernel has published a new
	 * generation.  Any metadata error leaves transaction.json as an explicit
	 * recovery barrier.
	 */
	if (atomic_install_file(controller.meta_fd, STATE_TMP_FILE, STATE_FILE,
				&state_text, true)) {
		error_errno("%s committed in kernel but %s update failed; "
			    "%s is retained",
			    operation_name(operation), STATE_FILE,
			    TRANSACTION_FILE);
		goto out;
	}
	if (remove_transaction(controller.meta_fd)) {
		error_errno("%s committed but transaction cleanup failed",
			    operation_name(operation));
		goto out;
	}
	transaction_installed = false;
	printf("%s: %s '%s' committed generation %" PRIu64 " -> %" PRIu64
	       "\n", program_name, operation_name(operation), snapshot_id,
	       state->generation, state->generation + 1);
	ret = 0;
	goto out;

rollback:
	switch_fds_close(&fds);
	if (!primary_errno)
		primary_errno = errno ? errno : EIO;
	if (cleanup_before_commit(&controller, operation, active_branch_fd,
				  snapshot_id, new_branch, renamed,
				  fresh_created)) {
		error_errno("compensation failed; %s is retained",
			    TRANSACTION_FILE);
		goto out;
	}
	transaction_installed = false;
	errno = primary_errno;
	error_msg("%s aborted; active view and %s were restored",
		  operation_name(operation), STATE_FILE);

out:
	if (kernel_committed && ret)
		error_msg("manual recovery is required before another operation");
	else if (transaction_installed && ret)
		error_msg("%s remains as a fail-stop recovery barrier",
			  TRANSACTION_FILE);
	switch_fds_close(&fds);
	close_preserve_errno(active_branch_fd);
	free(old_branch);
	text_free(&transaction_text);
	text_free(&state_text);
	strvec_free(&target_lowers);
	strvec_free(&prefix_lowers);
	state_free(next_state);
	state_free(state);
	controller_close(&controller);
	return ret;
}

static void usage(FILE *stream)
{
	fprintf(stream,
		"Usage:\n"
		"  %s --assume-quiesced checkpoint SANDBOX CHECKPOINT_ID\n"
		"  %s --assume-quiesced restore    SANDBOX CHECKPOINT_ID\n"
		"\n"
		"SANDBOX must contain base, layers, branches, meta and the mounted\n"
		"merged root.  --assume-quiesced is mandatory because DeltaFS v2\n"
		"does not make open workload fds, mmap or concurrent writes safe.\n",
		program_name, program_name);
}

int main(int argc, char **argv)
{
	enum operation operation;
	bool assume_quiesced = false;
	int arg = 1;

	program_name = argv[0] && argv[0][0] ? argv[0] : program_name;
	umask(0077);
	if (arg < argc && !strcmp(argv[arg], "--assume-quiesced")) {
		assume_quiesced = true;
		arg++;
	}
	if (arg < argc &&
	    (!strcmp(argv[arg], "-h") || !strcmp(argv[arg], "--help"))) {
		usage(stdout);
		return EXIT_SUCCESS;
	}
	if (!assume_quiesced || argc - arg != 3) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[arg], "checkpoint"))
		operation = OP_CHECKPOINT;
	else if (!strcmp(argv[arg], "restore"))
		operation = OP_RESTORE;
	else {
		error_msg("unknown operation '%s'", argv[arg]);
		usage(stderr);
		return EXIT_FAILURE;
	}
	if (!valid_component(argv[arg + 2])) {
		error_msg("invalid checkpoint id '%s'", argv[arg + 2]);
		return EXIT_FAILURE;
	}
	if (run_operation(operation, argv[arg + 1], argv[arg + 2]))
		return EXIT_FAILURE;
	return EXIT_SUCCESS;
}
