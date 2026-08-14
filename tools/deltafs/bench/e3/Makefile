# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Werror
CPPFLAGS += -I.

all: copyup_bench

copyup_bench: copyup_bench.c bench_common.c bench_common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) copyup_bench.c bench_common.c -o $@

tests/fiemap_fixture_test: tests/fiemap_fixture_test.c bench_common.c bench_common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< bench_common.c -o $@

tests/blockstat_test: tests/blockstat_test.c bench_common.c bench_common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< bench_common.c -o $@

tests/common_test: tests/common_test.c bench_common.c bench_common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< bench_common.c -o $@

check: all tests/fiemap_fixture_test tests/blockstat_test tests/common_test
	./tests/fiemap_fixture_test
	./tests/blockstat_test
	./tests/common_test
	python3 -m unittest discover tests
	python3 -m py_compile events.py run.py analyze.py tests/test_events.py \
		tests/test_runner.py tests/test_analyze.py

clean:
	$(RM) copyup_bench tests/fiemap_fixture_test tests/blockstat_test \
		tests/common_test

.PHONY: all check clean
