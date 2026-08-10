#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Host-safe controller transaction test.  The fake merged directory is not
# OverlayFS, so the ioctl terminates with ENOTTY after the controller has
# journaled, created a fresh branch and renamed the active upper.  The test
# verifies that this synchronous failure is compensated completely.

set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
controller=${DELTAFSCTL:-"$script_dir/deltafsctl"}
fixture="$script_dir/testdata/state-v1-initial.json"
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
	if [[ -n "$run_dir" && -d "$run_dir" ]]; then
		rm -rf -- "$run_dir"
	fi
	exit "$status"
}

[[ -x "$controller" ]] ||
	die "build the controller first: make -C $script_dir deltafsctl"
[[ -f "$fixture" ]] || die "missing state fixture: $fixture"

run_dir=$(mktemp -d)
trap cleanup EXIT
mkdir -p -- \
	"$run_dir/base" \
	"$run_dir/layers" \
	"$run_dir/branches/g1/upper" \
	"$run_dir/branches/g1/work" \
	"$run_dir/meta" \
	"$run_dir/merged"
cp -- "$fixture" "$run_dir/meta/state.json"
cp -- "$fixture" "$run_dir/meta/state.before"
printf 'active\n' > "$run_dir/branches/g1/upper/marker"

if "$controller" --assume-quiesced checkpoint "$run_dir" A; then
	die 'checkpoint against a non-OverlayFS merged directory succeeded'
fi

cmp -- "$run_dir/meta/state.before" "$run_dir/meta/state.json" ||
	die 'failed ioctl changed state.json'
[[ -f "$run_dir/branches/g1/upper/marker" ]] ||
	die 'failed ioctl did not restore active upper'
[[ ! -e "$run_dir/branches/g2" ]] ||
	die 'failed ioctl left the fresh branch behind'
[[ ! -e "$run_dir/layers/A" ]] ||
	die 'failed ioctl left the frozen layer name behind'
[[ ! -e "$run_dir/meta/transaction.json" &&
   ! -e "$run_dir/meta/.transaction.json.tmp" &&
   ! -e "$run_dir/meta/.state.json.tmp" ]] ||
	die 'failed ioctl left transaction metadata behind'

if "$controller" --assume-quiesced restore "$run_dir" missing; then
	die 'restore of an unknown checkpoint succeeded'
fi
[[ ! -e "$run_dir/branches/g2" ]] ||
	die 'unknown restore mutated the branch tree'

printf '{}\n' > "$run_dir/meta/transaction.json"
if "$controller" --assume-quiesced checkpoint "$run_dir" blocked; then
	die 'controller ignored an unresolved transaction'
fi
[[ ! -e "$run_dir/layers/blocked" ]] ||
	die 'blocked operation mutated the layer tree'

mkdir -p -- \
	"$run_dir/missing-target/base" \
	"$run_dir/missing-target/layers" \
	"$run_dir/missing-target/branches/g1/work" \
	"$run_dir/missing-target/branches/g2/upper" \
	"$run_dir/missing-target/branches/g2/work" \
	"$run_dir/missing-target/meta" \
	"$run_dir/missing-target/merged"
cat > "$run_dir/missing-target/meta/state.json" <<'EOF'
{
  "format": 1,
  "kernel_generation": 2,
  "active_branch": "g2",
  "active_lowers": ["layers/A", "base"],
  "retired_branches": ["g1"],
  "snapshots": {
    "A": {"lowers": ["layers/A", "base"]}
  }
}
EOF
if "$controller" --assume-quiesced restore "$run_dir/missing-target" A; then
	die 'restore accepted a missing frozen target layer'
fi
[[ ! -e "$run_dir/missing-target/meta/transaction.json" &&
   ! -e "$run_dir/missing-target/branches/g3" ]] ||
	die 'missing restore target was discovered after transaction mutation'

mkdir -p -- \
	"$run_dir/cycle/base" \
	"$run_dir/cycle/layers" \
	"$run_dir/cycle/branches/g1/work" \
	"$run_dir/cycle/branches/g2/work" \
	"$run_dir/cycle/branches/g3/upper" \
	"$run_dir/cycle/branches/g3/work" \
	"$run_dir/cycle/meta" \
	"$run_dir/cycle/merged"
cat > "$run_dir/cycle/meta/state.json" <<'EOF'
{
  "format": 1,
  "kernel_generation": 3,
  "active_branch": "g3",
  "active_lowers": ["layers/A", "layers/B", "base"],
  "retired_branches": ["g1", "g2"],
  "snapshots": {
    "A": {"lowers": ["layers/A", "layers/B", "base"]},
    "B": {"lowers": ["layers/B", "layers/A", "base"]}
  }
}
EOF
if "$controller" --assume-quiesced restore "$run_dir/cycle" A; then
	die 'controller accepted cyclic snapshot metadata'
fi
[[ ! -e "$run_dir/cycle/meta/transaction.json" ]] ||
	die 'cyclic metadata created a transaction'

printf 'All host-safe P6 controller transaction tests passed\n'
