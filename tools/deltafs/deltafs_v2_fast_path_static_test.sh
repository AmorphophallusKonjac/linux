#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Static regression gate for the two ioctl fixed-cost optimizations.

set -Eeuo pipefail

delta_source=${1:-../../fs/overlayfs/deltafs.c}
super_source=${2:-../../fs/overlayfs/super.c}
for source in "$delta_source" "$super_source"; do
    [[ -r "$source" ]] || {
        printf 'FAIL: kernel source is not readable: %s\n' "$source" >&2
        exit 1
    }
done

extract_function() {
    local name=$1
    local source=$2
    sed -n "/${name}[[:space:](]/,/^}[[:space:]]*$/p" \
        "$source"
}

fast=$(extract_function ovl_make_workdir_fast "$super_source")
[[ -n "$fast" ]] || {
    printf 'FAIL: ovl_make_workdir_fast is missing\n' >&2
    exit 1
}

for probe in \
    ovl_check_d_type_supported ovl_do_tmpfile ovl_check_rename_whiteout \
    ovl_can_decode_fh ovl_setxattr ovl_removexattr; do
    if grep -q "$probe" <<<"$fast"; then
        printf 'FAIL: fast helper still probes %s\n' "$probe" >&2
        exit 1
    fi
done

static=$(extract_function ovl_deltafs_validate_mount_static "$delta_source")
final=$(extract_function ovl_deltafs_final_revalidate_locked "$delta_source")
if grep -Eq '\bfor[[:space:]]*\(' <<<"$static"; then
    printf 'FAIL: mount-static validation still scans layers\n' >&2
    exit 1
fi
if grep -Eq '\bfor[[:space:]]*\(' <<<"$final"; then
    printf 'FAIL: final revalidation still scans layers\n' >&2
    exit 1
fi

grep -q 'ovl_make_workdir_fast' "$delta_source" || {
    printf 'FAIL: DeltaFS builder does not reference fast helper\n' >&2
    exit 1
}
printf 'PASS: fast workdir and O(1) active-view validation gates passed\n'
