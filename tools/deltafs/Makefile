# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -Werror
UAPI_HEADERS ?= ../../usr/include
KERNEL_DELTAFS_OBJ ?= ../../fs/overlayfs/deltafs.o
KERNEL_DELTAFS_SRC ?= ../../fs/overlayfs/deltafs.c
KERNEL_OVERLAYFS_SUPER_SRC ?= ../../fs/overlayfs/super.c
CHECKPOINT_MODE ?= enabled
CPPFLAGS += -I$(UAPI_HEADERS)

all: e0-tools

e0-phase1-tools: deltafs_e0_layout_test deltafs_e0_ioctl_test

e0-tools: e0-phase1-tools deltafsctl

e1-bench:
	$(MAKE) -C bench/e1 all

e2-bench:
	$(MAKE) -C bench/e2 all

e3-bench:
	$(MAKE) -C bench/e3 all

deltafsctl: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafsctl_test: deltafsctl.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DDELTAFSCTL_TESTING $< -o $@

deltafs_e0_layout_test: deltafs_e0_layout_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

deltafs_e0_ioctl_test: deltafs_e0_ioctl_test.c $(UAPI_HEADERS)/linux/deltafs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

$(UAPI_HEADERS)/linux/deltafs.h: ../../include/uapi/linux/deltafs.h
	$(MAKE) -C ../.. headers_install

clean:
	$(RM) deltafsctl deltafsctl_test deltafs_e0_layout_test \
		deltafs_e0_ioctl_test
	$(MAKE) -C bench/e1 clean
	$(MAKE) -C bench/e2 clean
	$(MAKE) -C bench/e3 clean

check-e0-checkpoints:
	./deltafs_e0_checkpoint_callsite_test.sh "$(KERNEL_DELTAFS_OBJ)" \
		"$(KERNEL_DELTAFS_SRC)" "$(CHECKPOINT_MODE)"

check-e0-fast-path:
	./deltafs_e0_fast_path_static_test.sh "$(KERNEL_DELTAFS_SRC)" \
		"$(KERNEL_OVERLAYFS_SUPER_SRC)"

test-e0-phase1: e0-phase1-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	./deltafs_e0_phase1_test.sh --backing-root "$(BACKING_ROOT)"

check-e0-layout: deltafs_e0_layout_test
	./deltafs_e0_layout_test

test-e0-controller: deltafsctl
	./deltafs_e0_controller_test.sh

test-e0-acceptance: e0-tools
	@test -n "$(BACKING_ROOT)" || \
		{ echo "BACKING_ROOT is required" >&2; exit 2; }
	@test -n "$(EXTRA_BACKING_ROOT)" || \
		{ echo "EXTRA_BACKING_ROOT is required" >&2; exit 2; }
	./deltafs_e0_acceptance_test.sh --backing-root "$(BACKING_ROOT)" \
		--extra-backing-root "$(EXTRA_BACKING_ROOT)"

.PHONY: all e0-phase1-tools e0-tools e1-bench e2-bench e3-bench clean \
	check-e0-checkpoints check-e0-fast-path test-e0-phase1 check-e0-layout test-e0-controller \
	test-e0-acceptance
