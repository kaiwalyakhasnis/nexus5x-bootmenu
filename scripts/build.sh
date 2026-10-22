#!/bin/bash
# The boot menu image: LineageOS's bullhead kernel + a ramdisk whose /init is
# out/menu/bootmenu.  -> out/bootmenu-boot.img, for the boot partition.
#   KERNEL=   Image.gz-dtb to use (default: the LineageOS build's)
#   TIMEOUT=  auto-boot seconds (default 5, 0 = wait for a key)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$P/local.env" ] && . "$P/local.env"
: "${LINEAGE:?set LINEAGE in local.env (see local.env.example)}"
KERNEL=${KERNEL:-$LINEAGE/out/target/product/bullhead/kernel}
GEN_INIT_CPIO=${GEN_INIT_CPIO:-$LINEAGE/out/target/product/bullhead/obj/KERNEL_OBJ/usr/gen_init_cpio}
TIMEOUT=${TIMEOUT:-5}
[ -f "$KERNEL" ] || { echo "no kernel at $KERNEL (build LineageOS or set KERNEL=)" >&2; exit 1; }
[ -x "$GEN_INIT_CPIO" ] || { echo "no gen_init_cpio at $GEN_INIT_CPIO (set GEN_INIT_CPIO=)" >&2; exit 1; }

make -C "$P/menu" -s
cat > "$P/out/ramdisk.list" <<LIST
dir /dev 755 0 0
dir /proc 755 0 0
dir /sys 755 0 0
dir /cache 755 0 0
file /init $P/out/menu/bootmenu 755 0 0
LIST
"$GEN_INIT_CPIO" "$P/out/ramdisk.list" | gzip -9 > "$P/out/ramdisk.cpio.gz"

# Stock bullhead cmdline minus the UART console and the Android-only bits.
# GLIBC_TUNABLES: glibc >= 2.35 registers rseq, which the 3.10 kernel lacks;
# unknown NAME=value parameters become /init's environment.
python3 "$P/scripts/mkbootimg.py" \
	--kernel "$KERNEL" --ramdisk "$P/out/ramdisk.cpio.gz" \
	--cmdline "boot_cpus=0-5 lpm_levels.sleep_disabled=1 msm_poweroff.download_mode=0 GLIBC_TUNABLES=glibc.pthread.rseq=0 bootmenu.timeout=$TIMEOUT" \
	--base 0x00000000 --pagesize 4096 --ramdisk_offset 0x02000000 --tags_offset 0x01E00000 \
	-o "$P/out/bootmenu-boot.img"
