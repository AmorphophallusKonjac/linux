#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P5 first-commit QEMU/KVM harness.
#
# This script mounts OverlayFS and must never be run on the development host.
# It refuses to continue unless QEMU/KVM guest evidence is present.

set -Eeuo pipefail

readonly OVERLAY_FEATURE_OPTIONS='index=off,nfs_export=off,metacopy=off,xino=off'
readonly OVERLAY_ROOT_OPTIONS='uuid=off,redirect_dir=nofollow'

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
commit_test="$script_dir/p5_commit_test"
backing_root=
run_dir=
merged=
marker_started=0
marker_finished=0
marker="deltafs-p5-commit[$$]"
mount_options=

usage()
{
	cat <<'EOF'
Usage: p5_commit_test.sh --backing-root PATH

PATH must be a writable directory on the backing filesystem used for all
active and target OverlayFS directories.  Run only inside the project QEMU/KVM
guest with the P5 kernel booted.
EOF
}

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

require_command()
{
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
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
	if grep -Eiq '(^|[^[:alnum:]])(qemu|kvm)([^[:alnum:]]|$)' <<< "$evidence"; then
		printf 'PASS: QEMU/KVM guest detected from DMI/device-tree\n'
		return
	fi

	die 'refusing to run outside a confirmed QEMU/KVM guest'
}

finish_marker()
{
	if ((marker_started != 0 && marker_finished == 0)); then
		printf '%s END\n' "$marker" 2>/dev/null > /dev/kmsg || true
		marker_finished=1
	fi
}

cleanup()
{
	local status=$?
	local still_mounted=0

	trap - EXIT
	set +e
	if [[ -n "$merged" ]] && mountpoint -q -- "$merged"; then
		if ! umount -- "$merged"; then
			printf 'FAIL: cleanup could not unmount %s\n' "$merged" >&2
			status=1
			still_mounted=1
		fi
	fi
	finish_marker
	if ((still_mounted == 0)) && [[ -n "$run_dir" && -d "$run_dir" ]]; then
		rm -rf -- "$run_dir"
	fi
	exit "$status"
}

scan_kernel_window()
{
	local diagnostics
	local window
	diagnostics='BUG:|WARNING:|KASAN:|KFENCE:|UBSAN:|use-after-free|double free'
	diagnostics+='|refcount_t:|possible circular locking dependency'
	diagnostics+='|suspicious RCU usage|kernel NULL pointer'
	diagnostics+='|general protection fault|unable to handle kernel'

	finish_marker
	window=$(dmesg | awk -v begin="$marker BEGIN" -v end="$marker END" '
		index($0, begin) { capture = 1; found_begin = 1 }
		capture { print }
		capture && index($0, end) { found_end = 1; exit }
		END { if (!found_begin || !found_end) exit 1 }
	') || die 'could not isolate the P5 kernel log window'

	if grep -Eiq "$diagnostics" <<< "$window"; then
		printf '%s\n' "$window" >&2
		die 'kernel diagnostics reported a P5 lifetime or locking failure'
	fi
	printf 'PASS: P5 kernel log window has no KASAN/refcount/lockdep failure\n'
}

while (($#)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || die '--backing-root requires a path'
		backing_root=$2
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

[[ -n "$backing_root" ]] || {
	usage >&2
	exit 1
}
((EUID == 0)) || die 'must run as root inside the guest'

require_command awk
require_command dmesg
require_command grep
require_command mount
require_command mountpoint
require_command mkdir
require_command mktemp
require_command realpath
require_command rm
require_command stat
require_command tr
require_command umount
confirm_qemu_kvm_guest

[[ -x "$commit_test" ]] || die "build the test first: make -C $script_dir p5_commit_test"
backing_root=$(realpath -e -- "$backing_root")
[[ -d "$backing_root" && -w "$backing_root" ]] || \
	die "backing root is not a writable directory: $backing_root"
[[ "$backing_root" != *:* && "$backing_root" != *,* ]] || \
	die 'backing root cannot contain colon or comma characters'
[[ $(stat -f -c %T -- "$backing_root") != overlayfs ]] || \
	die 'backing root itself must not be OverlayFS'
[[ -w /dev/kmsg ]] || die '/dev/kmsg must be writable for an isolated log window'

run_dir=$(mktemp -d "$backing_root/deltafs-p5.XXXXXX")
merged="$run_dir/merged"
mkdir -p -- \
	"$run_dir/active-upper" \
	"$run_dir/active-work" \
	"$run_dir/active-lower" \
	"$run_dir/scratch" \
	"$merged"
mount_options="lowerdir=$run_dir/active-lower"
mount_options+=",upperdir=$run_dir/active-upper"
mount_options+=",workdir=$run_dir/active-work,$OVERLAY_FEATURE_OPTIONS"
mount_options+=",$OVERLAY_ROOT_OPTIONS"

trap cleanup EXIT
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1

mount -t overlay overlay \
	-o "$mount_options" \
	"$merged"

"$commit_test" "$merged" "$run_dir/scratch" "$run_dir/active-upper"

umount -- "$merged"
printf 'PASS: unmount released the active and retired P5 views\n'
scan_kernel_window

rm -rf -- "$run_dir"
run_dir=
printf 'All P5 QEMU first-commit tests passed\n'
