# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../../../usr/include
CPPFLAGS += -I. -I$(UAPI_HEADERS)

all: temporal_write_bench checkpoint_v2

temporal_write_bench: temporal_write_bench.c ../e3/bench_common.c \
		../e3/bench_common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -I../e3 $< ../e3/bench_common.c -o $@

checkpoint_v2: checkpoint_v2.c deltafs_v2_common.c deltafs_v2_common.h \
		$(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) checkpoint_v2.c \
		deltafs_v2_common.c -o $@

check: all
	python3 -m unittest discover tests
	python3 -m py_compile events.py run.py analyze.py tests/test_events.py \
		tests/test_runner.py tests/test_analyze.py

clean:
	$(RM) temporal_write_bench checkpoint_v2

.PHONY: all check clean
