#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# DeltaFS v2 QEMU/KVM acceptance harness.

set -Eeuo pipefail
export LC_ALL=C

readonly PROGRAM=${0##*/}
readonly MODULE=overlay
readonly DEBUGFS=/sys/kernel/debug
readonly FAIL_FUNCTION="$DEBUGFS/fail_function"
readonly CHECKPOINT_SYMBOL=ovl_deltafs_build_checkpoint

backing_root=
extra_backing_root=
fault_mode=auto
max_fault_nth=512
run_dir=
mounted=
module_loaded=0
fault_added=0
skipped=0

usage()
{
	cat >&2 <<EOF
Usage: $PROGRAM --backing-root PATH --extra-backing-root PATH
       $PROGRAM --backing-root PATH --extra-backing-root PATH \
               [--no-fault-injection] [--require-fault-injection]
EOF
}

log() { printf '[deltafs-e0] %s\n' "$*"; }
pass() { printf 'PASS: %s\n' "$*"; }
skip() { printf 'SKIP: %s\n' "$*"; skipped=1; }
die() { log "FAIL: $*"; exit 1; }

while (($#)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || { usage; exit 2; }
		backing_root=$2; shift 2 ;;
	--extra-backing-root)
		(($# >= 2)) || { usage; exit 2; }
		extra_backing_root=$2; shift 2 ;;
	--no-fault-injection) fault_mode=disabled; shift ;;
	--require-fault-injection) fault_mode=required; shift ;;
	--max-fault-nth)
		(($# >= 2)) || { usage; exit 2; }
		max_fault_nth=$2; shift 2 ;;
	-h|--help) usage; exit 0 ;;
	*) usage; exit 2 ;;
	esac
done

[[ -n "$backing_root" && -n "$extra_backing_root" ]] || { usage; exit 2; }
[[ "$max_fault_nth" =~ ^[1-9][0-9]*$ ]] || die 'invalid --max-fault-nth'
((EUID == 0)) || die 'run as root (or through sudo)'
[[ -d "$backing_root" && -w "$backing_root" ]] || die "backing root is not writable: $backing_root"
[[ -d "$extra_backing_root" && -w "$extra_backing_root" ]] || die "extra backing root is not writable: $extra_backing_root"
[[ "$(stat -c '%d' "$backing_root")" != "$(stat -c '%d' "$extra_backing_root")" ]] ||
	die 'backing roots must be on different filesystems'

script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
repo_root=$(cd -- "$script_dir/../.." && pwd -P)
helper="$script_dir/deltafs_e0_ioctl_test"
controller="$script_dir/deltafsctl"
module_path="$repo_root/fs/overlayfs/overlay.ko"

for command in awk cat cp date dmesg findmnt grep install mkdir mount mountpoint \
	modprobe rm sed sleep stat sync tee umount uname; do
	command -v "$command" >/dev/null 2>&1 || die "missing command: $command"
done
if findmnt -rn -t overlay | grep -q .; then
	die 'an overlay mount already exists'
fi

run_dir="$backing_root/e0-acceptance-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p -- "$run_dir"
exec > >(tee -a "$run_dir/runner.log") 2>&1

disable_fault_injection()
{
	if [[ -d "$FAIL_FUNCTION" ]]; then
		printf '0\n' > "$FAIL_FUNCTION/times" 2>/dev/null || true
	fi
}

unregister_fault_injection()
{
	if ((fault_added)); then
		printf '!%s\n' "$CHECKPOINT_SYMBOL" > "$FAIL_FUNCTION/inject" 2>/dev/null || true
		fault_added=0
	fi
}

collect_diagnostics()
{
	dmesg -T > "$run_dir/dmesg-full.log" 2>/dev/null || true
	findmnt -J > "$run_dir/findmnt.json" 2>/dev/null || true
	cat /proc/mounts > "$run_dir/proc-mounts.txt" 2>/dev/null || true
	cat "$DEBUGFS/kmemleak" > "$run_dir/kmemleak.log" 2>/dev/null || true
	uname -a > "$run_dir/uname.txt" 2>/dev/null || true
	grep -Ei 'deltafs|overlay|use-after-free|refcount|lockdep|BUG:|WARNING:|KASAN|KFENCE|UBSAN' \
		"$run_dir/dmesg-full.log" > "$run_dir/dmesg-focus.log" || true
}

cleanup()
{
	local status=$?
	set +e
	disable_fault_injection
	unregister_fault_injection
	if [[ -n "$mounted" ]] && mountpoint -q -- "$mounted"; then umount -- "$mounted" || status=1; fi
	mounted=
	if ((module_loaded)); then modprobe -r "$MODULE" || status=1; module_loaded=0; fi
	collect_diagnostics
	log "results: $run_dir"
	if ((status == 0 && skipped)); then
		log 'DeltaFS E0 acceptance completed with skipped capability-dependent checks'; status=4
	elif ((status == 0)); then
		log 'All DeltaFS E0 acceptance checks passed'
	else
		log "DeltaFS E0 acceptance failed (exit $status)"
	fi
	trap - EXIT
	exit "$status"
}
trap cleanup EXIT

mount_overlay()
{
	local lower=$1 upper=$2 work=$3 target=$4 options
	mkdir -p -- "$lower" "$upper" "$work" "$target"
	options="lowerdir=$lower,upperdir=$upper,workdir=$work"
	options+=',index=off,nfs_export=off,metacopy=off,xino=off,uuid=off,redirect_dir=nofollow'
	mount -t overlay overlay -o "$options" "$target"
	mounted=$target
}

unmount_overlay()
{
	if [[ -n "$mounted" ]] && mountpoint -q -- "$mounted"; then umount -- "$mounted"; fi
	mounted=
}

init_controller_sandbox()
{
	local root=$1
	mkdir -p -- "$root"/{base,layers,meta,merged} "$root/branches/g1"/{upper,work}
	cat > "$root/meta/state.json" <<'EOF'
{
  "format": 2,
  "kernel_generation": 1,
  "active_branch": "g1",
  "active_lowers": ["base"],
  "retired_branches": [],
  "snapshots": {}
}
EOF
	printf 'base\n' > "$root/base/base-only"
}

run_controller_scenario()
{
	local root="$run_dir/controller"
	init_controller_sandbox "$root"
	mount_overlay "$root/base" "$root/branches/g1/upper" "$root/branches/g1/work" "$root/merged"
	mkdir -p -- "$root/branches/g9"/{upper,work}
	"$helper" negative "$root/merged" 1 "$root/branches/g9/upper" "$root/branches/g9/work"
	printf 'A\n' > "$root/merged/checkpoint-value"; sync
	"$controller" --assume-quiesced checkpoint "$root" A
	grep -Fq '"format": 2' "$root/meta/state.json" || die 'state format is not 2'
	[[ -f "$root/layers/A/checkpoint-value" ]] || die 'checkpoint upper was not frozen'
	printf 'B\n' > "$root/merged/branch-value"; sync
	cat "$root/merged/checkpoint-value" >/dev/null
	[[ ! -e "$root/merged/negative-before-switch" ]]
	"$controller" --assume-quiesced checkpoint "$root" B
	printf 'C\n' > "$root/merged/third-value"; sync
	"$controller" --assume-quiesced checkpoint "$root" C
	"$controller" --assume-quiesced restore "$root" A
	[[ "$(cat "$root/merged/checkpoint-value")" == A ]] || die 'restore A content mismatch'
	[[ ! -e "$root/merged/branch-value" && ! -e "$root/merged/third-value" ]] || die 'restore exposed newer branch'
	"$helper" probe-generation "$root/merged" 5
	pass 'checkpoint_derived_chain=PASS'
	pass 'restore_keep_bottom_all=PASS'
	unmount_overlay
}

run_depth_boundary()
{
	local root="$run_dir/depth" lower_list='' options i status
	mkdir -p -- "$root"/{base,upper,work,merged,prefix}
	for ((i = 0; i < 126; i++)); do
		mkdir -p -- "$root/lower-$i"
		[[ -z "$lower_list" ]] || lower_list+=:
		lower_list+="lower-$i"
	done
	lower_list+=':base'
	[[ "$lower_list" != *,* && "$lower_list" == lower-0:lower-1:*:base ]] ||
		die 'invalid colon-separated lowerdir list'
	options="lowerdir=$lower_list,upperdir=upper,workdir=work"
	options+=',index=off,nfs_export=off,metacopy=off,xino=off,uuid=off,redirect_dir=nofollow'
	(cd "$root" && mount -t overlay overlay -o "$options" merged)
	mounted="$root/merged"
	mkdir -p -- "$root/prefix"/{p0,p1} "$root/fresh128"/{upper,work} \
		"$root/fresh129"/{upper,work} "$root/fresh1"/{upper,work} \
		"$root/fresh0"/{upper,work}
	set +e
	"$helper" restore "$root/merged" 1 127 "$root/fresh129/upper" "$root/fresh129/work" \
		"$root/prefix/p0" "$root/prefix/p1" > "$run_dir/depth-129.log" 2>&1
	status=$?
	set -e
	((status != 0)) || die '129-lower target unexpectedly succeeded'
	grep -Fq 'Argument list too long' "$run_dir/depth-129.log" || die '129-lower target did not return E2BIG'
	"$helper" restore "$root/merged" 1 127 "$root/fresh128/upper" "$root/fresh128/work" "$root/prefix/p0"
	pass 'target_lower_128=PASS'
	pass 'target_lower_129_e2big=PASS'
	"$helper" restore "$root/merged" 2 1 "$root/fresh1/upper" "$root/fresh1/work" "$root/prefix/p1"
	pass 'restore_keep_bottom_1=PASS'
	"$helper" restore "$root/merged" 3 0 "$root/fresh0/upper" "$root/fresh0/work" "$root/prefix/p0"
	pass 'restore_keep_bottom_0=PASS'
	unmount_overlay
}

fault_capable() { [[ -d "$FAIL_FUNCTION" && -w "$FAIL_FUNCTION/inject" ]]; }

run_fault_injection()
{
	local root="$run_dir/fault" n status
	if [[ "$fault_mode" == disabled ]]; then skip 'fault_injection_exhausted (disabled)'; return; fi
	if ! fault_capable; then
		[[ "$fault_mode" == required ]] && die 'fail_function is unavailable'
		skip 'fault_injection_exhausted (fail_function unavailable)'; return
	fi
	mkdir -p -- "$root"/{base,upper,work,merged}
	printf 'base\n' > "$root/base/base-only"
	mount_overlay "$root/base" "$root/upper" "$root/work" "$root/merged"
	printf 'frozen\n' > "$root/merged/frozen"; sync
	printf '%s\n' "$CHECKPOINT_SYMBOL" > "$FAIL_FUNCTION/inject"; fault_added=1
	printf '0xFFFFFFFFFFFFFFF4\n' > "$FAIL_FUNCTION/$CHECKPOINT_SYMBOL/retval"
	printf '100\n' > "$FAIL_FUNCTION/probability"; printf '1\n' > "$FAIL_FUNCTION/interval"
	for ((n = 1; n <= max_fault_nth; n++)); do
		local branch="$root/trial-$n"
		mkdir -p -- "$branch"/{upper,work}
		printf '0\n' > "$FAIL_FUNCTION/times"; printf '%s\n' "$n" > "$FAIL_FUNCTION/space"; printf '1\n' > "$FAIL_FUNCTION/times"
		set +e
		"$helper" checkpoint "$root/merged" 1 "$branch/upper" "$branch/work" > "$branch.log" 2>&1
		status=$?; set -e; disable_fault_injection
		if ((status == 0)); then
			((n > 1)) || die 'fault injection did not hit a failure'
			pass "fault_injection_exhausted=$n"; unmount_overlay; return
		fi
		grep -Eiq 'Cannot allocate memory|No memory' "$branch.log" || die "fault point $n returned an unexpected error"
		"$helper" probe-generation "$root/merged" 1
		[[ "$(cat "$root/merged/frozen")" == frozen ]] || die "fault point $n changed active data"
	done
	die "fault injection did not reach success by $max_fault_nth"
}

make -C "$script_dir" e0-tools
make -C "$script_dir" test-e0-controller
"$script_dir/deltafs_e0_layout_test"
[[ -r "$module_path" ]] || make -C "$repo_root" M=fs/overlayfs modules
[[ -r "$module_path" ]] || die "overlay module was not built: $module_path"
mountpoint -q "$DEBUGFS" || mount -t debugfs debugfs "$DEBUGFS"
if lsmod 2>/dev/null | awk '{print $1}' | grep -qx "$MODULE"; then modprobe -r "$MODULE"; fi
install -D -m 0644 "$module_path" "/lib/modules/$(uname -r)/kernel/fs/overlayfs/overlay.ko"
depmod -a; modprobe "$MODULE"; module_loaded=1

run_controller_scenario
run_depth_boundary
run_fault_injection
for i in 1 2; do modprobe -r "$MODULE"; modprobe "$MODULE"; done
pass 'module_unload_cycles=PASS'

if [[ -r "$DEBUGFS/kmemleak" ]]; then
	printf 'scan\n' > "$DEBUGFS/kmemleak"; sleep 2
	[[ ! -s "$DEBUGFS/kmemleak" ]] || die 'kmemleak reported live objects after teardown'
	pass 'kmemleak=PASS'
else
	skip 'kmemleak (debugfs interface unavailable)'
fi
if dmesg | grep -Eiq 'use-after-free|refcount_t:|BUG: KASAN|KFENCE:|UBSAN:|WARNING:.*lockdep'; then
	die 'sanitizer or lockdep diagnostic found'
fi
pass 'sanitizer_dmesg=PASS'
pass 'retired_state_teardown=PASS'
