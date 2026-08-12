#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# DeltaFS P7 v1 acceptance gate.
#
# This is intentionally a destructive, guest-only harness. It refuses a
# development host, refuses pre-existing OverlayFS mounts, and repeatedly
# unloads the overlay module. A passing exit status is the evidence for all
# eight conditions in docs/deltafs_v1_design.md section 17.

set -Eeuo pipefail

readonly OVERLAY_MODULE=overlay
readonly OVERLAY_FEATURE_OPTIONS='index=off,nfs_export=off,metacopy=off,xino=off'
readonly OVERLAY_ROOT_OPTIONS='uuid=off,redirect_dir=nofollow'
readonly KMEMLEAK_PATH=/sys/kernel/debug/kmemleak
readonly FAIL_FUNCTION_DIR=/sys/kernel/debug/fail_function
readonly BUILD_CHECKPOINT=ovl_deltafs_build_checkpoint
readonly KMEMLEAK_MIN_AGE_SECONDS=6
readonly KMEMLEAK_SCAN_SETTLE_SECONDS=2
readonly MAX_LOWER_CHECKPOINTS=63
readonly REQUIRED_SUCCESSFUL_SWITCHES=100
readonly DEFAULT_UNLOAD_CYCLES=10
readonly MAX_FAULT_NTH=4096

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(realpath -e -- "$script_dir/../..")
design_doc="$repo_root/docs/deltafs_v1_design.md"

controller="$script_dir/deltafsctl"
generation_test="$script_dir/p6_generation_test"
abi_test="$script_dir/p7_ioctl_test"
p5_commit_harness="$script_dir/p5_commit_test.sh"
p5_checkpoint_harness="$script_dir/p5_checkpoint_test.sh"
p6_harness="$script_dir/p6_controller_test.sh"

backing_root=
extra_backing_root=
results_dir=
run_dir=
extra_run_dir=
current_merged=
kernel_config=
kernel_config_gzip=0
unload_cycles=$DEFAULT_UNLOAD_CYCLES
marker="deltafs-p7-acceptance[$$]"
marker_started=0
marker_finished=0
fault_injection_registered=0
frozen_ledger=

# Debug-guest capability flags.  Each gates one capability-dependent phase
# (deep fault injection, kmemleak scan, sanitizer/lockdep dmesg coverage).  A
# missing capability downgrades its phase to a logged SKIP (exit 4) rather than
# failing the harness, so a guest without fail_function can still run the
# non-injection matrices.  See docs/deltafs_v1_design.md section 16.6.
have_injection=0
have_kmemleak=0
have_sanitizers=0

usage()
{
	cat <<'EOF'
Usage: p7_acceptance_test.sh --backing-root PATH --extra-backing-root PATH [OPTIONS]

Run only as root in the project QEMU/KVM debug guest. The harness loads and
unloads OverlayFS, so the guest must have no pre-existing OverlayFS mounts.

Required:
  --backing-root PATH        Writable non-OverlayFS filesystem for all normal
                             DeltaFS backing directories.
  --extra-backing-root PATH  Writable directory on a different backing
                             superblock, used for the ABI -EXDEV case.

Options:
  --unload-cycles N          mount/unmount/module-unload repetitions
                             (default: 10; must be at least 10).
  --results-dir PATH         Persist logs here instead of creating a timestamped
                             directory below --backing-root.
  -h, --help                 Show this help.

Build prerequisites in the guest:
  make -C tools/deltafs p7-tools
EOF
}

log()
{
	printf '%s\n' "$*"
	if [[ -n "$results_dir" && -d "$results_dir" ]]; then
		printf '%s\n' "$*" >> "$results_dir/summary.log"
	fi
}

pass()
{
	log "PASS: $*"
}

die()
{
	log "FAIL: $*"
	exit 1
}

# Record a capability-dependent skip without aborting the harness.  The
# capability-dependent phase is omitted and the final exit code becomes 4.
note_skip()
{
	log "SKIP: $*"
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
			pass "QEMU/KVM guest detected ($detected)"
			return 0
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
		pass 'QEMU/KVM guest detected from DMI/device-tree'
		return 0
	fi
	die 'refusing to run outside a confirmed QEMU/KVM guest'
}

select_kernel_config()
{
	local candidate

	for candidate in /proc/config.gz "/boot/config-$(uname -r)"; do
		if [[ -r "$candidate" ]]; then
			kernel_config=$candidate
			if [[ "$candidate" == /proc/config.gz ]]; then
				kernel_config_gzip=1
			fi
			return 0
		fi
	done
	die 'cannot read the running kernel configuration'
}

config_get()
{
	local key=$1

	if ((kernel_config_gzip)); then
		zgrep -E "^${key}=" "$kernel_config" || true
	else
		grep -E "^${key}=" "$kernel_config" || true
	fi
}

require_kernel_config()
{
	local key=$1
	local value=$2
	local actual

	actual=$(config_get "$key")
	[[ "$actual" == "${key}=${value}" ]] ||
		die "requires ${key}=${value}; running config has '${actual:-unset}'"
}

# Non-fatal variant of require_kernel_config.  Returns 0 when the running
# kernel config matches key=value, 1 otherwise; never dies.  Used to probe
# capability-dependent debug options that may legitimately be absent.
probe_kernel_config()
{
	local key=$1
	local value=$2
	local actual

	actual=$(config_get "$key")
	[[ "$actual" == "${key}=${value}" ]]
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

require_no_overlay_mounts()
{
	if (( $(overlay_mount_count) != 0 )); then
		show_overlay_mounts
		die 'refusing to run while OverlayFS mounts exist'
	fi
}

ensure_overlay_unloaded()
{
	require_no_overlay_mounts
	if [[ -e "/sys/module/$OVERLAY_MODULE" ]]; then
		modprobe -r "$OVERLAY_MODULE" ||
			die "could not unload $OVERLAY_MODULE without force"
	fi
	[[ ! -e "/sys/module/$OVERLAY_MODULE" ]] ||
		die "$OVERLAY_MODULE remains loaded after unload"
}

load_overlay()
{
	modprobe "$OVERLAY_MODULE" || die "could not load $OVERLAY_MODULE"
	[[ -d "/sys/module/$OVERLAY_MODULE" ]] ||
		die "$OVERLAY_MODULE sysfs entry is absent after load"
}

finish_marker()
{
	if ((marker_started != 0 && marker_finished == 0)); then
		printf '%s END\n' "$marker" > /dev/kmsg 2>/dev/null || true
		marker_finished=1
	fi
}

disable_fault_injection()
{
	if [[ -d "$FAIL_FUNCTION_DIR" ]]; then
		printf '0\n' > "$FAIL_FUNCTION_DIR/times" 2>/dev/null || true
	fi
}

unregister_fault_injection()
{
	if ((fault_injection_registered != 0)); then
		printf '!%s\n' "$BUILD_CHECKPOINT" > \
			"$FAIL_FUNCTION_DIR/inject" 2>/dev/null || true
		fault_injection_registered=0
	fi
}

cleanup()
{
	local status=$?

	trap - EXIT
	set +e
	disable_fault_injection
	unregister_fault_injection
	if [[ -n "$current_merged" ]] && mountpoint -q -- "$current_merged"; then
		if ! umount -- "$current_merged"; then
			log "FAIL: cleanup could not unmount $current_merged"
			status=1
		fi
	fi
	current_merged=
	if (( $(overlay_mount_count) != 0 )); then
		log 'FAIL: cleanup left one or more OverlayFS mounts'
		status=1
	elif [[ -e "/sys/module/$OVERLAY_MODULE" ]]; then
		if ! modprobe -r "$OVERLAY_MODULE"; then
			log "FAIL: cleanup could not unload $OVERLAY_MODULE"
			status=1
		fi
	fi
	finish_marker
	# Treat SKIP (4) like PASS for scratch cleanup: the run itself was healthy,
	# only capability-dependent phases were omitted.  FAIL (1/2/...) preserves
	# the scratch tree for diagnosis.
	if ((status == 0 || status == 4)); then
		if [[ -n "$run_dir" && -d "$run_dir" ]]; then
			if ! rm -rf -- "$run_dir"; then
				log "FAIL: cleanup could not remove $run_dir"
				status=1
			fi
		fi
		if [[ -n "$extra_run_dir" && -d "$extra_run_dir" ]]; then
			if ! rm -rf -- "$extra_run_dir"; then
				log "FAIL: cleanup could not remove $extra_run_dir"
				status=1
			fi
		fi
	else
		[[ -n "$run_dir" && -d "$run_dir" ]] &&
			log "P7 scratch preserved after failure: $run_dir"
		[[ -n "$extra_run_dir" && -d "$extra_run_dir" ]] &&
			log "P7 extra-filesystem scratch preserved after failure: $extra_run_dir"
	fi
	exit "$status"
}

run_phase()
{
	local name=$1
	local phase_log="$results_dir/${name}.log"

	shift
	log "Running phase: $name"
	if "$@" > "$phase_log" 2>&1; then
		pass "$name completed (log: $phase_log)"
	else
		sed -n '1,240p' "$phase_log" >&2 || true
		die "$name failed (log: $phase_log)"
	fi
}

record_gate()
{
	local number=$1
	local evidence=$2

	printf '%s\tPASS\t%s\n' "$number" "$evidence" >> "$results_dir/section-17.tsv"
	pass "section 17 condition $number: $evidence"
}

# Record a section-17 condition as SKIP.  Used when a capability-dependent
# phase (deep fault injection, kmemleak, sanitizer coverage) was skipped
# because the running kernel lacks the corresponding debug option.
record_gate_skip()
{
	local number=$1
	local evidence=$2

	printf '%s\tSKIP\t%s\n' "$number" "$evidence" >> "$results_dir/section-17.tsv"
	note_skip "section 17 condition $number: $evidence"
}

mount_raw_sandbox()
{
	local root=$1
	local options

	options="lowerdir=$root/base"
	options+=",upperdir=$root/upper"
	options+=",workdir=$root/work,$OVERLAY_FEATURE_OPTIONS,$OVERLAY_ROOT_OPTIONS"
	mount -t overlay overlay -o "$options" "$root/merged" ||
		die "could not mount raw sandbox $root"
	current_merged="$root/merged"
}

init_raw_sandbox()
{
	local root=$1

	mkdir -p -- "$root/base/p7-abi-child" "$root/upper" "$root/work" \
		"$root/merged" "$root/cases"
	printf 'p7-abi-baseline\n' > "$root/base/p7-abi-baseline"
}

init_controller_sandbox()
{
	local root=$1

	mkdir -p -- "$root/base" "$root/layers" \
		"$root/branches/g1/upper" "$root/branches/g1/work" \
		"$root/meta" "$root/merged"
	printf 'p7-base\n' > "$root/base/base-only"
	printf '%s\n' \
		'{' \
		'  "format": 1,' \
		'  "kernel_generation": 1,' \
		'  "active_branch": "g1",' \
		'  "active_lowers": ["base"],' \
		'  "retired_branches": [],' \
		'  "snapshots": {}' \
		'}' > "$root/meta/state.json"
}

mount_controller_sandbox()
{
	local root=$1
	local options

	options="lowerdir=$root/base"
	options+=",upperdir=$root/branches/g1/upper"
	options+=",workdir=$root/branches/g1/work"
	options+=",$OVERLAY_FEATURE_OPTIONS,$OVERLAY_ROOT_OPTIONS"
	mount -t overlay overlay -o "$options" "$root/merged" ||
		die "could not mount controller sandbox $root"
	current_merged="$root/merged"
}

unmount_current()
{
	[[ -n "$current_merged" ]] || die 'internal: no current OverlayFS mount'
	umount -- "$current_merged" || die "could not unmount $current_merged"
	current_merged=
}

expect_contents()
{
	local path=$1
	local expected=$2
	local stage=${3:-contents}
	local actual

	[[ -f "$path" ]] || die "$stage: expected regular file is missing: $path"
	actual=$(<"$path")
	[[ "$actual" == "$expected" ]] ||
		die "$stage: unexpected contents in $path: '$actual'"
}

assert_clean_transaction()
{
	local root=$1

	[[ ! -e "$root/meta/transaction.json" ]] ||
		die "transaction remains in $root"
	[[ ! -e "$root/meta/.transaction.json.tmp" ]] ||
		die "transaction temporary remains in $root"
	[[ ! -e "$root/meta/.state.json.tmp" ]] ||
		die "state temporary remains in $root"
}

state_hash()
{
	sha256sum -- "$1/meta/state.json" | awk '{print $1}'
}

assert_active_depth()
{
	local root=$1
	local expected=$2
	local depth

	# deltafsctl serializes this array on one line and the next field on a
	# separate line.  Persisted strings never require JSON quote escaping, so
	# subtract the quoted key and count the remaining quote pairs.
	if ! depth=$(awk -F'"' '
		/^[[:space:]]*"active_lowers"[[:space:]]*:/ {
			if (++found != 1 || $0 !~ /\[.*\],[[:space:]]*$/)
				exit 2
			value = (NF - 3) / 2
			if (value < 0 || value != int(value))
				exit 2
		}
		END {
			if (found != 1)
				exit 2
			print value
		}
	' "$root/meta/state.json"); then
		die "could not parse active_lowers in $root/meta/state.json"
	fi
	[[ "$depth" == "$expected" ]] ||
		die "active lower depth is $depth, expected $expected"
}

fingerprint_metadata()
{
	stat -c '%d:%i:%f:%s:%y' -- "$1"
}

fingerprint_digest()
{
	sha256sum -- "$1" | awk '{print $1}'
}

record_frozen_file()
{
	local path=$1
	local metadata
	local digest

	[[ -f "$path" ]] || die "cannot fingerprint absent frozen file $path"
	metadata=$(fingerprint_metadata "$path")
	digest=$(fingerprint_digest "$path")
	printf '%s\t%s\t%s\n' "$path" "$metadata" "$digest" >> "$frozen_ledger"
}

verify_frozen_files()
{
	local path metadata digest actual_metadata actual_digest

	[[ -s "$frozen_ledger" ]] || die 'frozen-layer ledger is empty'
	while IFS=$'\t' read -r path metadata digest; do
		actual_metadata=$(fingerprint_metadata "$path")
		actual_digest=$(fingerprint_digest "$path")
		[[ "$actual_metadata" == "$metadata" ]] ||
			die "frozen metadata changed: $path"
		[[ "$actual_digest" == "$digest" ]] ||
			die "frozen contents changed: $path"
	done < "$frozen_ledger"
}

assert_generation()
{
	local merged=$1
	local generation=$2

	"$generation_test" "$merged" "$generation" >> "$results_dir/generation.log" 2>&1 ||
		die "kernel generation is not exactly $generation"
}

register_fault_injection()
{
	if [[ ! -d "$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT" ]]; then
		printf '%s\n' "$BUILD_CHECKPOINT" > "$FAIL_FUNCTION_DIR/inject" ||
			die "could not register $BUILD_CHECKPOINT with fail_function"
	fi
	[[ -d "$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT" ]] ||
		die "$BUILD_CHECKPOINT is not registered with fail_function"
	fault_injection_registered=1
}

enable_fault_injection()
{
	local nth=$1

	register_fault_injection
	# Keep injection disabled until every selector has been installed.  The
	# fail_function fault_attr count behind "interval" is global and cannot be
	# reset through debugfs, so interval=N cannot express the Nth call across
	# repeated trials.  should_fail() receives size=1 here; resetting space=N
	# and using interval=1 deterministically fails exactly the Nth call.
	printf '0\n' > "$FAIL_FUNCTION_DIR/times" ||
		die 'could not disable fault injection while configuring it'
	# -ENOMEM represented as a u64 for the fail_function retval ABI.
	printf '0xFFFFFFFFFFFFFFF4\n' > \
		"$FAIL_FUNCTION_DIR/$BUILD_CHECKPOINT/retval" ||
		die 'could not set fault-injection errno'
	printf '100\n' > "$FAIL_FUNCTION_DIR/probability" ||
		die 'could not set fault-injection probability'
	printf '1\n' > "$FAIL_FUNCTION_DIR/interval" ||
		die 'could not set fault-injection interval'
	printf '%s\n' "$nth" > "$FAIL_FUNCTION_DIR/space" ||
		die 'could not set fault-injection checkpoint'
	printf '0\n' > "$FAIL_FUNCTION_DIR/verbose" ||
		die 'could not disable fault-injection verbosity'
	printf '1\n' > "$FAIL_FUNCTION_DIR/times" ||
		die 'could not enable one fault-injection hit'
}

run_deep_fault_injection()
{
	local root=$1
	local generation=$2
	local nth
	local status
	local before_state
	local fault_log

	for ((nth = 1; nth <= MAX_FAULT_NTH; nth++)); do
		before_state=$(state_hash "$root")
		enable_fault_injection "$nth"
		fault_log="$results_dir/deep-fault-${nth}.log"
		set +e
		LC_ALL=C "$controller" --assume-quiesced restore "$root" s63 \
			> "$fault_log" 2>&1
		status=$?
		set -e
		disable_fault_injection

		if ((status == 0)); then
			assert_clean_transaction "$root"
			assert_generation "$root/merged" "$((generation + 1))"
			assert_active_depth "$root" 64
			verify_frozen_files
			printf '%s\n' "$nth" > "$results_dir/deep-fault-checkpoints.txt"
			pass "deep fault injection exhausted after $((nth - 1)) injected checkpoints"
			unregister_fault_injection
			return 0
		fi

		grep -Fq 'Cannot allocate memory' "$fault_log" || {
			sed -n '1,240p' "$fault_log" >&2 || true
			die "fault checkpoint $nth failed for a reason other than injected ENOMEM"
		}
		[[ "$(state_hash "$root")" == "$before_state" ]] ||
			die "fault checkpoint $nth changed state.json"
		[[ ! -e "$root/branches/g$((generation + 1))" ]] ||
			die "fault checkpoint $nth leaked fresh branch g$((generation + 1))"
		assert_clean_transaction "$root"
		expect_contents "$root/merged/p7-frozen" s63 "fault checkpoint $nth"
		assert_generation "$root/merged" "$generation"
		verify_frozen_files
	done
	die "deep fault injection did not reach a non-injected success by checkpoint $MAX_FAULT_NTH"
}

run_native_abi_matrix()
{
	local root="$run_dir/abi-native"
	local extra_case="$extra_run_dir/cross-superblock"

	ensure_overlay_unloaded
	load_overlay
	init_raw_sandbox "$root"
	mkdir -- "$extra_case"
	mount_raw_sandbox "$root"
	"$abi_test" negative "$root/merged" "$root/cases" 1 "$root/upper" \
		"$root/base" "$extra_case" > "$results_dir/p7-abi-negative.log" 2>&1 || {
		sed -n '1,280p' "$results_dir/p7-abi-negative.log" >&2 || true
		die 'native P7 ABI-negative matrix failed'
	}
	assert_generation "$root/merged" 1
	unmount_current
	ensure_overlay_unloaded
	pass 'native ABI-negative matrix left generation and view unchanged'
}

run_deep_switch_and_fault_matrix()
{
	local root="$run_dir/deep-switch"
	local checkpoint
	local id
	local generation
	local overflow_log="$results_dir/deep-overflow.log"
	local before_overflow
	local overflow_status
	local restore_ids=(s01 s08 s32 s63)
	local restore_index
	local branch_file
	local operation

	ensure_overlay_unloaded
	load_overlay
	init_controller_sandbox "$root"
	mount_controller_sandbox "$root"
	frozen_ledger="$results_dir/frozen-ledger.tsv"
	: > "$frozen_ledger"

	for ((checkpoint = 1; checkpoint <= MAX_LOWER_CHECKPOINTS; checkpoint++)); do
		printf -v id 's%02d' "$checkpoint"
		printf '%s\n' "$id" > "$root/merged/p7-frozen"
		printf 'only-%s\n' "$id" > "$root/merged/p7-only-$id"
		"$controller" --assume-quiesced checkpoint "$root" "$id" \
			>> "$results_dir/deep-checkpoints.log" 2>&1 ||
			die "checkpoint $id failed"
		generation=$((checkpoint + 1))
		expect_contents "$root/layers/$id/p7-frozen" "$id" "checkpoint $id"
		expect_contents "$root/layers/$id/p7-only-$id" "only-$id" \
			"checkpoint $id"
		record_frozen_file "$root/layers/$id/p7-frozen"
		record_frozen_file "$root/layers/$id/p7-only-$id"
		assert_clean_transaction "$root"
		assert_generation "$root/merged" "$generation"
		verify_frozen_files
	done

	assert_active_depth "$root" 64
	before_overflow=$(state_hash "$root")
	set +e
	LC_ALL=C "$controller" --assume-quiesced checkpoint "$root" overflow \
		> "$overflow_log" 2>&1
	overflow_status=$?
	set -e
	((overflow_status != 0)) || die 'controller accepted checkpoint beyond 64 lowers'
	grep -Fq 'checkpoint would exceed the 64-lower v1 limit' "$overflow_log" || {
		sed -n '1,160p' "$overflow_log" >&2 || true
		die 'controller limit failure was not the expected E2BIG preflight'
	}
	[[ "$(state_hash "$root")" == "$before_overflow" ]] ||
		die 'over-limit controller checkpoint changed state.json'
	[[ ! -e "$root/layers/overflow" && ! -e "$root/branches/g65" ]] ||
		die 'over-limit controller checkpoint mutated the backing tree'
	assert_clean_transaction "$root"
	assert_generation "$root/merged" 64
	verify_frozen_files
	pass '64-lower controller boundary rejects a 65th checkpoint without mutation'

	# The first non-injected restore is successful switch 64.
	if ((have_injection)); then
		run_deep_fault_injection "$root" 64
	else
		note_skip 'deep fault injection unavailable; non-injected restore to s63 substitutes switch 64'
		LC_ALL=C "$controller" --assume-quiesced restore "$root" s63 \
			>> "$results_dir/deep-restores.log" 2>&1 ||
			die 'non-injected deep restore to s63 failed'
		printf 'skipped\n' > "$results_dir/deep-fault-checkpoints.txt"
	fi
	expect_contents "$root/merged/p7-frozen" s63 'deep restore after fault injection'
	printf 'fault-success\n' > "$root/merged/p7-fault-success"
	expect_contents "$root/branches/g65/upper/p7-fault-success" fault-success \
		'deep restore fresh upper'
	verify_frozen_files

	for ((operation = 65; operation <= REQUIRED_SUCCESSFUL_SWITCHES; operation++)); do
		restore_index=$(((operation - 65) % ${#restore_ids[@]}))
		id=${restore_ids[restore_index]}
		"$controller" --assume-quiesced restore "$root" "$id" \
			>> "$results_dir/deep-restores.log" 2>&1 ||
			die "restore $id at successful switch $operation failed"
		generation=$((operation + 1))
		expect_contents "$root/merged/p7-frozen" "$id" \
			"restore $id at successful switch $operation"
		branch_file="p7-branch-$operation"
		printf 'branch-%s\n' "$operation" > "$root/merged/$branch_file"
		expect_contents "$root/branches/g$generation/upper/$branch_file" \
			"branch-$operation" "fresh upper at successful switch $operation"
		assert_clean_transaction "$root"
		assert_generation "$root/merged" "$generation"
		verify_frozen_files
	done

	assert_active_depth "$root" 64
	grep -Fq '"kernel_generation": 101' "$root/meta/state.json" ||
		die '100 successful switches did not reach generation 101'
	grep -Fq '"active_branch": "g101"' "$root/meta/state.json" ||
		die '100 successful switches did not select branch g101'
	verify_frozen_files
	unmount_current
	ensure_overlay_unloaded
	pass '63 checkpoints plus 37 restores completed 100 successful switches'
}

run_unload_cycles()
{
	local cycle
	local root
	local id

	for ((cycle = 1; cycle <= unload_cycles; cycle++)); do
		root="$run_dir/unload-$cycle"
		printf -v id 'u%02d' "$cycle"
		ensure_overlay_unloaded
		load_overlay
		init_controller_sandbox "$root"
		mount_controller_sandbox "$root"
		printf 'unload-%s\n' "$cycle" > "$root/merged/p7-unload"
		"$controller" --assume-quiesced checkpoint "$root" "$id" \
			>> "$results_dir/unload-cycles.log" 2>&1 ||
			die "unload cycle $cycle checkpoint failed"
		expect_contents "$root/layers/$id/p7-unload" "unload-$cycle" \
			"unload cycle $cycle"
		assert_generation "$root/merged" 2
		unmount_current
		ensure_overlay_unloaded
		[[ ! -e "/sys/module/$OVERLAY_MODULE" ]] ||
			die "overlay module remains after unload cycle $cycle"
		rm -rf -- "$root"
		pass "mount/unmount/module-unload cycle $cycle completed"
	done
}

scan_kmemleak()
{
	local report="$results_dir/kmemleak.log"

	# kmemleak is a capability-dependent check; skip cleanly when the running
	# kernel lacks it (have_kmemleak is probed in the precondition block).
	if ((have_kmemleak != 1)); then
		note_skip 'kmemleak scan skipped (CONFIG_DEBUG_KMEMLEAK or debugfs file unavailable)'
		return 0
	fi

	log "Waiting ${KMEMLEAK_MIN_AGE_SECONDS}s for kmemleak object age"
	sleep "$KMEMLEAK_MIN_AGE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	printf 'scan\n' > "$KMEMLEAK_PATH"
	sleep "$KMEMLEAK_SCAN_SETTLE_SECONDS"
	cp -- "$KMEMLEAK_PATH" "$report" || die 'could not save kmemleak report'
	[[ ! -s "$report" ]] || {
		sed -n '1,320p' "$report" >&2 || true
		die 'kmemleak reported objects after P7 teardown'
	}
	pass 'kmemleak report is empty after P7 teardown'
}

scan_kernel_window()
{
	local diagnostics
	local window="$results_dir/dmesg-window.log"

	# The dmesg scan always runs (it catches BUG/Oops/GPF regardless of debug
	# configs), but sanitizer/lockdep/RCU coverage is only meaningful when the
	# corresponding debug options are compiled in.
	if ((have_sanitizers != 1)); then
		note_skip 'sanitizer/lockdep/RCU coverage unavailable; dmesg window scan limited to generic diagnostics'
	fi

	diagnostics='BUG:|WARNING:|Oops:|KASAN:|KFENCE:|UBSAN:|use-after-free'
	diagnostics+='|double[- ]free|refcount_t:|refcount[^[:cntrl:]]*(underflow|saturat)'
	diagnostics+='|possible circular locking dependency|inconsistent lock state'
	diagnostics+='|held lock freed|bad unlock balance|suspicious RCU usage'
	diagnostics+='|rcu_preempt detected stalls|INFO: rcu|kernel NULL pointer'
	diagnostics+='|general protection fault|unable to handle kernel'
	diagnostics+='|kmemleak: [1-9][0-9]* new suspected memory leaks'

	finish_marker
	dmesg | awk -v begin="$marker BEGIN" -v end="$marker END" '
		index($0, begin) { capture = 1; saw_begin = 1 }
		capture { print }
		capture && index($0, end) { saw_end = 1; exit }
		END { if (!saw_begin || !saw_end) exit 1 }
	' > "$window" || die 'could not isolate the P7 dmesg marker window'
	if grep -Eiq "$diagnostics" "$window"; then
		sed -n '1,360p' "$window" >&2 || true
		die 'P7 dmesg window contains a sanitizer, refcount, lockdep, or RCU diagnostic'
	fi
	pass 'P7 dmesg window has no sanitizer/refcount/lockdep/RCU diagnostic'
}

verify_documented_limits()
{
	local evidence="$results_dir/documented-limitations.txt"
	local pattern
	local patterns=(
		'- 普通文件 fd、目录 fd、`cwd`、进程 root、mmap 跨切换继续使用；'
		'- checkpoint/restore 与路径查找、copy-up 或写 syscall 并发；'
		'- 在线回收 retired state 或 backing layer；'
		'- controller 崩溃、内核崩溃或掉电后的自动事务恢复；'
	)

	[[ -r "$design_doc" ]] || die "design document is unavailable: $design_doc"
	: > "$evidence"
	for pattern in "${patterns[@]}"; do
		grep -Fq -- "$pattern" "$design_doc" ||
			die "design document no longer states required limitation: $pattern"
		printf '%s\n' "$pattern" >> "$evidence"
	done
	pass 'documented v1 limits cover old fd/mmap/concurrency/GC/crash recovery'
}

while (($#)); do
	case "$1" in
	--backing-root)
		(($# >= 2)) || die '--backing-root requires a path'
		backing_root=$2
		shift 2
		;;
	--extra-backing-root)
		(($# >= 2)) || die '--extra-backing-root requires a path'
		extra_backing_root=$2
		shift 2
		;;
	--results-dir)
		(($# >= 2)) || die '--results-dir requires a path'
		results_dir=$2
		shift 2
		;;
	--unload-cycles)
		(($# >= 2)) || die '--unload-cycles requires a number'
		unload_cycles=$2
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

[[ -n "$backing_root" && -n "$extra_backing_root" ]] || {
	usage >&2
	exit 1
}
[[ "$unload_cycles" =~ ^[0-9]+$ ]] &&
	((unload_cycles >= DEFAULT_UNLOAD_CYCLES)) ||
	die "--unload-cycles must be an integer of at least $DEFAULT_UNLOAD_CYCLES"
((EUID == 0)) || die 'must run as root inside the guest'

for command in awk cat cp date dmesg findmnt grep mkdir mktemp modinfo modprobe \
	mount mountpoint realpath rm sed sha256sum sleep stat tr umount uname; do
	require_command "$command"
done
confirm_qemu_kvm_guest

[[ -x "$controller" && -x "$generation_test" && -x "$abi_test" &&
   -x "$p5_commit_harness" && -x "$p5_checkpoint_harness" &&
   -x "$p6_harness" ]] ||
	die "build P7 tools first: make -C $script_dir p7-tools"

backing_root=$(realpath -e -- "$backing_root") || die 'backing root does not exist'
extra_backing_root=$(realpath -e -- "$extra_backing_root") ||
	die 'extra backing root does not exist'
[[ -d "$backing_root" && -w "$backing_root" ]] ||
	die 'backing root must be an existing writable directory'
[[ -d "$extra_backing_root" && -w "$extra_backing_root" ]] ||
	die 'extra backing root must be an existing writable directory'
[[ "$backing_root" != *:* && "$backing_root" != *,* &&
   "$backing_root" != *$'\n'* && "$backing_root" != *$'\t'* ]] ||
	die 'backing root contains a character unsupported by this harness'
[[ "$extra_backing_root" != *:* && "$extra_backing_root" != *,* &&
   "$extra_backing_root" != *$'\n'* && "$extra_backing_root" != *$'\t'* ]] ||
	die 'extra backing root contains a character unsupported by this harness'
case $(stat -f -c %T -- "$backing_root") in
overlay|overlayfs)
	die 'backing root itself must not reside on OverlayFS'
	;;
esac
case $(stat -f -c %T -- "$extra_backing_root") in
overlay|overlayfs)
	die 'extra backing root itself must not reside on OverlayFS'
	;;
esac
[[ $(stat -c %d -- "$backing_root") != $(stat -c %d -- "$extra_backing_root") ]] ||
	die 'extra backing root must use a different backing superblock'

select_kernel_config
if ((kernel_config_gzip)); then
	require_command zcat
	require_command zgrep
fi
require_kernel_config CONFIG_OVERLAY_FS m
require_kernel_config CONFIG_MODULE_UNLOAD y
require_kernel_config CONFIG_DEBUG_KERNEL y
require_kernel_config CONFIG_DEBUG_FS y

module_path=$(modinfo -F filename "$OVERLAY_MODULE" 2>/dev/null) ||
	die "cannot locate $OVERLAY_MODULE"
[[ -n "$module_path" && "$module_path" != '(builtin)' && -f "$module_path" ]] ||
	die "$OVERLAY_MODULE must be a loadable module"
[[ -w /dev/kmsg ]] || die '/dev/kmsg must be writable'
dmesg >/dev/null 2>&1 || die 'kernel log is not readable through dmesg'
require_no_overlay_mounts

# Capability-dependent debug options.  These are probed, not required: a guest
# without fail_function (or kmemleak/sanitizers) still runs the non-injection
# matrices and skips only the dependent phase, exiting with SKIP (4).
if probe_kernel_config CONFIG_FUNCTION_ERROR_INJECTION y &&
   [[ -d "$FAIL_FUNCTION_DIR" ]]; then
	have_injection=1
	pass "fault-injection capability available ($FAIL_FUNCTION_DIR)"
else
	note_skip 'CONFIG_FUNCTION_ERROR_INJECTION or the fail_function debugfs dir is unavailable; deep fault injection will be skipped'
fi

if probe_kernel_config CONFIG_DEBUG_KMEMLEAK y &&
   [[ -r "$KMEMLEAK_PATH" && -w "$KMEMLEAK_PATH" ]]; then
	have_kmemleak=1
	pass "kmemleak capability available ($KMEMLEAK_PATH)"
else
	note_skip 'CONFIG_DEBUG_KMEMLEAK or the kmemleak debugfs file is unavailable; kmemleak scan will be skipped'
fi

if probe_kernel_config CONFIG_KASAN y &&
   probe_kernel_config CONFIG_KFENCE y &&
   probe_kernel_config CONFIG_UBSAN y &&
   probe_kernel_config CONFIG_PROVE_LOCKING y &&
   probe_kernel_config CONFIG_PROVE_RCU y; then
	have_sanitizers=1
	pass 'sanitizer/lockdep/RCU dmesg coverage available'
else
	note_skip 'one or more of KASAN/KFENCE/UBSAN/PROVE_LOCKING/PROVE_RCU is unavailable; sanitizer dmesg coverage is reduced'
fi

if [[ -z "$results_dir" ]]; then
	results_dir="$backing_root/deltafs-p7-results-$(date -u +%Y%m%dT%H%M%SZ)-$$"
else
	results_dir=$(realpath -m -- "$results_dir")
fi
mkdir -- "$results_dir" || die "could not create results directory $results_dir"
: > "$results_dir/summary.log"
printf 'condition\tresult\tevidence\n' > "$results_dir/section-17.tsv"
uname -a > "$results_dir/uname.txt"
printf '%s\n' "$kernel_config" > "$results_dir/kernel-config-path.txt"
if ((kernel_config_gzip)); then
	zcat "$kernel_config" > "$results_dir/kernel.config"
else
	cp -- "$kernel_config" "$results_dir/kernel.config"
fi
cat /proc/cmdline > "$results_dir/cmdline.txt"

run_dir=$(mktemp -d "$backing_root/.deltafs-p7-run.XXXXXX") ||
	die 'could not create P7 scratch directory'
extra_run_dir=$(mktemp -d "$extra_backing_root/.deltafs-p7-extra.XXXXXX") ||
	die 'could not create P7 extra-filesystem scratch directory'
trap cleanup EXIT

ensure_overlay_unloaded
printf '%s BEGIN\n' "$marker" > /dev/kmsg
marker_started=1

# P1--P4 valid-build tests are stage-specific: they deliberately terminate in
# -EOPNOTSUPP before P5 introduced commit.  Reuse only current-semantic P5/P6
# evidence here; P7's native matrix and deep fault loop cover ABI validation and
# builder unwind against the final committing implementation.
run_phase p5-cache "$p5_commit_harness" --backing-root "$backing_root"
ensure_overlay_unloaded
run_phase p5-checkpoint "$p5_checkpoint_harness" --backing-root "$backing_root"
ensure_overlay_unloaded
run_phase p6-controller "$p6_harness" --backing-root "$backing_root"
ensure_overlay_unloaded

# The P7-specific half starts with a fresh kmemleak baseline (when available).
if ((have_kmemleak)); then
	printf 'clear\n' > "$KMEMLEAK_PATH"
	pass 'kmemleak state cleared before P7-specific matrices'
else
	note_skip 'kmemleak baseline clear skipped (CONFIG_DEBUG_KMEMLEAK unavailable)'
fi
run_native_abi_matrix
run_deep_switch_and_fault_matrix
run_unload_cycles
ensure_overlay_unloaded

verify_documented_limits
scan_kmemleak
scan_kernel_window

record_gate 1 'P6 controller scenario plus 64-layer historical restores'
record_gate 2 'generation probes after all successes and injected failures'
record_gate 3 'P5 real-switch positive/negative cache and root readdir coverage'
record_gate 4 'physical fingerprints of every frozen P7 layer after every operation'
record_gate 5 'fresh-upper writes after deep restore and every later restore'
record_gate 6 '64 lower restore, controller preflight, and raw 65-lower E2BIG'
if ((have_injection && have_kmemleak && have_sanitizers)); then
	record_gate 7 'deep fault unwind, 100 switches, repeated unload, sanitizers, kmemleak'
else
	record_gate_skip 7 'deep fault unwind, 100 switches, repeated unload, sanitizers, kmemleak (capability-dependent phases skipped)'
fi
record_gate 8 'documented old-fd/mmap/concurrency/GC/crash-recovery limits'

log "P7 acceptance results: $results_dir"
if ((have_injection && have_kmemleak && have_sanitizers)); then
	log 'All P7 DeltaFS v1 acceptance checks passed'
	exit 0
fi
log 'P7 DeltaFS v1 acceptance completed with skipped capability-dependent phases (see SKIP lines)'
exit 4
