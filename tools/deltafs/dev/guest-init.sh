#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# deltafs 开发工作流 —— guest 侧开机初始化 + 对齐状态检查。
# 在 QEMU guest 里以 root 运行（通常直接跑 9p 共享里的副本）:
#   bash /mnt/host/tools/deltafs/dev/guest-init.sh
#
# 首次 9p 尚未挂载时，先手工执行一次:
#   mkdir -p /mnt/host
#   modprobe 9pnet_virtio; modprobe 9p
#   mount -t 9p -o trans=virtio,version=9p2000.L host /mnt/host

set -euo pipefail

HOSTMNT=/mnt/host

die() { echo "ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "需要 root"

if ! mountpoint -q "$HOSTMNT" 2>/dev/null; then
	mkdir -p "$HOSTMNT"
	modprobe 9pnet_virtio 2>/dev/null || true
	modprobe 9p 2>/dev/null || true
	mount -t 9p -o trans=virtio,version=9p2000.L host "$HOSTMNT" \
		|| die "9p 挂载失败（QEMU 启动参数需要 -virtfs local,path=<树>,mount_tag=host,security_model=none）"
fi
echo "9p 共享         : $HOSTMNT 已挂载"

if ! mountpoint -q /sys/kernel/debug 2>/dev/null; then
	mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null \
		|| echo "WARN: debugfs 挂载失败，部分测试需要它"
fi

KREL_FILE=$HOSTMNT/include/config/kernel.release
[ -r "$KREL_FILE" ] || die "host 树未配置，先在 host 运行 tools/deltafs/dev/host-build.sh release"

KREL=$(cat "$KREL_FILE")
GREL=$(uname -r)
echo "guest uname -r  : $GREL"
echo "host 树 release : $KREL"

if [ "$GREL" = "$KREL" ]; then
	echo "状态            : 已对齐"
	echo "下一步          : bash $HOSTMNT/tools/deltafs/dev/guest-reload.sh   # 加载/热替换 overlay.ko"
else
	echo "状态            : 未对齐（release 不一致，insmod 会报 Invalid module format）"
	echo "下一步          : bash $HOSTMNT/tools/deltafs/dev/guest-align.sh    # 一次性对齐（需要重启一次）"
fi
