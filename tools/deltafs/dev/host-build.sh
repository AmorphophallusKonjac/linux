#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# deltafs 开发工作流 —— host 侧编译脚本。
# 只在 host 上运行；guest 侧脚本见同目录 guest-init.sh / guest-align.sh / guest-reload.sh。

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
KROOT=$(cd "$SCRIPT_DIR/../../.." && pwd)
J=$(nproc)

# guest 里真正会用到的模块全集。启动所需驱动（virtio/ext4）全部内建，
# 此处必须包含手工选定模块的完整模块依赖闭包。
MINIMAL_MODULES=(
	arch/x86/kernel/msr.ko
	drivers/md/dm-multipath.ko
	fs/autofs/autofs4.ko
	fs/nls/nls_iso8859-1.ko
	fs/overlayfs/overlay.ko
	fs/xfs/xfs.ko
	lib/libcrc32c.ko
	fs/netfs/netfs.ko
	fs/9p/9p.ko
	net/9p/9pnet.ko
	net/9p/9pnet_virtio.ko
)

die() { echo "ERROR: $*" >&2; exit 1; }

usage() {
	cat <<'EOF'
用法: host-build.sh <命令>

  release   release 对齐构建：bzImage + 最小模块集 + 工具（每个新 release 串跑一次）
  module    日常迭代：只重编 overlay.ko + 工具（改 fs/overlayfs 或 UAPI 头后跑）
  tools     只编 tools/deltafs 测试工具
  status    查看 release / vermagic / 产物状态
EOF
}

cmd_release() {
	echo "==> 检查最小模块依赖闭包"
	"$SCRIPT_DIR/minimal-modules-test.sh"

	echo "==> make -j$J bzImage"
	make -C "$KROOT" -j"$J" bzImage

	echo "==> 最小模块集: ${MINIMAL_MODULES[*]}"
	make -C "$KROOT" -j"$J" "${MINIMAL_MODULES[@]}"

	cmd_tools
	cmd_status
	echo
	echo "==> host 侧完成。在 guest 里执行:"
	echo "      bash /mnt/host/tools/deltafs/dev/guest-align.sh"
}

cmd_module() {
	echo "==> make -j$J M=fs/overlayfs modules"
	make -C "$KROOT" -j"$J" M=fs/overlayfs modules

	cmd_tools
	echo
	echo "==> host 侧完成。在 guest 里执行:"
	echo "      bash /mnt/host/tools/deltafs/dev/guest-reload.sh"
}

cmd_tools() {
	echo "==> make -C tools/deltafs v2-tools"
	make -C "$KROOT/tools/deltafs" v2-tools
}

cmd_status() {
	local rel v f
	rel=$(cat "$KROOT/include/config/kernel.release" 2>/dev/null) ||
		die "树未配置（缺 include/config/kernel.release），先 make olddefconfig && make prepare"
	echo "kernel.release      : $rel"

	for f in "arch/x86/boot/bzImage" "${MINIMAL_MODULES[@]}"; do
		if [ -f "$KROOT/$f" ]; then
			printf '%-30s %s %10s bytes\n' "$f" \
				"$(date -r "$KROOT/$f" '+%F %T')" "$(stat -c %s "$KROOT/$f")"
		else
			printf '%-30s %s\n' "$f" "MISSING"
		fi
	done

	if [ -f "$KROOT/fs/overlayfs/overlay.ko" ]; then
		v=$(modinfo -F vermagic "$KROOT/fs/overlayfs/overlay.ko")
		echo "overlay.ko vermagic : $v"
		case "$v" in
			"$rel "*) echo "vermagic 与 release 一致: OK" ;;
			*) die "overlay.ko vermagic 与 kernel.release 不一致，重跑 host-build.sh module" ;;
		esac
	fi
	echo "guest 侧对齐状态在 guest 里运行 guest-init.sh 查看"
}

case "${1:-}" in
	release) cmd_release ;;
	module) cmd_module ;;
	tools) cmd_tools ;;
	status) cmd_status ;;
	*) usage; exit 1 ;;
esac
