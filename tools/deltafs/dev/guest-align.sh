#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# deltafs 开发工作流 —— guest 侧 release 对齐（每个新 release 串跑一次，含首次）。
# 做三件事：
#   1. 把最小模块集（9p/xfs/f2fs/overlay 及模块依赖）装进
#      /lib/modules/<新release>/ 并 depmod
#   2. make install 安装新 bzImage（grub 流程）
#   3. 引导你重启并验证 uname -r
# 幂等，可安全重试。

set -euo pipefail

HOSTMNT=/mnt/host
MINIMAL_MODULES=(
	arch/x86/kernel/msr.ko
	drivers/md/dm-multipath.ko
	fs/autofs/autofs4.ko
	fs/nls/nls_iso8859-1.ko
	fs/overlayfs/overlay.ko
	fs/f2fs/f2fs.ko
	crypto/crc32_generic.ko
	fs/xfs/xfs.ko
	lib/lz4/lz4_compress.ko
	lib/lz4/lz4hc_compress.ko
	lib/libcrc32c.ko
	fs/netfs/netfs.ko
	fs/9p/9p.ko
	net/9p/9pnet.ko
	net/9p/9pnet_virtio.ko
)

die() { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARN: $*" >&2; }
step() { echo; echo "==> $*"; }

[ "$(id -u)" = 0 ] || die "需要 root"
mountpoint -q "$HOSTMNT" 2>/dev/null || die "$HOSTMNT 未挂载，先运行 guest-init.sh"

KREL_FILE=$HOSTMNT/include/config/kernel.release
[ -r "$KREL_FILE" ] || die "host 树未配置，先在 host 运行 host-build.sh release"
KREL=$(cat "$KREL_FILE")
GREL=$(uname -r)

step "检查 host 构建产物"
[ -f "$HOSTMNT/arch/x86/boot/bzImage" ] || die "缺 bzImage：先在 host 跑 host-build.sh release"
for m in "${MINIMAL_MODULES[@]}"; do
	[ -f "$HOSTMNT/$m" ] || die "缺 $m：先在 host 跑 host-build.sh release"
done

V=$(modinfo -F vermagic "$HOSTMNT/fs/overlayfs/overlay.ko")
case "$V" in
	"$KREL "*) ;;
	*) die "overlay.ko vermagic($V) 与树 release($KREL) 不一致：在 host 重跑 host-build.sh release" ;;
esac

step "安装最小模块集到 /lib/modules/$KREL"
DST=/lib/modules/$KREL/kernel
for m in "${MINIMAL_MODULES[@]}"; do
	install -D -m 0644 "$HOSTMNT/$m" "$DST/$m"
done
# 让 depmod/modprobe 知道哪些是内建模块（virtio_blk/ext4 等），缺失只影响告警
[ -f "$HOSTMNT/modules.builtin" ] && \
	install -D -m 0644 "$HOSTMNT/modules.builtin" "/lib/modules/$KREL/modules.builtin"
depmod -a "$KREL"

if [ "$GREL" = "$KREL" ]; then
	step "guest 内核已是 $KREL，无需 make install / 重启"
	echo "模块已就位。现在执行:"
	echo "  bash $HOSTMNT/tools/deltafs/dev/guest-reload.sh"
	exit 0
fi

command -v git >/dev/null 2>&1 || \
	warn "guest 无 git：make install 期间若改写共享树 kernel.release 会被校验并中止"

step "安装内核镜像 (make -C $HOSTMNT install)"
make -C "$HOSTMNT" install

# guest 侧 kbuild 在缺 git 等情况下可能重算并改写共享树上的 kernel.release
KREL_AFTER=$(cat "$KREL_FILE")
[ "$KREL_AFTER" = "$KREL" ] || die \
	"kernel.release 被 guest 改写为 '$KREL_AFTER'（原 '$KREL'）。
恢复方法：回 host 重跑 host-build.sh release（重建 bzImage 会顺带恢复该文件），再重跑本脚本"

step "检查 /boot"
boot_entries=()
for f in /boot/*"$KREL"*; do
	if [ -e "$f" ]; then
		boot_entries+=("$f")
	fi
done
if [ "${#boot_entries[@]}" -gt 0 ]; then
	printf '%s\n' "${boot_entries[@]}"
	echo "注意确认 grub 默认启动项指向新内核（/etc/default/grub 的 GRUB_DEFAULT）"
else
	warn "/boot 下没有包含 $KREL 的文件，bootloader 可能没装上，检查 make install 输出"
fi

cat <<EOF

完成。下一步:
  1. reboot
  2. 开机后验证:  uname -r        # 期望输出 $KREL
  3. bash $HOSTMNT/tools/deltafs/dev/guest-init.sh     # 状态应显示「已对齐」
  4. 此后日常迭代:
       host : ./tools/deltafs/dev/host-build.sh module
       guest: bash $HOSTMNT/tools/deltafs/dev/guest-reload.sh
EOF
