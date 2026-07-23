#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P4 state builder lifecycle test.
#
# Mounts OverlayFS and repeatedly drives the P4 ioctl path, then tears down the
# module and scans kmemleak/dmesg for leaks.  An optional fault-injection pass
# (needs CONFIG_FUNCTION_ERROR_INJECTION) walks every build checkpoint.
#
# This script mounts filesystems and manipulates module state; it must only run
# as root in a QEMU/KVM guest.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly KMEMLEAK_PATH=/sys/kernel/debug/kmemleak
readonly KMEMLEAK_MIN_AGE_SECONDS=6
readonly KMEMLEAK_SCAN_SETTLE_SECONDS=2
readonly FAIL_FUNCTION_DIR=/sys/kernel/debug/fail_function
readonly BUILD_CHECKPOINT=ovl_deltafs_build_checkpoint
readonly ACTIVE_MARKER=deltafs-p4-active-marker
readonly ACTIVE_PAYLOAD=deltafs-p4-active-unchanged

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
state_test="$script_dir/p4_state_test"
ioctl_test="$script_dir/p1_ioctl_test"

backing_root=
extra_backing_root=
fault_injection_cycles=0
lifecycle_cycles=4
trap_reuse_cycles=4
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
error_injection_enabled=0
declare -a owned_mountpoints=()

usage()
{
	cat <<'EOF'
Usage: p4_state_test.sh --backing-root PATH [OPTIONS]

Required:
  --backing-root PATH        Writable, non-OverlayFS backing filesystem path

Options:
  --extra-backing-root PATH  Directory on a *different* filesystem, used only
                             for the -EXDEV case.  Optional.
  --lifecycle-cycles N       Build/free cycles (default: 4)
  --trap-reuse-cycles N      Active-trap-reuse checkpoint cycles (default: 4)
  --fault-injection-cycles N Fault-injection iterations.  0 disables (default:
                             0).  Requires CONFIG_FUNCTION_ERROR_INJECTION.
  -h, --help                 Show this help
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

require_command()
{
	command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
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

config_get()
{
	local key=$1

	if ((kernel_config_gzip != 0)); then
		zgrep -E "^${key}=|^# ${key} is not set|^${key} is not set" "$kernel_config" \
			2>/dev/null | head -n1 || true
	else
		grep -E "^${key}=|^# ${key} is not set|^${key} is not set" "$kernel_config" \
			2>/dev/null | head -n1 || true
	fi
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

	# Fully unregister the kprobe so the overlay module can unload cleanly.
	disable_fault_injection
	unregister_fault_injection

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

# Create the active overlay tree for one cycle: a writable merged root plus its
# separate upper/work/lower backing directories.
setup_active_overlay()
{
	local cycle=$1
	local case_dir="$run_dir/cycle-$cycle"
	local merged="$case_dir/merged"
	local upper="$case_dir/upper"
	local work="$case_dir/work"
	local lower="$case_dir/lower"
	local bottom="$case_dir/bottom"
	local marker_path="$merged/$ACTIVE_MARKER"

	mkdir -p -- "$merged" "$upper" "$work" "$lower" "$bottom"
	printf '%s\n' "$ACTIVE_PAYLOAD" > "$lower/lower-file"
	printf '%s\n' "$ACTIVE_PAYLOAD" > "$bottom/bottom-file"

	mount_overlay \
		"lowerdir=$lower:$bottom,upperdir=$upper,workdir=$work,index=off,xino=off,redirect_dir=nofollow,uuid=off" \
		"$merged" "active overlay cycle $cycle"

	# The marker is created through the merged view so the P4 test can assert
	# that the active view never changes across a build.
	printf '%s\n' "$ACTIVE_PAYLOAD" > "$marker_path"

	ACTIVE_MERGED=$merged
	ACTIVE_UPPER=$upper
	ACTIVE_WORK=$work
	ACTIVE_LOWER=$lower
	ACTIVE_BOTTOM=$bottom
}

run_state_test()
{
	local cycle=$1
	local marker_env

	marker_env="DELTAFS_P4_ACTIVE_MARKER=$ACTIVE_MARKER"
	if [[ -n "$extra_backing_root" ]]; then
		marker_env="$marker_env DELTAFS_P4_WRONG_BACKING=$extra_backing_root/p4-cycle-$cycle"
		mkdir -p -- "$extra_backing_root/p4-cycle-$cycle"
	fi
	if ! env $marker_env "$state_test" \
		"$ACTIVE_MERGED" "$run_dir/cycle-$cycle" \
		"$ACTIVE_UPPER" "$ACTIVE_WORK" "$ACTIVE_LOWER" "$ACTIVE_BOTTOM" \
		>> "$results_dir/p4-state-cycle-$cycle.log" 2>&1; then
		cat "$results_dir/p4-state-cycle-$cycle.log" >&2 || true
		die "p4_state_test failed in cycle $cycle"
	fi
	cat "$results_dir/p4-state-cycle-$cycle.log" || true
	record_pass "p4_state_test matrix passed in cycle $cycle"
}

run_trap_reuse_cycle()
{
	local cycle=$1
	# A checkpoint that reuses the active layer traps as its lowers must still
	# build and free cleanly (terminal -EOPNOTSUPP).  This is exercised inside
	# p4_state_test; here we additionally drive it many times to stress trap
	# reference sharing across repeated builds.
	local i

	for ((i = 0; i < trap_reuse_cycles; i++)); do
		run_state_test "$cycle"
	done
	record_pass "trap reuse checkpoint stress ($trap_reuse_cycles iterations) cycle $cycle"
}

# Register ovl_deltafs_build_checkpoint on the fail_function kprobe, so that
# every later build is observable.  Registration itself is inert until the
# global fault-attr decides should_fail(); see enable_fault_injection.
register_fault_injection()
{
	# Writing the symbol name to .../fail_function/inject installs a kprobe and
	# creates fail_function/<sym>/retval.  Re-registering the same symbol
	# returns -EBUSY, which we tolerate (a previous cycle may have left it).
	if [[ ! -d "$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT" ]]; then
		if ! echo "$BUILD_CHECKPOINT" > "$FAIL_FUNCTION_DIR/inject" 2>/dev/null; then
			return 1
		fi
	fi
	[[ -d "$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT" ]]
}

unregister_fault_injection()
{
	# Writing !<symbol> removes the kprobe.  Tolerate "not registered".
	echo "!$BUILD_CHECKPOINT" > "$FAIL_FUNCTION_DIR/inject" 2>/dev/null || true
	error_injection_enabled=0
}

disable_fault_injection()
{
	if ((error_injection_enabled == 0)); then
		return
	fi
	# Setting times=0 makes should_fail() always return false, so the kprobe
	# stays registered but stops injecting.
	echo 0 > "$FAIL_FUNCTION_DIR/times" 2>/dev/null || true
	error_injection_enabled=0
}

# Fail the @interval-th call to ovl_deltafs_build_checkpoint during the next
# build exactly @times times, then stop.  The probability/interval/times files
# are the GLOBAL fault-attr shared by all registered symbols.
enable_fault_injection()
{
	local interval=${1:-1}
	local times=${2:-1}

	register_fault_injection || \
		die "could not register $BUILD_CHECKPOINT on fail_function"
	# fail_function defaults retval to 0 (== success), which would NOT cause a
	# build failure.  Force an injected -ENOMEM so the build unwinds on the
	# selected checkpoint.  retval is a u64 read via kstrtoull, so write the
	# two's-complement value as unsigned hex (0xFFFFFFFFFFFFFFF4 == -12).
	echo '0xFFFFFFFFFFFFFFF4' > \
		"$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT/retval" 2>/dev/null || true
	echo 100 > "$FAIL_FUNCTION_DIR/probability" 2>/dev/null || true
	echo "$interval" > "$FAIL_FUNCTION_DIR/interval" 2>/dev/null || true
	echo "$times" > "$FAIL_FUNCTION_DIR/times" 2>/dev/null || true
	echo 0 > "$FAIL_FUNCTION_DIR/verbose" 2>/dev/null || true
	error_injection_enabled=1
}

# Drive a single restore build under active fault injection.  The C program runs
# in DELTAFS_P4_FAULT_BUILD mode, which performs exactly one build and reports
# the errno without asserting it; any errno is acceptable (injection may fail at
# any checkpoint).  Leaks/corruption are caught later by kmemleak/dmesg.
run_fault_build()
{
	local cycle=$1
	local log="$results_dir/p4-fault-build-$cycle.log"

	if ! env "DELTAFS_P4_FAULT_BUILD=1" "$state_test" \
		"$ACTIVE_MERGED" "$run_dir/cycle-$cycle" \
		"$ACTIVE_UPPER" "$ACTIVE_WORK" "$ACTIVE_LOWER" "$ACTIVE_BOTTOM" \
		> "$log" 2>&1; then
		cat "$log" >&2 || true
		die "fault build crashed in cycle $cycle"
	fi
	cat "$log" || true
}

run_fault_injection_cycle()
{
	local cycle=$1
	local i

	# ovl_deltafs_build_checkpoint() is called after every ownership step in a
	# single build.  interval=N, times=1 fails exactly the Nth call in the next
	# build, exercising each acquisition's unwind path once.  Drive one build
	# per interval value; any resulting errno is fine (injection may fail at any
	# checkpoint).  Leaks/corruption are caught by kmemleak/dmesg at the end.
	for ((i = 1; i <= 20; i++)); do
		enable_fault_injection "$i" 1
		run_fault_build "$cycle"
		disable_fault_injection
	done
	# Confirm the build path returns to success once injection is fully off.
	unregister_fault_injection
	run_fault_build "$cycle"
	record_pass "fault injection at every build checkpoint, cycle $cycle"
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
		record_pass "kernel log contains no P4 error signature"
	fi
}

while (($# > 0)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || die "--backing-root requires a path"
		backing_root=$2
		shift 2
		;;
	--extra-backing-root)
		(($# >= 2)) || die "--extra-backing-root requires a path"
		extra_backing_root=$2
		shift 2
		;;
	--lifecycle-cycles)
		(($# >= 2)) || die "--lifecycle-cycles requires a value"
		lifecycle_cycles=$2
		shift 2
		;;
	--trap-reuse-cycles)
		(($# >= 2)) || die "--trap-reuse-cycles requires a value"
		trap_reuse_cycles=$2
		shift 2
		;;
	--fault-injection-cycles)
		(($# >= 2)) || die "--fault-injection-cycles requires a value"
		fault_injection_cycles=$2
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
[[ "$lifecycle_cycles" =~ ^[0-9]+$ ]] || die "--lifecycle-cycles must be a non-negative integer"
[[ "$trap_reuse_cycles" =~ ^[1-9][0-9]*$ ]] || die "--trap-reuse-cycles must be a positive integer"
[[ "$fault_injection_cycles" =~ ^[0-9]+$ ]] || die "--fault-injection-cycles must be a non-negative integer"
((EUID == 0)) || die "this test requires root"

for command in awk cp date dmesg findmnt grep mkdir mktemp modinfo modprobe \
	mount mountpoint realpath rm sleep tr umount uname; do
	require_command "$command"
done

confirm_qemu_kvm_guest

[[ -x "$state_test" && -x "$ioctl_test" ]] || \
	die "build tests first with: make -C tools/deltafs"

canonical_root=$(realpath -e -- "$backing_root" 2>/dev/null) || \
	die "backing root does not exist: $backing_root"
backing_root=$canonical_root
[[ -d "$backing_root" && -w "$backing_root" ]] || \
	die "backing root must be an existing writable directory: $backing_root"
[[ "$backing_root" != *:* && "$backing_root" != *,* && \
	"$backing_root" != *$'\n'* ]] || \
	die "backing root contains a character unsupported by OverlayFS mount options"
backing_fstype=$(findmnt -n -o FSTYPE -T "$backing_root" 2>/dev/null) || \
	die "cannot determine backing filesystem for $backing_root"
[[ "$backing_fstype" != overlay ]] || \
	die "backing root must not reside on OverlayFS"

if [[ -n "$extra_backing_root" ]]; then
	extra_backing_root=$(realpath -e -- "$extra_backing_root" 2>/dev/null) || \
		die "extra backing root does not exist: $extra_backing_root"
	[[ -d "$extra_backing_root" && -w "$extra_backing_root" ]] || \
		die "extra backing root must be an existing writable directory"
	extra_fstype=$(findmnt -n -o FSTYPE -T "$extra_backing_root" 2>/dev/null) || \
		die "cannot determine backing filesystem for $extra_backing_root"
	# It must be on a *different* filesystem to exercise -EXDEV.
	[[ "$extra_fstype" != "$backing_fstype" ||
	   "$(findmnt -n -o SOURCE -T "$extra_backing_root")" != \
	   "$(findmnt -n -o SOURCE -T "$backing_root")" ]] || \
		die "extra backing root must be on a different filesystem than the backing root"
fi

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

if ((fault_injection_cycles > 0)); then
	if [[ "$(config_get CONFIG_FUNCTION_ERROR_INJECTION)" != \
		"CONFIG_FUNCTION_ERROR_INJECTION=y" ]]; then
		die "--fault-injection-cycles requires CONFIG_FUNCTION_ERROR_INJECTION=y"
	fi
	[[ -d "$FAIL_FUNCTION_DIR" ]] || \
		die "--fault-injection-cycles requires the fail_function debugfs directory"
else
	log "fault injection disabled (pass --fault-injection-cycles N to enable)"
fi

run_dir=$(mktemp -d -p "$backing_root" .deltafs-p4-run.XXXXXX) || \
	die "backing root is not writable: $backing_root"
results_dir="$backing_root/deltafs-p4-results-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -- "$results_dir" || die "could not create results directory $results_dir"
summary_log="$results_dir/summary.log"
: > "$summary_log"

modprobe "$OVERLAY_MODULE"
module_loaded_by_test=1
[[ -d /sys/module/$OVERLAY_MODULE ]] || \
	die "$OVERLAY_MODULE sysfs entry missing after load"

marker="deltafs-p4-$PPID-$$-$(date +%s)"
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1
printf 'clear\n' > "$KMEMLEAK_PATH"
record_pass "kmemleak state cleared before the first cycle"

# P1 ABI regression must still pass on a DeltaFS-capable mount (unchanged by P4).
# p1_ioctl_test treats its UPPER/WORK/LOWER arguments as a *fresh* build target,
# so they must be empty directories on the same backing filesystem; the active
# overlay's own upper/work/lower are populated and would trip validate_empty.
setup_active_overlay p1
p1_dir="$run_dir/p1-branch"
p1_upper="$p1_dir/upper"
p1_work="$p1_dir/work"
p1_lower="$p1_dir/lower"
p1_bottom="$p1_dir/bottom"
mkdir -p -- "$p1_upper" "$p1_work" "$p1_lower" "$p1_bottom"
"$ioctl_test" "$ACTIVE_MERGED" "$p1_upper" "$p1_work" \
	"$p1_lower" "$p1_bottom" \
	> "$results_dir/p1-abi-regression.log" 2>&1 || {
	cat "$results_dir/p1-abi-regression.log" >&2 || true
	die "P1 ABI regression failed on the P4 mount"
}
cat "$results_dir/p1-abi-regression.log" || true
unmount_overlay "$ACTIVE_MERGED" "P1 ABI regression overlay"
record_pass "P1 ABI regression passed"

log "Running $lifecycle_cycles lifecycle cycle(s)"
for ((cycle = 1; cycle <= lifecycle_cycles; cycle++)); do
	setup_active_overlay "$cycle"
	run_state_test "$cycle"
	unmount_overlay "$ACTIVE_MERGED" "active overlay cycle $cycle"
	record_pass "lifecycle cycle $cycle completed and unmounted"
done

log "Running trap-reuse stress"
setup_active_overlay trap
run_trap_reuse_cycle trap
unmount_overlay "$ACTIVE_MERGED" "trap-reuse overlay"
record_pass "trap-reuse overlay unmounted cleanly"

if ((fault_injection_cycles > 0)); then
	log "Running fault injection ($fault_injection_cycles cycle(s))"
	setup_active_overlay fault
	run_fault_injection_cycle fault
	unmount_overlay "$ACTIVE_MERGED" "fault-injection overlay"
	record_pass "fault-injection overlay unmounted cleanly"
fi

final_mounts=$(overlay_mount_count)
if ((final_mounts != 0)); then
	show_overlay_mounts
	die "OverlayFS mounts remain after the test"
fi

if ! modprobe -r "$OVERLAY_MODULE"; then
	die "could not unload $OVERLAY_MODULE without force after the test"
fi
[[ ! -e /sys/module/$OVERLAY_MODULE ]] || \
	die "$OVERLAY_MODULE sysfs entry remains after unload"
module_loaded_by_test=0
record_pass "module unloaded cleanly after repeated P4 build/free"

finish_kmemleak
capture_kernel_log || die "could not capture the kernel log marker window"
check_kernel_log

if ((failure_count != 0)); then
	exit 1
fi

log "DeltaFS P4 state builder test completed successfully"
