#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P5 checkpoint/multi-commit QEMU/KVM harness.
#
# This script mounts OverlayFS, performs three commits on the same mount,
# unloads the overlay module, and scans kmemleak plus the isolated kernel log
# window.  It must never be run on the development host.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly OVERLAY_FEATURE_OPTIONS='index=off,nfs_export=off,metacopy=off,xino=off'
readonly OVERLAY_ROOT_OPTIONS='uuid=off,redirect_dir=nofollow'
readonly KMEMLEAK_PATH=/sys/kernel/debug/kmemleak
readonly KMEMLEAK_MIN_AGE_SECONDS=6
readonly KMEMLEAK_SCAN_SETTLE_SECONDS=2
readonly BASE_NAME=deltafs-p5-base-only
readonly BASE_PAYLOAD=deltafs-p5-base

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
checkpoint_test="$script_dir/p5_checkpoint_test"
backing_root=
run_dir=
merged=
module_loaded=0
marker_started=0
marker_finished=0
marker="deltafs-p5-checkpoint[$$]"
mount_options=
# kmemleak is an optional leak check, not a functional prerequisite.  When the
# running kernel lacks it the scan is skipped and the harness exits SKIP (4).
have_kmemleak=0

usage()
{
	cat <<'EOF'
Usage: p5_checkpoint_test.sh --backing-root PATH

PATH must be a writable directory on the backing filesystem used for every
active, checkpoint, and branch directory.  Run only inside the project
QEMU/KVM guest with the P5 kernel/module installed.  The guest must have no
pre-existing OverlayFS mounts; this harness unloads overlay when it finishes.
EOF
}

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

note_skip()
{
	printf 'SKIP: %s\n' "$*"
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
	if grep -Eiq '(^|[^[:alnum:]])(qemu|kvm)([^[:alnum:]]|$)' \
		<<< "$evidence"; then
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
	if ((module_loaded != 0 && still_mounted == 0)); then
		if ! modprobe -r "$OVERLAY_MODULE"; then
			printf 'FAIL: cleanup could not unload %s\n' \
				"$OVERLAY_MODULE" >&2
			status=1
		else
			module_loaded=0
		fi
	fi
	finish_marker
	if ((still_mounted == 0)) && [[ -n "$run_dir" && -d "$run_dir" ]]; then
		rm -rf -- "$run_dir"
	fi
	exit "$status"
}

scan_kmemleak()
{
	local report="$run_dir/kmemleak.log"

	if ((have_kmemleak != 1)); then
		note_skip "kmemleak scan skipped ($KMEMLEAK_PATH unavailable)"
		return
	fi

	printf 'Waiting %ss for kmemleak minimum object age\n' \
		"$KMEMLEAK_MIN_AGE_SECONDS"
	sleep "$KMEMLEAK_MIN_AGE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	cp -- "$KMEMLEAK_PATH" "$report" || \
		die 'could not save the kmemleak report'
	if [[ -s "$report" ]]; then
		cat -- "$report" >&2
		die 'kmemleak reported unreferenced objects after multi-commit teardown'
	fi
	printf 'PASS: kmemleak report is empty after multi-commit teardown\n'
}

scan_kernel_window()
{
	local diagnostics
	local window

	diagnostics='BUG:|WARNING:|KASAN:|KFENCE:|UBSAN:|use-after-free|double free'
	diagnostics+='|refcount_t:|possible circular locking dependency'
	diagnostics+='|suspicious RCU usage|kernel NULL pointer'
	diagnostics+='|general protection fault|unable to handle kernel'
	diagnostics+='|kmemleak: [1-9][0-9]* new suspected memory leaks'

	finish_marker
	window=$(dmesg | awk -v begin="$marker BEGIN" -v end="$marker END" '
		index($0, begin) { capture = 1; found_begin = 1 }
		capture { print }
		capture && index($0, end) { found_end = 1; exit }
		END { if (!found_begin || !found_end) exit 1 }
	') || die 'could not isolate the P5 checkpoint kernel log window'

	if grep -Eiq "$diagnostics" <<< "$window"; then
		printf '%s\n' "$window" >&2
		die 'kernel diagnostics reported a P5 multi-commit failure'
	fi
	printf 'PASS: P5 checkpoint log has no sanitizer/refcount/lockdep failure\n'
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
require_command cp
require_command dmesg
require_command findmnt
require_command grep
require_command modinfo
require_command modprobe
require_command mount
require_command mountpoint
require_command mkdir
require_command mktemp
require_command realpath
require_command rm
require_command sleep
require_command stat
require_command tr
require_command umount
confirm_qemu_kvm_guest

[[ -x "$checkpoint_test" ]] || \
	die "build the test first: make -C $script_dir p5_checkpoint_test"
backing_root=$(realpath -e -- "$backing_root")
[[ -d "$backing_root" && -w "$backing_root" ]] || \
	die "backing root is not a writable directory: $backing_root"
[[ "$backing_root" != *:* && "$backing_root" != *,* ]] || \
	die 'backing root cannot contain colon or comma characters'
case $(stat -f -c %T -- "$backing_root") in
overlay|overlayfs)
	die 'backing root itself must not be OverlayFS'
	;;
esac

existing_mounts=$(findmnt -rn -t overlay || true)
if [[ -n "$existing_mounts" ]]; then
	printf '%s\n' "$existing_mounts" >&2
	die 'refusing to run with pre-existing OverlayFS mounts'
fi
module_path=$(modinfo -F filename "$OVERLAY_MODULE" 2>/dev/null) || \
	die "cannot locate the $OVERLAY_MODULE module"
[[ -n "$module_path" && "$module_path" != '(builtin)' && -f "$module_path" ]] || \
	die "$OVERLAY_MODULE must be provided as a loadable module"
if [[ -r "$KMEMLEAK_PATH" && -w "$KMEMLEAK_PATH" ]]; then
	have_kmemleak=1
else
	note_skip "$KMEMLEAK_PATH is unavailable; kmemleak scan will be skipped"
fi
[[ -w /dev/kmsg ]] || die '/dev/kmsg must be writable for an isolated log window'
dmesg >/dev/null 2>&1 || die 'kernel log is not readable through dmesg'

run_dir=$(mktemp -d "$backing_root/deltafs-p5-checkpoint.XXXXXX")
merged="$run_dir/merged"
mkdir -p -- \
	"$run_dir/active-upper" \
	"$run_dir/active-work" \
	"$run_dir/active-lower" \
	"$run_dir/scratch" \
	"$merged"
printf '%s\n' "$BASE_PAYLOAD" > "$run_dir/active-lower/$BASE_NAME"
mount_options="lowerdir=$run_dir/active-lower"
mount_options+=",upperdir=$run_dir/active-upper"
mount_options+=",workdir=$run_dir/active-work,$OVERLAY_FEATURE_OPTIONS"
mount_options+=",$OVERLAY_ROOT_OPTIONS"

trap cleanup EXIT
if [[ -d /sys/module/$OVERLAY_MODULE ]]; then
	modprobe -r "$OVERLAY_MODULE" || \
		die "could not establish an unloaded $OVERLAY_MODULE baseline"
	printf 'PASS: preloaded overlay module removed before testing\n'
fi
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1
if ((have_kmemleak)); then
	printf 'clear\n' > "$KMEMLEAK_PATH"
	printf 'PASS: kmemleak state cleared before checkpoint testing\n'
else
	note_skip 'kmemleak baseline clear skipped (kmemleak unavailable)'
fi
modprobe "$OVERLAY_MODULE"
module_loaded=1

mount -t overlay overlay \
	-o "$mount_options" \
	"$merged"

"$checkpoint_test" \
	"$merged" \
	"$run_dir/scratch" \
	"$run_dir/active-upper" \
	"$run_dir/active-lower"

umount -- "$merged"
printf 'PASS: unmount released the active and three retired P5 views\n'
modprobe -r "$OVERLAY_MODULE"
module_loaded=0
printf 'PASS: overlay module unloaded after multi-commit teardown\n'

scan_kmemleak
scan_kernel_window

rm -rf -- "$run_dir"
run_dir=
if ((have_kmemleak)); then
	printf 'All P5 checkpoint/multi-commit QEMU tests passed\n'
	exit 0
fi
printf 'P5 checkpoint/multi-commit tests completed; kmemleak scan skipped (exit SKIP)\n'
exit 4
