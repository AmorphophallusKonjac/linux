#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P2 passive-state lifecycle test.
#
# This test intentionally performs repeated OverlayFS module load/unload and
# mount failure injection.  It must only run as root in a QEMU/KVM guest.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly KMEMLEAK_PATH=/sys/kernel/debug/kmemleak
readonly KMEMLEAK_MIN_AGE_SECONDS=6
readonly KMEMLEAK_SCAN_SETTLE_SECONDS=2

backing_root=
module_cycles=10
mount_cycles=10
run_dir=
results_dir=
summary_log=
kernel_config=
kernel_config_gzip=0
marker=
marker_started=0
marker_closed=0
kernel_log_captured=0
module_loaded_by_test=0
pass_count=0
failure_count=0
error_reported=0
declare -a owned_mountpoints=()

usage()
{
	cat <<'EOF'
Usage: p2_lifecycle_test.sh --backing-root PATH [OPTIONS]

Required:
  --backing-root PATH       Writable, non-OverlayFS backing filesystem path

Options:
  --module-cycles N         Module load/unload cycles (default: 10)
  --mount-cycles N          Writable/lower-only pairs per module cycle
                            (default: 10)
  -h, --help                Show this help
EOF
}

log()
{
	printf '%s\n' "$*"
	if [[ -n "$summary_log" ]]; then
		printf '%s\n' "$*" >> "$summary_log"
	fi
}

record_pass()
{
	pass_count=$((pass_count + 1))
	log "PASS: $*"
}

record_failure()
{
	failure_count=$((failure_count + 1))
	log "FAIL: $*"
}

die()
{
	error_reported=1
	record_failure "$*"
	exit 1
}

on_error()
{
	local status=$1
	local line=$2

	if ((error_reported == 0)); then
		error_reported=1
		record_failure "unexpected command failure at line $line (status $status)"
	fi
	exit "$status"
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

capture_kernel_log()
{
	local full_log="$results_dir/dmesg-full.log"
	local window_log="$results_dir/dmesg.log"
	local begin="$marker BEGIN"
	local end="$marker END"

	if ((marker_started == 0)); then
		return 1
	fi
	if ((marker_closed == 0)); then
		printf '%s\n' "$end" > /dev/kmsg || return 1
		marker_closed=1
	fi

	dmesg > "$full_log" 2>&1 || return 1
	awk -v begin="$begin" -v end="$end" '
		index($0, begin) { capture = 1; saw_begin = 1 }
		capture { print }
		capture && index($0, end) { saw_end = 1; exit }
		END { if (!saw_begin || !saw_end) exit 1 }
	' "$full_log" > "$window_log" || return 1

	kernel_log_captured=1
	return 0
}

cleanup()
{
	local original_status=$?
	local cleanup_failures=0
	local i count

	trap - ERR EXIT
	set +e

	for ((i = ${#owned_mountpoints[@]} - 1; i >= 0; i--)); do
		if mountpoint -q -- "${owned_mountpoints[i]}"; then
			if ! umount -- "${owned_mountpoints[i]}"; then
				log "FAIL: cleanup could not unmount ${owned_mountpoints[i]}"
				cleanup_failures=$((cleanup_failures + 1))
			fi
		fi
	done

	if ((module_loaded_by_test != 0)) && [[ -d /sys/module/$OVERLAY_MODULE ]]; then
		count=$(overlay_mount_count)
		if ((count == 0)); then
			if ! modprobe -r "$OVERLAY_MODULE"; then
				log "FAIL: cleanup could not unload $OVERLAY_MODULE without force"
				cleanup_failures=$((cleanup_failures + 1))
			fi
		else
			log "FAIL: cleanup left $OVERLAY_MODULE loaded because OverlayFS mounts exist"
			cleanup_failures=$((cleanup_failures + 1))
		fi
	fi

	if [[ -n "$run_dir" && -d "$run_dir" ]]; then
		if ! rm -rf -- "$run_dir"; then
			log "FAIL: cleanup could not remove test directory $run_dir"
			cleanup_failures=$((cleanup_failures + 1))
		fi
	fi

	if ((marker_started != 0 && kernel_log_captured == 0)); then
		if ! capture_kernel_log; then
			log "FAIL: could not capture the kernel log marker window"
			cleanup_failures=$((cleanup_failures + 1))
		fi
	fi

	failure_count=$((failure_count + cleanup_failures))
	log "RESULT: passed=$pass_count failed=$failure_count"
	if [[ -n "$results_dir" ]]; then
		log "kmemleak log: $results_dir/kmemleak.log"
		log "kernel log:   $results_dir/dmesg.log"
	fi

	if ((original_status != 0 || failure_count != 0)); then
		exit 1
	fi
	exit 0
}

trap 'on_error $? $LINENO' ERR
trap cleanup EXIT

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
			record_pass "QEMU/KVM guest detected by systemd-detect-virt ($detected)"
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
		record_pass "QEMU/KVM guest detected from DMI/device-tree evidence"
		return
	fi

	die "refusing to run: this system is not confirmed as a QEMU/KVM guest"
}

select_kernel_config()
{
	local candidate

	if [[ -r /proc/config.gz ]]; then
		require_command zgrep
		kernel_config=/proc/config.gz
		kernel_config_gzip=1
		return
	fi

	for candidate in \
		"/boot/config-$(uname -r)" \
		"/lib/modules/$(uname -r)/build/.config"; do
		if [[ -r "$candidate" ]]; then
			kernel_config=$candidate
			return
		fi
	done

	die "cannot read the running kernel configuration"
}

require_kernel_config()
{
	local key=$1
	local value=$2
	local setting="$key=$value"

	if ((kernel_config_gzip != 0)); then
		zgrep -Fxq "$setting" "$kernel_config" || \
			die "running kernel requires $setting"
	else
		grep -Fxq "$setting" "$kernel_config" || \
			die "running kernel requires $setting"
	fi
}

mount_overlay()
{
	local options=$1
	local target=$2
	local description=$3

	if ! mount -t overlay overlay -o "$options" "$target"; then
		die "$description mount failed"
	fi
	owned_mountpoints+=("$target")
}

unmount_overlay()
{
	local target=$1
	local description=$2

	if ! umount -- "$target"; then
		die "$description unmount failed"
	fi
	if mountpoint -q -- "$target"; then
		die "$description remains mounted after umount"
	fi
}

run_writable_mount()
{
	local cycle=$1
	local iteration=$2
	local case_dir="$run_dir/module-$cycle/writable-$iteration"
	local lower="$case_dir/lower"
	local upper="$case_dir/upper"
	local work="$case_dir/work"
	local merged="$case_dir/merged"
	local lower_payload="lower-$cycle-$iteration"
	local upper_payload="upper-$cycle-$iteration"
	local actual

	mkdir -p -- "$lower" "$upper" "$work" "$merged"
	printf '%s' "$lower_payload" > "$lower/lower.txt"
	mount_overlay "lowerdir=$lower,upperdir=$upper,workdir=$work" "$merged" \
		"writable cycle $cycle/$iteration"

	actual=$(<"$merged/lower.txt")
	[[ "$actual" == "$lower_payload" ]] || \
		die "writable cycle $cycle/$iteration did not read lower data"
	printf '%s' "$upper_payload" > "$merged/upper.txt"
	actual=$(<"$upper/upper.txt")
	[[ "$actual" == "$upper_payload" ]] || \
		die "writable cycle $cycle/$iteration did not write upper data"

	unmount_overlay "$merged" "writable cycle $cycle/$iteration"
	record_pass "writable mount cycle $cycle/$iteration"
}

run_lower_only_mount()
{
	local cycle=$1
	local iteration=$2
	local case_dir="$run_dir/module-$cycle/lower-only-$iteration"
	local top_lower="$case_dir/top-lower"
	local bottom_lower="$case_dir/bottom-lower"
	local merged="$case_dir/merged"
	local top_payload="top-lower-$cycle-$iteration"
	local bottom_payload="bottom-lower-$cycle-$iteration"
	local actual

	mkdir -p -- "$top_lower" "$bottom_lower" "$merged"
	printf '%s' "$top_payload" > "$top_lower/top.txt"
	printf '%s' "$bottom_payload" > "$bottom_lower/bottom.txt"
	printf '%s' "$top_payload" > "$top_lower/shared.txt"
	printf '%s' "$bottom_payload" > "$bottom_lower/shared.txt"
	mount_overlay "lowerdir=$top_lower:$bottom_lower" "$merged" \
		"lower-only cycle $cycle/$iteration"

	actual=$(<"$merged/top.txt")
	[[ "$actual" == "$top_payload" ]] || \
		die "lower-only cycle $cycle/$iteration did not read the top lower"
	actual=$(<"$merged/bottom.txt")
	[[ "$actual" == "$bottom_payload" ]] || \
		die "lower-only cycle $cycle/$iteration did not read the bottom lower"
	actual=$(<"$merged/shared.txt")
	[[ "$actual" == "$top_payload" ]] || \
		die "lower-only cycle $cycle/$iteration used the wrong layer order"

	unmount_overlay "$merged" "lower-only cycle $cycle/$iteration"
	record_pass "lower-only mount cycle $cycle/$iteration"
}

run_missing_lower_failure()
{
	local cycle=$1
	local case_dir="$run_dir/module-$cycle/missing-lower"
	local valid="$case_dir/valid"
	local missing="$case_dir/does-not-exist"
	local merged="$case_dir/merged"

	mkdir -p -- "$valid" "$merged"
	if mount -t overlay overlay -o "lowerdir=$valid:$missing" "$merged" \
		2>> "$results_dir/expected-mount-failures.log"; then
		owned_mountpoints+=("$merged")
		die "missing-lower mount unexpectedly succeeded in module cycle $cycle"
	fi
	if mountpoint -q -- "$merged"; then
		die "missing-lower failure left a mount in module cycle $cycle"
	fi
	record_pass "fs-context/parameter failure in module cycle $cycle"
}

run_nested_workdir_failure()
{
	local cycle=$1
	local case_dir="$run_dir/module-$cycle/nested-workdir"
	local lower="$case_dir/lower"
	local upper="$case_dir/upper"
	local work="$upper/work"
	local merged="$case_dir/merged"

	mkdir -p -- "$lower" "$work" "$merged"
	printf '%s' "nested-workdir-$cycle" > "$lower/lower.txt"
	if mount -t overlay overlay \
		-o "lowerdir=$lower,upperdir=$upper,workdir=$work" "$merged" \
		2>> "$results_dir/expected-mount-failures.log"; then
		owned_mountpoints+=("$merged")
		die "nested-workdir mount unexpectedly succeeded in module cycle $cycle"
	fi
	if mountpoint -q -- "$merged"; then
		die "nested-workdir failure left a mount in module cycle $cycle"
	fi
	record_pass "post-upper setup failure in module cycle $cycle"
}

run_module_cycle()
{
	local cycle=$1
	local iteration count

	if ! modprobe "$OVERLAY_MODULE"; then
		die "could not load $OVERLAY_MODULE in module cycle $cycle"
	fi
	module_loaded_by_test=1
	[[ -d /sys/module/$OVERLAY_MODULE ]] || \
		die "$OVERLAY_MODULE sysfs entry missing after load in cycle $cycle"
	[[ -r /sys/module/$OVERLAY_MODULE/refcnt ]] || \
		die "$OVERLAY_MODULE does not expose an unloadable module refcount"

	for ((iteration = 1; iteration <= mount_cycles; iteration++)); do
		run_writable_mount "$cycle" "$iteration"
		run_lower_only_mount "$cycle" "$iteration"
	done

	run_missing_lower_failure "$cycle"
	run_nested_workdir_failure "$cycle"

	count=$(overlay_mount_count)
	if ((count != 0)); then
		show_overlay_mounts
		die "OverlayFS mounts remain before module unload in cycle $cycle"
	fi

	if ! modprobe -r "$OVERLAY_MODULE"; then
		die "could not unload $OVERLAY_MODULE without force in cycle $cycle"
	fi
	[[ ! -e /sys/module/$OVERLAY_MODULE ]] || \
		die "$OVERLAY_MODULE sysfs entry remains after unload in cycle $cycle"
	module_loaded_by_test=0
	record_pass "module load/unload cycle $cycle"
}

finish_kmemleak()
{
	local report="$results_dir/kmemleak.log"

	log "Waiting ${KMEMLEAK_MIN_AGE_SECONDS}s for kmemleak minimum object age"
	sleep "$KMEMLEAK_MIN_AGE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	if ! cp -- "$KMEMLEAK_PATH" "$report"; then
		die "could not save kmemleak report"
	fi

	if [[ -s "$report" ]]; then
		record_failure "kmemleak reported unreferenced objects"
	else
		record_pass "kmemleak report is empty"
	fi
}

check_kernel_log()
{
	local log_path="$results_dir/dmesg.log"
	local findings="$results_dir/dmesg-findings.log"
	local pattern

	pattern='BUG:|WARNING:|Oops:|KASAN:|KFENCE:|UBSAN:|use-after-free|double[- ]free|refcount_t:|refcount[^[:cntrl:]]*(underflow|saturat)|kernel BUG|general protection fault|possible circular locking dependency|inconsistent lock state|held lock freed|bad unlock balance|scheduling while atomic|sleeping function called from invalid context'
	if grep -Ein "$pattern" "$log_path" > "$findings"; then
		record_failure "kernel error signature found (see $findings)"
	else
		rm -f -- "$findings"
		record_pass "kernel log contains no lifecycle error signature"
	fi
}

while (($# > 0)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || die "--backing-root requires a path"
		backing_root=$2
		shift 2
		;;
	--module-cycles)
		(($# >= 2)) || die "--module-cycles requires a value"
		module_cycles=$2
		shift 2
		;;
	--mount-cycles)
		(($# >= 2)) || die "--mount-cycles requires a value"
		mount_cycles=$2
		shift 2
		;;
	-h|--help)
		trap - ERR EXIT
		usage
		exit 0
		;;
	*)
		usage >&2
		die "unknown argument: $1"
		;;
	esac
done

[[ -n "$backing_root" ]] || {
	usage >&2
	die "--backing-root is required"
}
[[ "$module_cycles" =~ ^[1-9][0-9]*$ ]] || \
	die "--module-cycles must be a positive integer"
[[ "$mount_cycles" =~ ^[1-9][0-9]*$ ]] || \
	die "--mount-cycles must be a positive integer"
((EUID == 0)) || die "this test requires root"

for command in awk cp date dmesg findmnt grep mkdir mktemp modinfo modprobe \
	mount mountpoint realpath rm sleep tr umount uname; do
	require_command "$command"
done

confirm_qemu_kvm_guest

canonical_root=$(realpath -e -- "$backing_root" 2>/dev/null) || \
	die "backing root does not exist: $backing_root"
backing_root=$canonical_root
[[ -d "$backing_root" ]] || die "backing root is not a directory: $backing_root"
[[ "$backing_root" != *:* && "$backing_root" != *,* && \
	"$backing_root" != *$'\n'* ]] || \
	die "backing root contains a character unsupported by OverlayFS mount options"

backing_fstype=$(findmnt -n -o FSTYPE -T "$backing_root" 2>/dev/null) || \
	die "cannot determine backing filesystem for $backing_root"
[[ "$backing_fstype" != overlay ]] || \
	die "backing root must not reside on OverlayFS"

select_kernel_config
require_kernel_config CONFIG_OVERLAY_FS m
require_kernel_config CONFIG_MODULE_UNLOAD y
require_kernel_config CONFIG_DEBUG_KERNEL y
require_kernel_config CONFIG_DEBUG_FS y
require_kernel_config CONFIG_DEBUG_KMEMLEAK y

module_path=$(modinfo -F filename "$OVERLAY_MODULE" 2>/dev/null) || \
	die "cannot locate the $OVERLAY_MODULE module"
[[ -n "$module_path" && "$module_path" != '(builtin)' && -f "$module_path" ]] || \
	die "$OVERLAY_MODULE must be provided as a loadable module"

initial_mounts=$(overlay_mount_count)
if ((initial_mounts != 0)); then
	show_overlay_mounts
	die "refusing to run with pre-existing OverlayFS mounts"
fi
[[ ! -e /sys/module/$OVERLAY_MODULE ]] || \
	die "$OVERLAY_MODULE is already loaded; unload it manually before this test"

debugfs_type=$(findmnt -n -o FSTYPE -T /sys/kernel/debug 2>/dev/null || true)
[[ "$debugfs_type" == debugfs ]] || die "debugfs is not mounted at /sys/kernel/debug"
[[ -r "$KMEMLEAK_PATH" && -w "$KMEMLEAK_PATH" ]] || \
	die "$KMEMLEAK_PATH must be readable and writable"
[[ -w /dev/kmsg ]] || die "/dev/kmsg must be writable"
dmesg >/dev/null 2>&1 || die "kernel log is not readable through dmesg"

run_dir=$(mktemp -d -p "$backing_root" .deltafs-p2-run.XXXXXX) || \
	die "backing root is not writable: $backing_root"
results_dir="$backing_root/deltafs-p2-results-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -- "$results_dir" || die "could not create results directory $results_dir"
summary_log="$results_dir/summary.log"
: > "$summary_log"

marker="deltafs-p2-$PPID-$$-$(date +%s)"
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1
printf 'clear\n' > "$KMEMLEAK_PATH"
record_pass "kmemleak state cleared before the first module cycle"

log "Running $module_cycles module cycle(s), $mount_cycles mount pair(s) each"
for ((cycle = 1; cycle <= module_cycles; cycle++)); do
	run_module_cycle "$cycle"
done

[[ ! -e /sys/module/$OVERLAY_MODULE ]] || \
	die "$OVERLAY_MODULE remains loaded after the final cycle"
final_mounts=$(overlay_mount_count)
((final_mounts == 0)) || die "OverlayFS mounts remain after the final cycle"

finish_kmemleak
capture_kernel_log || die "could not capture the kernel log marker window"
check_kernel_log

if ((failure_count != 0)); then
	exit 1
fi

log "DeltaFS P2 lifecycle test completed successfully"
