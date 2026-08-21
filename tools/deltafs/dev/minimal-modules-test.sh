#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Static regression gate for the release/guest minimal module dependency set.

set -Eeuo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
readonly SCRIPT_DIR
readonly HOST_SCRIPT=$SCRIPT_DIR/host-build.sh
readonly GUEST_SCRIPT=$SCRIPT_DIR/guest-align.sh
readonly REQUIRED_MODULES=(
	arch/x86/kernel/msr.ko
	drivers/md/dm-multipath.ko
	fs/autofs/autofs4.ko
	fs/nls/nls_iso8859-1.ko
	fs/overlayfs/overlay.ko
	fs/f2fs/f2fs.ko
	crypto/crc32_generic.ko
	fs/xfs/xfs.ko
	lib/lz4/lz4_compress.ko
	lib/lz4/lz4hc_compress.ko
	lib/libcrc32c.ko
	fs/netfs/netfs.ko
	fs/9p/9p.ko
	net/9p/9pnet.ko
	net/9p/9pnet_virtio.ko
)

fail() {
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

read_module_list() {
	local script=$1

	awk '
		/^[[:space:]]*MINIMAL_MODULES=\([[:space:]]*$/ {
			in_list = 1
			next
		}
		in_list && /^[[:space:]]*\)[[:space:]]*$/ { exit }
		in_list {
			line = $0
			sub(/[[:space:]]*#.*/, "", line)
			gsub(/[[:space:]"]/, "", line)
			if (length(line))
				print line
		}
	' "$script"
}

contains() {
	local expected=$1
	shift
	local module

	for module in "$@"; do
		[[ $module == "$expected" ]] && return 0
	done
	return 1
}

mapfile -t host_modules < <(read_module_list "$HOST_SCRIPT")
mapfile -t guest_modules < <(read_module_list "$GUEST_SCRIPT")

((${#host_modules[@]} > 0)) || fail "no MINIMAL_MODULES found in $HOST_SCRIPT"
((${#guest_modules[@]} > 0)) || fail "no MINIMAL_MODULES found in $GUEST_SCRIPT"

[[ ${host_modules[*]} == "${guest_modules[*]}" ]] || fail \
	"host and guest MINIMAL_MODULES differ"

for module in "${REQUIRED_MODULES[@]}"; do
	contains "$module" "${host_modules[@]}" || fail \
		"MINIMAL_MODULES is missing required dependency $module"
done

	printf 'PASS: host/guest minimal module lists contain all %d required modules\n' \
	"${#REQUIRED_MODULES[@]}"
