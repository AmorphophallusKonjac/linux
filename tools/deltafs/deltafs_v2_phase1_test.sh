#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Run the DeltaFS v2 phase-one guest checks as one reproducible command.

set -Eeuo pipefail
export LC_ALL=C

readonly PROGRAM=${0##*/}
readonly MODULE=overlay
readonly CHECKPOINT_SYMBOL=ovl_deltafs_build_checkpoint
readonly DEBUGFS=/sys/kernel/debug
readonly FAIL_FUNCTION="$DEBUGFS/fail_function"

backing_root=
fault_mode=auto
max_fault_nth=512

usage()
{
	cat >&2 <<EOF
Usage: $PROGRAM --backing-root PATH [--no-fault-injection]
       $PROGRAM --backing-root PATH --require-fault-injection
       $PROGRAM --backing-root PATH --max-fault-nth N

The script runs inside the QEMU guest. PATH must be writable. Results and
diagnostics remain below PATH.
EOF
}

log()
{
	printf '[deltafs-v2] %s\n' "$*"
}

die()
{
	log "FAIL: $*"
	exit 1
}

while (($#)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || { usage; exit 2; }
		backing_root=$2
		shift 2
		;;
	--no-fault-injection)
		fault_mode=disabled
		shift
		;;
	--require-fault-injection)
		fault_mode=required
		shift
		;;
	--max-fault-nth)
		(($# >= 2)) || { usage; exit 2; }
		max_fault_nth=$2
		shift 2
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		usage
		exit 2
		;;
	esac
done

[[ -n "$backing_root" ]] || { usage; exit 2; }
[[ "$max_fault_nth" =~ ^[1-9][0-9]*$ ]] ||
	die "--max-fault-nth must be a positive integer"
((EUID == 0)) || die "run as root (or through sudo)"

script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(cd -- "$script_dir/../.." && pwd)
helper="$script_dir/deltafs_v2_ioctl_test"
layout_test="$script_dir/deltafs_v2_layout_test"
module_path="$repo_root/fs/overlayfs/overlay.ko"

for command in awk cat cp date dmesg findmnt grep install lsmod make mkdir \
	mount mountpoint modprobe rm sed sync tee umount uname; do
	command -v "$command" >/dev/null 2>&1 || die "missing command: $command"
done

[[ -d "$backing_root" ]] || die "backing root is not a directory: $backing_root"
[[ -w "$backing_root" ]] || die "backing root is not writable: $backing_root"
if findmnt -rn -t overlay | grep -q .; then
	die "an overlay mount already exists; start a clean guest"
fi

run_dir="$backing_root/results-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$run_dir"
runner_log="$run_dir/runner.log"
exec > >(tee -a "$runner_log") 2>&1

mounted=
module_loaded=0
fault_skipped=0
fault_added=0

disable_fault_injection()
{
	if [[ -d "$FAIL_FUNCTION" ]]; then
		printf '0\n' > "$FAIL_FUNCTION/times" 2>/dev/null || true
	fi
}

unregister_fault_injection()
{
	if ((fault_added)); then
		printf '!%s\n' "$CHECKPOINT_SYMBOL" > \
			"$FAIL_FUNCTION/inject" 2>/dev/null || true
		fault_added=0
	fi
}

collect_diagnostics()
{
	dmesg -T > "$run_dir/dmesg-full.log" 2>/dev/null || true
	findmnt -J > "$run_dir/findmnt.json" 2>/dev/null || true
	cat /proc/mounts > "$run_dir/proc-mounts.txt" 2>/dev/null || true
	cat "$DEBUGFS/kmemleak" > "$run_dir/kmemleak.log" 2>/dev/null || true
	if [[ -r /proc/config.gz ]]; then
		cat /proc/config.gz > "$run_dir/kernel-config.gz" 2>/dev/null || true
	elif [[ -r "/boot/config-$(uname -r)" ]]; then
		cp "/boot/config-$(uname -r)" "$run_dir/kernel-config" || true
	fi
	uname -a > "$run_dir/uname.txt" 2>/dev/null || true
	grep -Ei 'deltafs|overlay|use-after-free|refcount|lockdep|BUG:|WARNING:' \
		"$run_dir/dmesg-full.log" > "$run_dir/dmesg-focus.log" || true
}

cleanup()
{
	local status=$?
	set +e
	disable_fault_injection
	unregister_fault_injection
	if [[ -n "$mounted" ]] && mountpoint -q "$mounted"; then
		umount "$mounted" || status=1
	fi
	mounted=
	if ((module_loaded)); then
		modprobe -r "$MODULE" || status=1
		module_loaded=0
	fi
	collect_diagnostics
	if ((status == 0 && fault_skipped)) && [[ "$fault_mode" != disabled ]]; then
		status=4
	fi
	log "results: $run_dir"
	if ((status == 0)); then
		if ((fault_skipped)); then
			log 'DeltaFS v2 phase1 smoke checks passed (fault injection skipped)'
		else
			log 'All DeltaFS v2 phase1 checks passed'
		fi
	elif ((status == 4)); then
		log 'DeltaFS v2 phase1 completed with skipped checks'
	else
		log "DeltaFS v2 phase1 failed (exit $status)"
	fi
	trap - EXIT
	exit "$status"
}
trap cleanup EXIT

log "source: $repo_root"
log "results: $run_dir"

make -C "$script_dir" v2-phase1-tools
"$layout_test"
[[ -r "$module_path" ]] || make -C "$repo_root" M=fs/overlayfs modules
[[ -r "$module_path" ]] || die "overlay module was not built: $module_path"

mountpoint -q "$DEBUGFS" || mount -t debugfs debugfs "$DEBUGFS"
if lsmod 2>/dev/null | awk '{print $1}' | grep -qx "$MODULE"; then
	modprobe -r "$MODULE" || die "could not unload the existing overlay module"
fi
install -D -m 0644 "$module_path" \
	"/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
depmod -a
modprobe "$MODULE"
module_loaded=1

mount_test()
{
	local lower=$2
	local upper=$3
	local work=$4
	local merged=$5
	local options

	mkdir -p "$lower" "$upper" "$work" "$merged"
	options="lowerdir=$lower,upperdir=$upper,workdir=$work"
	options+=",index=off,nfs_export=off,metacopy=off"
	options+=",xino=off,uuid=off,redirect_dir=nofollow"
	mount -t overlay overlay -o "$options" "$merged"
	mounted=$merged
}

unmount_test()
{
	if [[ -n "$mounted" ]] && mountpoint -q "$mounted"; then
		umount "$mounted"
	fi
	mounted=
}

run_smoke()
{
	local root="$run_dir/smoke"

	mkdir -p "$root"/{base,layers,merged}
	mkdir -p "$root"/branches/{g1,g2,g3}/{upper,work}
	printf 'base\n' > "$root/base/base-only"
	mount_test "$root" "$root/base" "$root/branches/g1/upper" \
		"$root/branches/g1/work" "$root/merged"

	log 'running native v2 negative matrix'
	"$helper" negative "$root/merged" 1 \
		"$root/branches/g2/upper" "$root/branches/g2/work"
	printf 'checkpoint-one\n' > "$root/merged/checkpoint-one"
	sync
	mv "$root/branches/g1/upper" "$root/layers/c1"
	log 'running checkpoint with renamed active upper'
	"$helper" checkpoint "$root/merged" 1 \
		"$root/branches/g2/upper" "$root/branches/g2/work"
	test "$(cat "$root/merged/checkpoint-one")" = checkpoint-one
	printf 'generation-two\n' > "$root/merged/generation-two"
	log 'running restore with keep_bottom=1'
	"$helper" restore "$root/merged" 2 1 \
		"$root/branches/g3/upper" "$root/branches/g3/work"
	test "$(cat "$root/merged/base-only")" = base
	test ! -e "$root/merged/checkpoint-one"
	test ! -e "$root/merged/generation-two"
	"$helper" probe-generation "$root/merged" 3
	unmount_test
}

fault_capable()
{
	[[ -d "$FAIL_FUNCTION" && -w "$FAIL_FUNCTION/inject" ]]
}

configure_fault()
{
	local nth=$1

	printf '0\n' > "$FAIL_FUNCTION/times"
	printf '%s\n' "$nth" > "$FAIL_FUNCTION/space"
	printf '1\n' > "$FAIL_FUNCTION/times"
}

run_fault_operation()
{
	local operation=$1
	local generation=$2
	local keep_bottom=$3
	local root=$4
	local success_name=$5
	local success_n=
	local n

	for n in $(seq 1 "$max_fault_nth"); do
		local branch="$root/trials/$operation-$n"
		local output_file="$root/trials/$operation-$n.log"
		local status

		rm -rf "$branch"
		mkdir -p "$branch"/{upper,work}
		configure_fault "$n"
		set +e
		if [[ "$operation" == checkpoint ]]; then
			"$helper" checkpoint "$root/merged" "$generation" \
				"$branch/upper" "$branch/work" > "$output_file" 2>&1
		else
			"$helper" restore "$root/merged" "$generation" "$keep_bottom" \
				"$branch/upper" "$branch/work" > "$output_file" 2>&1
		fi
		status=$?
		set -e
		disable_fault_injection
		if ((status == 0)); then
			success_n=$n
			break
		fi
		grep -Fq 'Cannot allocate memory' "$output_file" || {
			sed -n '1,160p' "$output_file"
			die "$operation fault point $n returned an unexpected error"
		}
		"$helper" probe-generation "$root/merged" "$generation"
		test "$(cat "$root/merged/frozen")" = frozen
	done
	[[ -n "$success_n" ]] ||
		die "$operation fault injection did not reach a successful request"
	((success_n > 1)) || die "$operation fault injection did not inject a failure"
	printf '%s=%s\n' "$success_name" "$success_n"
}

run_fault_injection()
{
	local root="$run_dir/fault"

	if [[ "$fault_mode" == disabled ]]; then
		fault_skipped=1
		log 'SKIP: fault injection disabled by request'
		return
	fi
	if ! fault_capable; then
		if [[ "$fault_mode" == required ]]; then
			die 'fail_function is unavailable'
		fi
		fault_skipped=1
		log 'SKIP: CONFIG_FUNCTION_ERROR_INJECTION/fail_function unavailable'
		return
	fi

	mkdir -p "$root"/{base,layers,merged,initial/upper,initial/work,trials}
	printf 'base\n' > "$root/base/base-only"
	mount_test "$root" "$root/base" "$root/initial/upper" \
		"$root/initial/work" "$root/merged"
	printf 'frozen\n' > "$root/merged/frozen"
	sync
	mv "$root/initial/upper" "$root/layers/c1"

	if [[ ! -d "$FAIL_FUNCTION/$CHECKPOINT_SYMBOL" ]]; then
		printf '%s\n' "$CHECKPOINT_SYMBOL" > "$FAIL_FUNCTION/inject"
		fault_added=1
	fi
	printf '0xFFFFFFFFFFFFFFF4\n' > \
		"$FAIL_FUNCTION/$CHECKPOINT_SYMBOL/retval"
	printf '100\n' > "$FAIL_FUNCTION/probability"
	printf '1\n' > "$FAIL_FUNCTION/interval"
	printf '0\n' > "$FAIL_FUNCTION/verbose"

	log 'exhausting checkpoint ownership failure points'
	run_fault_operation checkpoint 1 0 "$root" checkpoint_success
	"$helper" probe-generation "$root/merged" 2
	log 'exhausting restore ownership failure points'
	run_fault_operation restore 2 1 "$root" restore_success
	disable_fault_injection
	unregister_fault_injection
	"$helper" probe-generation "$root/merged" 3
	test "$(cat "$root/merged/base-only")" = base
	test ! -e "$root/merged/frozen"
	unmount_test
}

run_smoke
run_fault_injection

log 'phase-one checks completed'
