#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# Host-safe DeltaFS v2 controller checks.  The fake merged directory makes the
# ioctl fail with ENOTTY after the durable transaction and backing mutations;
# the controller must compensate before returning to the caller.

set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
controller=${DELTAFSCTL:-"$script_dir/deltafsctl"}
run_dir=

die()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

cleanup()
{
	local status=$?
	trap - EXIT
	if [[ -n "$run_dir" ]]; then
		rm -rf -- "$run_dir"
	fi
	exit "$status"
}

[[ -x "$controller" ]] ||
	die "build the controller first: make -C $script_dir deltafsctl"

run_dir=$(mktemp -d)
trap cleanup EXIT

make_sandbox()
{
	local root=$1
	local fixture=$2

	mkdir -p -- "$root"/{base,layers,meta,merged}
	mkdir -p -- "$root"/branches/g1/{upper,work}
	cp -- "$fixture" "$root/meta/state.json"
}

# v1 metadata is rejected before any directory or transaction is touched.
legacy="$run_dir/legacy"
make_sandbox "$legacy" "$script_dir/testdata/state-v1-initial.json"
if "$controller" --assume-quiesced checkpoint "$legacy" rejected; then
	die 'format-1 state was accepted by the v2 controller'
fi
[[ ! -e "$legacy/meta/transaction.json" &&
	! -e "$legacy/branches/g2" ]] ||
	die 'format-1 rejection mutated the sandbox'

# A checkpoint against a non-OverlayFS fd exercises the v2 request path and
# verifies that the renamed upper and fresh branch are restored on failure.
checkpoint="$run_dir/checkpoint"
make_sandbox "$checkpoint" "$script_dir/testdata/state-v2-initial.json"
printf 'active\n' > "$checkpoint/branches/g1/upper/marker"
if "$controller" --assume-quiesced checkpoint "$checkpoint" A; then
	die 'checkpoint against a non-OverlayFS merged directory succeeded'
fi
[[ -f "$checkpoint/branches/g1/upper/marker" ]] ||
	die 'failed checkpoint did not restore active upper'
[[ ! -e "$checkpoint/branches/g2" &&
	! -e "$checkpoint/layers/A" ]] ||
	die 'failed checkpoint left backing-tree mutations'
[[ ! -e "$checkpoint/meta/transaction.json" &&
	! -e "$checkpoint/meta/.transaction.json.tmp" &&
	! -e "$checkpoint/meta/.state.json.tmp" ]] ||
	die 'failed checkpoint left transaction metadata'

# Restore A from an active B -> A -> base chain.  The controller must derive
# keep_bottom=2 and pass an empty lower prefix; ENOTTY still has to compensate.
restore="$run_dir/restore"
mkdir -p -- "$restore"/{base,layers,meta,merged}
mkdir -p -- "$restore"/layers/{A,B}
mkdir -p -- "$restore"/branches/{g1,g2}
mkdir -p -- "$restore"/branches/g3/{upper,work}
cat > "$restore/meta/state.json" <<'EOF'
{
  "format": 2,
  "kernel_generation": 3,
  "active_branch": "g3",
  "active_lowers": ["layers/B", "layers/A", "base"],
  "retired_branches": ["g1", "g2"],
  "snapshots": {
    "A": {"lowers": ["layers/A", "base"]},
    "B": {"lowers": ["layers/B", "layers/A", "base"]}
  }
}
EOF
if "$controller" --assume-quiesced restore "$restore" A; then
	die 'restore against a non-OverlayFS merged directory succeeded'
fi
[[ ! -e "$restore/branches/g4" &&
	! -e "$restore/meta/transaction.json" ]] ||
	die 'failed restore left fresh branch or transaction metadata'

# The format-2 transaction schema is part of the durable controller contract;
# inspect the source so a future edit cannot silently drop these fields.
grep -Fq 'keep_bottom' "$script_dir/deltafsctl.c" ||
	die 'controller does not serialize keep_bottom'
grep -Fq 'new_lower_prefix' "$script_dir/deltafsctl.c" ||
	die 'controller does not serialize new_lower_prefix'
grep -Fq 'longest_common_suffix' "$script_dir/deltafsctl.c" ||
	die 'controller does not compute the common suffix'

printf 'All DeltaFS v2 controller unit tests passed\n'
