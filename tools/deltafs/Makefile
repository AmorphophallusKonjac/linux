# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../usr/include
KERNEL_DELTAFS_OBJ ?= ../../fs/overlayfs/deltafs.o
KERNEL_DELTAFS_SRC ?= ../../fs/overlayfs/deltafs.c
CHECKPOINT_MODE ?= enabled
CPPFLAGS += -I$(UAPI_HEADERS)

all: v2-phase1-tools

v2-phase1-tools: deltafs_v2_layout_test deltafs_v2_ioctl_test

v2-tools: v2-phase1-tools

e2-bench:
	$(MAKE) -C bench/e2 all

e3-bench:
	$(MAKE) -C bench/e3 all

p5_commit_test: p5_commit_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p5_checkpoint_test: p5_checkpoint_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p6_generation_test: p6_generation_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

p7_ioctl_test: p7_ioctl_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafsctl: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafsctl_test: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DDELTAFSCTL_TESTING $< -o $@

deltafs_v2_layout_test: deltafs_v2_layout_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafs_v2_ioctl_test: deltafs_v2_ioctl_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

$(UAPI_HEADERS)/linux/deltafs.h: ../../include/uapi/linux/deltafs.h
	$(MAKE) -C ../.. headers_install

clean:
	$(RM) p5_commit_test p5_checkpoint_test p6_generation_test p7_ioctl_test \
		deltafsctl deltafsctl_test deltafs_v2_layout_test \
		deltafs_v2_ioctl_test
	$(MAKE) -C bench/e2 clean
	$(MAKE) -C bench/e3 clean

test-p6-controller: deltafsctl
	./p6_controller_unit_test.sh

test-p7-schedule:
	DELTAFS_P7_SCHEDULE_SELFTEST=1 ./p7_acceptance_test.sh

p7-tools: p5_commit_test p5_checkpoint_test p6_generation_test \
	p7_ioctl_test deltafsctl deltafsctl_test

check-p7-checkpoints:
	./p7_checkpoint_callsite_test.sh "$(KERNEL_DELTAFS_OBJ)" \
		"$(KERNEL_DELTAFS_SRC)" "$(CHECKPOINT_MODE)"

test-p7: p7-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	@test -n "$(EXTRA_BACKING_ROOT)" || \
		{ echo "EXTRA_BACKING_ROOT is required" >&2; exit 2; }
	./p7_acceptance_test.sh --backing-root "$(BACKING_ROOT)" \
		--extra-backing-root "$(EXTRA_BACKING_ROOT)"

test-v2-phase1: v2-phase1-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	./deltafs_v2_phase1_test.sh --backing-root "$(BACKING_ROOT)"

check-v2-layout: deltafs_v2_layout_test
	./deltafs_v2_layout_test

.PHONY: all v2-phase1-tools v2-tools e2-bench e3-bench clean \
	test-p6-controller test-p7-schedule p7-tools check-p7-checkpoints test-p7 \
	test-v2-phase1 check-v2-layout
