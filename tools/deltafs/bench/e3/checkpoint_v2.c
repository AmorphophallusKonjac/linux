// SPDX-License-Identifier: GPL-2.0

#include "deltafs_v2_common.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_generation(const char *text, uint64_t *generation)
{
	char *end;
	unsigned long long value;

	if (!text || !text[0] || text[0] == '+' || text[0] == '-') {
		errno = EINVAL;
		return -1;
	}
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || *end || !value || value == UINT64_MAX) {
		if (!errno)
			errno = EINVAL;
		return -1;
	}
	*generation = (uint64_t)value;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t generation;

	if (argc != 5 || parse_generation(argv[2], &generation)) {
		fprintf(stderr, "Usage: %s MERGED EXPECTED_GENERATION UPPER WORK\n",
			argv[0]);
		return EXIT_FAILURE;
	}
	if (e3_checkpoint_v2(argv[1], argv[3], argv[4], generation)) {
		fprintf(stderr, "%s: DeltaFS v2 checkpoint: %s\n", argv[0],
			strerror(errno));
		return EXIT_FAILURE;
	}
	printf("checkpoint: generation %llu -> %llu\n",
	       (unsigned long long)generation,
	       (unsigned long long)(generation + 1));
	return EXIT_SUCCESS;
}
