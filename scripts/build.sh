#!/bin/bash
# The boot menu image: LineageOS's bullhead kernel + embedded-compose's
# initramfs (busybox init + the menu app in menu/, built with ./ec from an
# embedded-compose checkout).
#   out/bootmenu-boot.img        for the phone's boot partition (board nexus5x)
#   out/bootmenu-boot-qemu.img   the same app for qemu/ (board nexus5x-qemu)
# Settings (local.env): LINEAGE, EMBEDDED_COMPOSE (default ../embedded-compose),
#   KERNEL (Image.gz-dtb, default the LineageOS build's), TIMEOUT (auto-boot s, 0: none).
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$P/local.env" ] && . "$P/local.env"
: "${LINEAGE:?set LINEAGE in local.env (see local.env.example)}"
EC=${EMBEDDED_COMPOSE:-$P/../embedded-compose}
KERNEL=${KERNEL:-$LINEAGE/out/target/product/bullhead/kernel}
TIMEOUT=${TIMEOUT:-0}
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL (build LineageOS or set KERNEL=)" >&2; exit 1; }
[ -x "$EC/ec" ] || { echo "no embedded-compose at $EC (set EMBEDDED_COMPOSE=)" >&2; exit 1; }
mkdir -p "$P/out"

for board in nexus5x nexus5x-qemu; do
	# EC_POWER_REBOOT=0: Power selects in the menu; holding it must not reboot.
	APP_DIR="$P/menu" KERNEL="$KERNEL" EC_ENV_EXTRA="EC_POWER_REBOOT=0" "$EC/ec" build "$board"
	out=$P/out/bootmenu-boot.img
	[ "$board" = nexus5x-qemu ] && out=$P/out/bootmenu-boot-qemu.img
	# Stock bullhead cmdline minus the UART console and the Android-only bits.
	# GLIBC_TUNABLES: glibc >= 2.35 registers rseq, which the 3.10 kernel lacks;
	# unknown NAME=value parameters become /init's environment.
	python3 "$P/scripts/mkbootimg.py" \
		--kernel "$KERNEL" --ramdisk "$EC/out/$board-menu/initramfs.cpio.gz" \
		--cmdline "boot_cpus=0-5 lpm_levels.sleep_disabled=1 msm_poweroff.download_mode=0 GLIBC_TUNABLES=glibc.pthread.rseq=0 bootmenu.timeout=$TIMEOUT" \
		--base 0x00000000 --pagesize 4096 --ramdisk_offset 0x02000000 --tags_offset 0x01E00000 \
		-o "$out"
done
