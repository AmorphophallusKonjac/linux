// SPDX-License-Identifier: GPL-2.0
#include "../bench_common.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

int main(void)
{
	char hash[BENCH_SHA256_HEX_SIZE];
	char escaped[64];
	unsigned char data[40];
	unsigned char again[40];

	assert(!bench_sha256_buffer("abc", 3, hash));
	assert(!strcmp(hash,
		"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	assert(!bench_json_escape("a\n\"\\", escaped, sizeof(escaped)));
	assert(!strcmp(escaped, "a\\u000a\\\"\\\\"));
	assert(!bench_fill_bytes(17, data, sizeof(data)));
	assert(!bench_fill_bytes(17, again, sizeof(again)));
	assert(!memcmp(data, again, sizeof(data)));
	assert(!bench_sha256_buffer(data, sizeof(data), hash));
	assert(!strcmp(hash,
		"289b27ef2bcfa111d62d7a27478f1464934de2a5b4e54694435084be8218e218"));
	errno = 0;
	assert(bench_json_escape("toolong", escaped, 2) == -1);
	assert(errno == ENOSPC);
	return 0;
}
