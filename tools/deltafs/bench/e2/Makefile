# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../../../usr/include
CPPFLAGS += -I$(UAPI_HEADERS)

all: switch_once

switch_once: switch_once.c e2_common.c e2_common.h \
		$(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) switch_once.c e2_common.c -o $@

tests/request_layout_test: tests/request_layout_test.c e2_common.c \
		e2_common.h $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) \
		tests/request_layout_test.c e2_common.c -o $@

check: all tests/request_layout_test
	./tests/request_layout_test
	python3 -m unittest discover tests
	python3 -m py_compile run.py analyze.py tests/test_runner.py \
		tests/test_analyze.py

clean:
	$(RM) switch_once tests/request_layout_test

.PHONY: all check clean
