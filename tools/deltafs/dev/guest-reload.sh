#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# deltafs 开发工作流 —— guest 侧 overlay.ko 热替换（日常迭代，不重启）。
# 前提：guest 已对齐（uname -r == host 树 release），host 已跑 host-build.sh module。
# 幂等，可安全重试。

set -euo pipefail

HOSTMNT=/mnt/host

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "需要 root"
mountpoint -q "$HOSTMNT" 2>/dev/null || die "$HOSTMNT 未挂载，先运行 guest-init.sh"

KO=$HOSTMNT/fs/overlayfs/overlay.ko
[ -f "$KO" ] || die "缺 $KO：先在 host 跑 host-build.sh module"

KREL_FILE=$HOSTMNT/include/config/kernel.release
[ -r "$KREL_FILE" ] || die "host 树未配置，先在 host 运行 host-build.sh release"
KREL=$(cat "$KREL_FILE")
GREL=$(uname -r)
[ "$GREL" = "$KREL" ] || die "release 不一致 (guest: $GREL / 树: $KREL)，先跑 guest-align.sh"

V=$(modinfo -F vermagic "$KO")
case "$V" in
	"$GREL "*) ;;
	*) die "overlay.ko vermagic($V) 与运行内核($GREL) 不一致：在 host 重跑 host-build.sh module" ;;
esac

echo "==> 卸载所有 overlay 挂载"
for _ in 1 2 3; do
	awk '$3=="overlay"{print $2}' /proc/mounts | sort -r | \
		xargs -r -n1 umount 2>/dev/null || true
done
LEFT=$(awk '$3=="overlay"{print $2}' /proc/mounts)
[ -z "$LEFT" ] || die "仍有 overlay 挂载卸不掉:
$LEFT
排查: 残留测试进程占用 (lsof / ps aux | grep <测试名>)、cwd 遗留在挂载点内"

echo "==> 卸载旧模块"
if grep -q '^overlay ' /proc/modules; then
	modprobe -r overlay 2>/dev/null || rmmod overlay
fi
if grep -q '^overlay ' /proc/modules; then
	die "overlay 模块仍在使用（引用计数未归零），先释放上述挂载/进程后重试"
fi

echo "==> 加载新模块: $KO"
insmod "$KO" || { dmesg | tail -n 20; die "insmod 失败"; }

grep '^overlay ' /proc/modules
echo "==> 完成，dmesg 末尾:"
dmesg | tail -n 5
