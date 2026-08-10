# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../usr/include
CPPFLAGS += -I$(UAPI_HEADERS)

all: p1_ioctl_test p3_cache_test p4_state_test p5_commit_test \
	p5_checkpoint_test p6_generation_test deltafsctl deltafsctl_test

p1_ioctl_test: p1_ioctl_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p3_cache_test: p3_cache_test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p4_state_test: p4_state_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p5_commit_test: p5_commit_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p5_checkpoint_test: p5_checkpoint_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p6_generation_test: p6_generation_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafsctl: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafsctl_test: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DDELTAFSCTL_TESTING $< -o $@

$(UAPI_HEADERS)/linux/deltafs.h:
	$(MAKE) -C ../.. headers_install

clean:
	$(RM) p1_ioctl_test p3_cache_test p4_state_test p5_commit_test \
		p5_checkpoint_test p6_generation_test deltafsctl deltafsctl_test

test-p6-controller: deltafsctl
	./p6_controller_unit_test.sh

.PHONY: all clean test-p6-controller
