# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../usr/include
KERNEL_DELTAFS_OBJ ?= ../../fs/overlayfs/deltafs.o
KERNEL_DELTAFS_SRC ?= ../../fs/overlayfs/deltafs.c
KERNEL_OVERLAYFS_SUPER_SRC ?= ../../fs/overlayfs/super.c
CHECKPOINT_MODE ?= enabled
CPPFLAGS += -I$(UAPI_HEADERS)

all: v2-phase1-tools

v2-phase1-tools: deltafs_v2_layout_test deltafs_v2_ioctl_test

v2-tools: v2-phase1-tools deltafsctl

e2-bench:
	$(MAKE) -C bench/e2 all

e3-bench:
	$(MAKE) -C bench/e3 all

e4-bench:
	$(MAKE) -C bench/e4 all

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
	$(RM) deltafsctl deltafsctl_test deltafs_v2_layout_test \
		deltafs_v2_ioctl_test
	$(MAKE) -C bench/e2 clean
	$(MAKE) -C bench/e3 clean
	$(MAKE) -C bench/e4 clean

check-v2-checkpoints:
	./deltafs_v2_checkpoint_callsite_test.sh "$(KERNEL_DELTAFS_OBJ)" \
		"$(KERNEL_DELTAFS_SRC)" "$(CHECKPOINT_MODE)"

check-v2-fast-path:
	./deltafs_v2_fast_path_static_test.sh "$(KERNEL_DELTAFS_SRC)" \
		"$(KERNEL_OVERLAYFS_SUPER_SRC)"

test-v2-phase1: v2-phase1-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	./deltafs_v2_phase1_test.sh --backing-root "$(BACKING_ROOT)"

check-v2-layout: deltafs_v2_layout_test
	./deltafs_v2_layout_test

test-v2-controller: deltafsctl
	./deltafs_v2_controller_test.sh

test-v2-acceptance: v2-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	@test -n "$(EXTRA_BACKING_ROOT)" || \
		{ echo "EXTRA_BACKING_ROOT is required" >&2; exit 2; }
	./deltafs_v2_acceptance_test.sh --backing-root "$(BACKING_ROOT)" \
		--extra-backing-root "$(EXTRA_BACKING_ROOT)"

.PHONY: all v2-phase1-tools v2-tools e2-bench e3-bench e4-bench clean \
	check-v2-checkpoints check-v2-fast-path test-v2-phase1 check-v2-layout test-v2-controller \
	test-v2-acceptance
