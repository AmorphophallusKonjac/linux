#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P3 generation=1 cache protocol test.
#
# This script mounts OverlayFS and must only run in an isolated QEMU/KVM guest.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly MAX_ITERATIONS=100000
readonly ORIGINAL_PAYLOAD=deltafs-p3-lower-original
readonly BOTTOM_PAYLOAD=deltafs-p3-bottom-only
readonly SHARED_TOP_PAYLOAD=deltafs-p3-shared-top
readonly SHARED_BOTTOM_PAYLOAD=deltafs-p3-shared-bottom

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cache_test="$script_dir/p3_cache_test"
ioctl_test="$script_dir/p1_ioctl_test"
backing_root=
iterations=128
run_dir=
merged=
mounted=0
module_loaded_by_test=0

usage()
{
	cat <<'EOF'
Usage: p3_cache_test.sh --backing-root PATH [--iterations N]

Required:
  --backing-root PATH   Writable, non-OverlayFS backing filesystem path

Options:
  --iterations N        Positive/negative lookup iterations (default: 128)
  -h, --help            Show this help
EOF
}

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

require_command()
{
	command -v "$1" >/dev/null 2>&1 ||
		die "required command not found: $1"
}

overlay_mount_count()
{
	awk '$0 ~ / - overlay / { count++ } END { print count + 0 }' \
		/proc/self/mountinfo
}

show_overlay_mounts()
{
	awk '$0 ~ / - overlay / { print "  " $5 }' /proc/self/mountinfo >&2
}

confirm_qemu_kvm_guest()
{
	local detected=
	local evidence=
	local path

	if command -v systemd-detect-virt >/dev/null 2>&1; then
		detected=$(systemd-detect-virt --vm 2>/dev/null || true)
		case "$detected" in
		kvm|qemu)
			printf 'PASS: QEMU/KVM guest detected (%s)\n' "$detected"
			return
			;;
		esac
	fi

	for path in \
		/sys/class/dmi/id/sys_vendor \
		/sys/class/dmi/id/product_name \
		/sys/class/dmi/id/board_vendor \
		/sys/class/dmi/id/bios_vendor \
		/proc/device-tree/model \
		/proc/device-tree/compatible \
		/proc/device-tree/hypervisor/compatible; do
		if [[ -r "$path" ]]; then
			evidence+=" $(tr '\0' '\n' < "$path" 2>/dev/null || true)"
		fi
	done

	if grep -Eiq '(^|[^[:alnum:]])(qemu|kvm)([^[:alnum:]]|$)' \
		<<< "$evidence"; then
		printf 'PASS: QEMU/KVM guest detected from DMI/device-tree\n'
		return
	fi

	die "refusing to run outside a confirmed QEMU/KVM guest"
}

cleanup()
{
	local status=$?

	trap - EXIT
	set +e

	if ((mounted != 0)) && [[ -n "$merged" ]] &&
	   mountpoint -q -- "$merged"; then
		umount -- "$merged" ||
			printf 'FAIL: cleanup could not unmount %s\n' "$merged" >&2
	fi

	if ((module_loaded_by_test != 0)) &&
	   [[ -d /sys/module/$OVERLAY_MODULE ]] &&
	   (( $(overlay_mount_count) == 0 )); then
		modprobe -r "$OVERLAY_MODULE" ||
			printf 'FAIL: cleanup could not unload %s\n' \
				"$OVERLAY_MODULE" >&2
	fi

	if [[ -n "$run_dir" && -d "$run_dir" ]]; then
		if ((status == 0)); then
			rm -rf -- "$run_dir"
		else
			printf 'Test files preserved at %s\n' "$run_dir" >&2
		fi
	fi

	exit "$status"
}

trap cleanup EXIT

while (($#)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || die "--backing-root requires a value"
		backing_root=$2
		shift 2
		;;
	--iterations)
		(($# >= 2)) || die "--iterations requires a value"
		iterations=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown argument: $1"
		;;
	esac
done

[[ $EUID -eq 0 ]] || die "this test must run as root"
[[ -n "$backing_root" ]] || {
	usage >&2
	die "--backing-root is required"
}
[[ "$iterations" =~ ^[1-9][0-9]*$ ]] ||
	die "--iterations must be a positive integer"
((iterations <= MAX_ITERATIONS)) ||
	die "--iterations must not exceed $MAX_ITERATIONS"

for command in awk findmnt grep mktemp modprobe mount mountpoint realpath tr umount; do
	require_command "$command"
done
confirm_qemu_kvm_guest

[[ -x "$cache_test" && -x "$ioctl_test" ]] ||
	die "build tests first with: make -C tools/deltafs"
[[ -d "$backing_root" && -w "$backing_root" ]] ||
	die "backing root must be an existing writable directory"
backing_root=$(realpath -e -- "$backing_root")
[[ $(findmnt -n -o FSTYPE --target "$backing_root") != overlay ]] ||
	die "backing root must not reside on OverlayFS"

if (( $(overlay_mount_count) != 0 )); then
	show_overlay_mounts
	die "refusing to run while other OverlayFS mounts exist"
fi

if [[ ! -d /sys/module/$OVERLAY_MODULE ]]; then
	modprobe "$OVERLAY_MODULE"
	module_loaded_by_test=1
fi
[[ -d /sys/module/$OVERLAY_MODULE ]] ||
	die "OverlayFS module/sysfs state is unavailable"

run_dir=$(mktemp -d "$backing_root/deltafs-p3.XXXXXX")
top_lower="$run_dir/top-lower"
bottom_lower="$run_dir/bottom-lower"
upper="$run_dir/upper"
work="$run_dir/work"
merged="$run_dir/merged"

mkdir -p -- "$top_lower" "$bottom_lower" "$upper" "$work" "$merged"
printf '%s\n' "$ORIGINAL_PAYLOAD" > "$top_lower/lower-positive.txt"
printf '%s\n' "$BOTTOM_PAYLOAD" > "$bottom_lower/bottom-only.txt"
printf '%s\n' "$SHARED_TOP_PAYLOAD" > "$top_lower/shared.txt"
printf '%s\n' "$SHARED_BOTTOM_PAYLOAD" > "$bottom_lower/shared.txt"

mount -t overlay overlay \
	-o "lowerdir=$top_lower:$bottom_lower,upperdir=$upper,workdir=$work,index=off,xino=off" \
	"$merged"
mounted=1

"$cache_test" "$merged" "$top_lower" "$iterations"
"$ioctl_test" "$merged" "$upper" "$work" "$top_lower" "$bottom_lower"

umount -- "$merged"
mounted=0
mount -t overlay overlay \
	-o "lowerdir=$top_lower:$bottom_lower" "$merged"
mounted=1

for ((i = 0; i < iterations; i++)); do
	[[ $(<"$merged/lower-positive.txt") == "$ORIGINAL_PAYLOAD" ]] ||
		die "lower-only positive lookup returned wrong data"
	[[ $(<"$merged/bottom-only.txt") == "$BOTTOM_PAYLOAD" ]] ||
		die "lower-only lookup did not reach bottom layer"
	[[ $(<"$merged/shared.txt") == "$SHARED_TOP_PAYLOAD" ]] ||
		die "lower-only lookup violated layer precedence"
	[[ ! -e "$merged/.deltafs-p3-lower-missing" ]] ||
		die "lower-only negative lookup unexpectedly succeeded"
done
printf 'PASS: lower-only generation=1 lookup (%s iterations)\n' "$iterations"

umount -- "$merged"
mounted=0

mkdir -p -- "$top_lower/nested-layer"
if mount -t overlay overlay \
	-o "lowerdir=$top_lower:$top_lower/nested-layer" "$merged"; then
	mounted=1
	die "overlapping lower layer roots unexpectedly mounted"
fi
mountpoint -q -- "$merged" &&
	die "overlapping lower failure left an OverlayFS mount"
printf 'PASS: trap rejects overlapping layer roots\n'

if (( $(overlay_mount_count) != 0 )); then
	show_overlay_mounts
	die "OverlayFS mounts remain after P3 test"
fi

if ((module_loaded_by_test != 0)); then
	modprobe -r "$OVERLAY_MODULE"
	module_loaded_by_test=0
fi

printf 'All P3 cache protocol tests passed\n'
