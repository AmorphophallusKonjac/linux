#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Static P7 regression gate: ownership fault checkpoints must survive -O2.

set -Eeuo pipefail

readonly SYMBOL=ovl_deltafs_build_checkpoint

object=${1:-../../fs/overlayfs/deltafs.o}
source=${2:-../../fs/overlayfs/deltafs.c}

command -v objdump >/dev/null 2>&1 || {
	printf 'FAIL: objdump is required\n' >&2
	exit 1
}
[[ -r "$object" ]] || {
	printf 'FAIL: kernel object is not readable: %s\n' "$object" >&2
	exit 1
}
[[ -r "$source" ]] || {
	printf 'FAIL: kernel source is not readable: %s\n' "$source" >&2
	exit 1
}

source_calls=$(awk -v symbol="$SYMBOL" '
	{
		line = $0
		sub(/^[[:space:]]*/, "", line)
		if (index(line, symbol "(") &&
		    line !~ /^(\/\*|\*|\/\/|noinline|ALLOW_ERROR_INJECTION)/)
			calls++
	}
	END { print calls + 0 }
' "$source")

if ((source_calls == 0)); then
	printf 'FAIL: no %s call sites found in %s\n' "$SYMBOL" "$source" >&2
	exit 1
fi

calls=$(objdump -dr -- "$object" | awk -v symbol="$SYMBOL" '
	$0 ~ /R_[[:alnum:]_]+/ && index($0, symbol) { calls++ }
	END { print calls + 0 }
')

if ((calls != source_calls)); then
	printf 'FAIL: %s retains %d of %d source %s call site(s)\n' \
		"$object" "$calls" "$source_calls" "$SYMBOL" >&2
	exit 1
fi

printf 'PASS: %s retains all %d source %s call sites\n' \
	"$object" "$calls" "$SYMBOL"
