#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P6 checkpoint/restore controller QEMU/KVM harness.
#
# The primary scenario creates A -> B -> C and restores A into a fresh A'
# branch.  A pre-ioctl controller fault verifies synchronous compensation; a
# second mount crashes immediately after a successful ioctl and verifies the
# fail-stop transaction barrier.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly OVERLAY_FEATURE_OPTIONS='index=off,nfs_export=off,metacopy=off,xino=off'
readonly OVERLAY_ROOT_OPTIONS='uuid=off,redirect_dir=nofollow'
readonly KMEMLEAK_PATH=/sys/kernel/debug/kmemleak
readonly KMEMLEAK_MIN_AGE_SECONDS=6
readonly KMEMLEAK_SCAN_SETTLE_SECONDS=2

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
controller="$script_dir/deltafsctl"
test_controller="$script_dir/deltafsctl_test"
generation_test="$script_dir/p6_generation_test"
backing_root=
run_dir=
current_merged=
module_loaded=0
marker_started=0
marker_finished=0
marker="deltafs-p6-controller[$$]"
# kmemleak is an optional leak check, not a functional prerequisite.  When the
# running kernel lacks it the scan is skipped and the harness exits SKIP (4).
have_kmemleak=0

usage()
{
	cat <<'EOF'
Usage: p6_controller_test.sh --backing-root PATH

PATH must be a writable directory on the single backing filesystem used by
the DeltaFS sandbox.  Run only in the project QEMU/KVM guest with the P6
kernel/module and tools installed.  The guest must have no existing OverlayFS
mounts because the harness unloads overlay during teardown.
EOF
}

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

pass()
{
	printf 'PASS: %s\n' "$*"
}

note_skip()
{
	printf 'SKIP: %s\n' "$*"
}

require_command()
{
	command -v "$1" >/dev/null 2>&1 ||
		die "required command not found: $1"
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
			pass "QEMU/KVM guest detected ($detected)"
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
		pass 'QEMU/KVM guest detected from DMI/device-tree'
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
	if [[ -n "$current_merged" ]] &&
	   mountpoint -q -- "$current_merged"; then
		if ! umount -- "$current_merged"; then
			printf 'FAIL: cleanup could not unmount %s\n' \
				"$current_merged" >&2
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
	cp -- "$KMEMLEAK_PATH" "$report" ||
		die 'could not save the kmemleak report'
	if [[ -s "$report" ]]; then
		cat -- "$report" >&2
		die 'kmemleak reported objects after P6 controller teardown'
	fi
	pass 'kmemleak report is empty after P6 teardown'
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
	') || die 'could not isolate the P6 kernel log window'
	if grep -Eiq "$diagnostics" <<< "$window"; then
		printf '%s\n' "$window" >&2
		die 'kernel diagnostics reported a P6 controller failure'
	fi
	pass 'P6 kernel log has no sanitizer/refcount/lockdep failure'
}

init_sandbox()
{
	local root=$1

	mkdir -p -- \
		"$root/base" \
		"$root/layers" \
		"$root/branches/g1/upper" \
		"$root/branches/g1/work" \
		"$root/meta" \
		"$root/merged"
	printf 'deltafs-p6-base\n' > "$root/base/base-only"
	cat > "$root/meta/state.json" <<'EOF'
{
  "format": 1,
  "kernel_generation": 1,
  "active_branch": "g1",
  "active_lowers": ["base"],
  "retired_branches": [],
  "snapshots": {}
}
EOF
}

mount_sandbox()
{
	local root=$1
	local options

	options="lowerdir=$root/base"
	options+=",upperdir=$root/branches/g1/upper"
	options+=",workdir=$root/branches/g1/work"
	options+=",$OVERLAY_FEATURE_OPTIONS,$OVERLAY_ROOT_OPTIONS"
	mount -t overlay overlay -o "$options" "$root/merged"
	current_merged="$root/merged"
}

unmount_current()
{
	[[ -n "$current_merged" ]] || die 'internal: no current mount'
	umount -- "$current_merged"
	current_merged=
}

expect_contents()
{
	local path=$1
	local expected=$2
	local actual

	[[ -f "$path" ]] || die "expected regular file is missing: $path"
	actual=$(<"$path")
	[[ "$actual" == "$expected" ]] ||
		die "unexpected contents in $path: '$actual'"
}

expect_missing()
{
	[[ ! -e "$1" ]] || die "path unexpectedly exists: $1"
}

fingerprint()
{
	local path=$1

	{
		stat -c '%d:%i:%f:%s:%y' -- "$path"
		sha256sum -- "$path"
	}
}

expect_fingerprint()
{
	local path=$1
	local expected=$2
	local stage=$3
	local actual

	actual=$(fingerprint "$path")
	[[ "$actual" == "$expected" ]] ||
		die "$stage changed frozen file $path"
}

assert_clean_transaction()
{
	local root=$1

	expect_missing "$root/meta/transaction.json"
	expect_missing "$root/meta/.transaction.json.tmp"
	expect_missing "$root/meta/.state.json.tmp"
}

run_primary_scenario()
{
	local root=$1
	local a_state a_only b_state b_only c_state c_only base_fp

	mount_sandbox "$root"
	printf 'A\n' > "$root/merged/state"
	printf 'A-only\n' > "$root/merged/only-a"
	base_fp=$(fingerprint "$root/base/base-only")

	if DELTAFSCTL_TEST_FAILPOINT=before_ioctl \
		"$test_controller" --assume-quiesced \
		checkpoint "$root" rollback; then
		die 'injected pre-ioctl checkpoint unexpectedly succeeded'
	fi
	[[ -d "$root/branches/g1/upper" ]] ||
		die 'checkpoint rollback did not restore the active upper'
	expect_missing "$root/layers/rollback"
	expect_missing "$root/branches/g2"
	expect_contents "$root/merged/state" A
	grep -q '"kernel_generation": 1' "$root/meta/state.json" ||
		die 'rollback changed state generation'
	assert_clean_transaction "$root"
	pass 'pre-ioctl checkpoint failure restored tree, state and transaction'

	"$controller" --assume-quiesced checkpoint "$root" A
	a_state=$(fingerprint "$root/layers/A/state")
	a_only=$(fingerprint "$root/layers/A/only-a")
	expect_missing "$root/branches/g2/upper/state"
	assert_clean_transaction "$root"

	printf 'B\n' > "$root/merged/state"
	printf 'B-only\n' > "$root/merged/only-b"
	rm -- "$root/merged/only-a"
	"$controller" --assume-quiesced checkpoint "$root" B
	b_state=$(fingerprint "$root/layers/B/state")
	b_only=$(fingerprint "$root/layers/B/only-b")
	expect_fingerprint "$root/layers/A/state" "$a_state" checkpoint-B
	expect_fingerprint "$root/layers/A/only-a" "$a_only" checkpoint-B

	printf 'C\n' > "$root/merged/state"
	printf 'C-only\n' > "$root/merged/only-c"
	rm -- "$root/merged/only-b"
	"$controller" --assume-quiesced checkpoint "$root" C
	c_state=$(fingerprint "$root/layers/C/state")
	c_only=$(fingerprint "$root/layers/C/only-c")
	expect_fingerprint "$root/layers/A/state" "$a_state" checkpoint-C
	expect_fingerprint "$root/layers/B/state" "$b_state" checkpoint-C

	"$controller" --assume-quiesced restore "$root" A
	expect_contents "$root/merged/state" A
	expect_contents "$root/merged/only-a" A-only
	expect_missing "$root/merged/only-b"
	expect_missing "$root/merged/only-c"
	expect_contents "$root/merged/base-only" deltafs-p6-base

	printf 'A-prime\n' > "$root/merged/state"
	printf 'A-prime-only\n' > "$root/merged/only-a-prime"
	expect_contents "$root/branches/g5/upper/state" A-prime
	expect_contents "$root/branches/g5/upper/only-a-prime" A-prime-only

	expect_fingerprint "$root/layers/A/state" "$a_state" restore-A-prime
	expect_fingerprint "$root/layers/A/only-a" "$a_only" restore-A-prime
	expect_fingerprint "$root/layers/B/state" "$b_state" restore-A-prime
	expect_fingerprint "$root/layers/B/only-b" "$b_only" restore-A-prime
	expect_fingerprint "$root/layers/C/state" "$c_state" restore-A-prime
	expect_fingerprint "$root/layers/C/only-c" "$c_only" restore-A-prime
	expect_fingerprint "$root/base/base-only" "$base_fp" restore-A-prime

	grep -q '"kernel_generation": 5' "$root/meta/state.json" ||
		die 'controller state did not reach generation 5'
	grep -q '"active_branch": "g5"' "$root/meta/state.json" ||
		die 'controller state did not select g5'
	grep -Fq '"active_lowers": ["layers/A", "base"]' \
		"$root/meta/state.json" ||
		die 'restore did not persist checkpoint A lower chain'
	grep -Fq '"retired_branches": ["g1", "g2", "g3", "g4"]' \
		"$root/meta/state.json" ||
		die 'retired branch history is incomplete'
	assert_clean_transaction "$root"
	"$generation_test" "$root/merged" 5
	pass "A -> B -> C, restore A -> A' preserved every frozen layer"
	unmount_current
}

run_ambiguous_crash_scenario()
{
	local root=$1
	local status

	mount_sandbox "$root"
	printf 'crash-snapshot\n' > "$root/merged/state"
	set +e
	DELTAFSCTL_TEST_FAILPOINT=after_ioctl \
		"$test_controller" --assume-quiesced \
		checkpoint "$root" crash
	status=$?
	set -e
	[[ "$status" -eq 200 ]] ||
		die "after-ioctl crash returned $status instead of 200"

	[[ -f "$root/meta/transaction.json" ]] ||
		die 'after-ioctl crash lost transaction intent'
	grep -q '"kernel_generation": 1' "$root/meta/state.json" ||
		die 'crash scenario unexpectedly committed state.json'
	[[ -d "$root/layers/crash" && -d "$root/branches/g2/upper" ]] ||
		die 'crash scenario did not reach the committed backing layout'
	"$generation_test" "$root/merged" 2
	if "$controller" --assume-quiesced checkpoint "$root" blocked; then
		die 'controller ignored an unresolved transaction'
	fi
	expect_missing "$root/layers/blocked"
	expect_missing "$root/branches/g3"
	pass 'post-ioctl crash retained intent and blocked later operations'
	unmount_current
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
require_command cat
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
require_command sha256sum
require_command sleep
require_command stat
require_command tr
require_command umount
confirm_qemu_kvm_guest

[[ -x "$controller" && -x "$test_controller" &&
   -x "$generation_test" ]] ||
	die "build P6 tools first: make -C $script_dir"
backing_root=$(realpath -e -- "$backing_root")
[[ -d "$backing_root" && -w "$backing_root" ]] ||
	die "backing root is not writable: $backing_root"
[[ "$backing_root" != *:* && "$backing_root" != *,* ]] ||
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
module_path=$(modinfo -F filename "$OVERLAY_MODULE" 2>/dev/null) ||
	die "cannot locate the $OVERLAY_MODULE module"
[[ -n "$module_path" && "$module_path" != '(builtin)' &&
   -f "$module_path" ]] ||
	die "$OVERLAY_MODULE must be a loadable module"
if [[ -r "$KMEMLEAK_PATH" && -w "$KMEMLEAK_PATH" ]]; then
	have_kmemleak=1
else
	note_skip "$KMEMLEAK_PATH is unavailable; kmemleak scan will be skipped"
fi
[[ -w /dev/kmsg ]] ||
	die '/dev/kmsg must be writable for an isolated log window'
dmesg >/dev/null 2>&1 ||
	die 'kernel log is not readable through dmesg'

run_dir=$(mktemp -d "$backing_root/deltafs-p6-controller.XXXXXX")
init_sandbox "$run_dir/primary"
init_sandbox "$run_dir/crash"

trap cleanup EXIT
if [[ -d /sys/module/$OVERLAY_MODULE ]]; then
	modprobe -r "$OVERLAY_MODULE" ||
		die "could not unload preloaded $OVERLAY_MODULE"
	pass 'preloaded overlay module removed before P6 testing'
fi
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1
if ((have_kmemleak)); then
	printf 'clear\n' > "$KMEMLEAK_PATH"
	pass 'kmemleak state cleared before P6 testing'
else
	note_skip 'kmemleak baseline clear skipped (kmemleak unavailable)'
fi
modprobe "$OVERLAY_MODULE"
module_loaded=1

run_primary_scenario "$run_dir/primary"
run_ambiguous_crash_scenario "$run_dir/crash"

modprobe -r "$OVERLAY_MODULE"
module_loaded=0
pass 'overlay module unloaded after P6 scenarios'
scan_kmemleak
scan_kernel_window

rm -rf -- "$run_dir"
run_dir=
if ((have_kmemleak)); then
	printf 'All P6 checkpoint/restore controller tests passed\n'
	exit 0
fi
printf 'P6 controller tests completed; kmemleak scan skipped (exit SKIP)\n'
exit 4
