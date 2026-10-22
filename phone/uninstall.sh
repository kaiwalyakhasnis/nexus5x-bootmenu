#!/system/bin/sh
# Puts the phone back as it was before install.sh, as root:
#   adb root; adb shell sh /sdcard/bootmenu-install/uninstall.sh [--purge]
# boot <- the menu's LineageOS image (the boot partition from before the menu,
# or a newer one if install.sh ran again after a LineageOS update),
# recovery <- TWRP from /sdcard/bootmenu-backup/recovery.img.
# --purge also deletes /cache/bootmenu/.
set -e
BN=${BN:-/dev/block/platform/soc.0/f9824900.sdhci/by-name}
DIR=${DIR:-/cache/bootmenu}
BK=${BK:-/sdcard/bootmenu-backup}

die() { echo "uninstall: $*" >&2; exit 1; }
say() { echo "uninstall: $*"; }

[ "$(id -u)" = 0 ] || die "run as root (adb root, or su -c sh $0)"
[ "$(getprop ro.product.device)" = bullhead ] || die "this is not a Nexus 5X (bullhead)"
part() {
	d=$(readlink -f "$BN/$1") || die "no $1 partition"
	case "$d" in
	/dev/block/mmcblk0p[0-9]*) echo "$d" ;;
	*) die "$1 is '$d', expected /dev/block/mmcblk0pN" ;;
	esac
}
BOOT=$(part boot)
RECOVERY=$(part recovery)
[ "$BOOT" != "$RECOVERY" ] || die "boot and recovery are the same device"

# write IMAGE PARTITION: only boot images, read back after writing
write() {
	[ "$(head -c 8 "$1")" = "ANDROID!" ] || die "$1 is not a boot image"
	[ "$(stat -c %s "$1")" -le "$(blockdev --getsize64 "$2")" ] || die "$1 is bigger than $2"
	dd if="$1" of="$2" bs=4096 2>/dev/null
	sync
	n=$(stat -c %s "$1")
	[ "$(head -c "$n" "$2" | sha1sum | cut -d' ' -f1)" = "$(sha1sum "$1" | cut -d' ' -f1)" ] ||
		die "$2 read back differs from $1"
	say "$1 -> $2"
}

LINEAGE=$DIR/1-LineageOS.img
[ -f "$LINEAGE" ] || LINEAGE=$BK/boot.img
[ -f "$LINEAGE" ] || die "no LineageOS boot image in $DIR or $BK"
[ -f "$BK/recovery.img" ] || die "no $BK/recovery.img"
write "$LINEAGE" "$BOOT"
write "$BK/recovery.img" "$RECOVERY"
if [ "$1" = --purge ]; then
	rm -rf "$DIR"
	say "removed $DIR"
fi
say "done: boot is LineageOS, recovery is TWRP again"
