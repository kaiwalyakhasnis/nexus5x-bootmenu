#!/system/bin/sh
# One-time setup of the boot menu, on the phone, as root (LineageOS 15.1):
#   adb root; adb shell sh /sdcard/bootmenu-install/install.sh
#
# 1. Backs up boot and recovery to /sdcard/bootmenu-backup/ (first run only).
# 2. Fills /cache/bootmenu/: LineageOS from the phone's own boot partition (so
#    it always matches the installed /system), TWRP from the recovery
#    partition, and the images next to this script (images/*.img).
# 3. Writes bootmenu-boot.img into the boot partition, and checks it.
# The recovery partition is not written here; the menu does that when you pick.
#
# Run it again after flashing a LineageOS zip (the zip replaces boot): the
# new LineageOS boot image becomes the menu's LineageOS entry.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BN=${BN:-/dev/block/platform/soc.0/f9824900.sdhci/by-name}
DIR=${DIR:-/cache/bootmenu}
BK=${BK:-/sdcard/bootmenu-backup}

die() { echo "install: $*" >&2; exit 1; }
say() { echo "install: $*"; }

[ "$(id -u)" = 0 ] || die "run as root (adb root, or su -c sh $0)"
[ "$(getprop ro.product.device)" = bullhead ] || die "this is not a Nexus 5X (bullhead)"

# by-name symlink -> mmcblk0pN; anything else (a whole disk, a missing link) stops here.
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

u32() { od -An -tu4 -j"$2" -N4 < "$1" | tr -d ' \n'; }   # little-endian header field
is_bootimg() { [ "$(head -c 8 "$1")" = "ANDROID!" ]; }
is_menu() { [ "$(dd if="$1" bs=8 skip=6 count=1 2>/dev/null)" = bootmenu ]; }   # header name field
# Bytes a header v0 image really uses: header page + kernel + ramdisk + second (+ a qcom dt).
img_size() {
	ps=$(u32 "$1" 36)
	t=$ps
	for o in 8 16 24; do n=$(u32 "$1" $o); t=$((t + (n + ps - 1) / ps * ps)); done
	dt=$(u32 "$1" 40)
	[ "$dt" -gt 16 ] && t=$((t + (dt + ps - 1) / ps * ps))    # small values are header_version
	echo $t
}
# copy SRC DST [BYTES]: through a temp file, compared before it replaces DST
copy() {
	if [ -n "$3" ]; then
		dd if="$1" of="$2.tmp" bs=4096 count=$(($3 / 4096)) 2>/dev/null
	else
		cat "$1" > "$2.tmp"
	fi
	head -c "$(stat -c %s "$2.tmp")" "$1" | cmp -s - "$2.tmp" || die "copy of $1 differs"
	mv "$2.tmp" "$2"
}

MENU=$HERE/bootmenu-boot.img
[ -f "$MENU" ] && is_bootimg "$MENU" && is_menu "$MENU" || die "no bootmenu-boot.img next to this script"
[ "$(stat -c %s "$MENU")" -le "$(blockdev --getsize64 "$BOOT")" ] || die "bootmenu-boot.img is bigger than boot"

# 1. Backups, before anything is written.
mkdir -p "$BK"
if [ ! -f "$BK/boot.img" ]; then
	is_menu "$BOOT" && die "boot already holds the menu but $BK/boot.img is missing"
	dd if="$BOOT" of="$BK/boot.img" bs=1048576 2>/dev/null
	dd if="$RECOVERY" of="$BK/recovery.img" bs=1048576 2>/dev/null
	say "backed up boot and recovery to $BK/"
fi

# 2. The images. All of them are in place before boot is touched.
# Space: every image that isn't there already (a changed one briefly exists twice).
need=0
for f in "$HERE"/images/*.img; do
	[ -f "$f" ] || continue
	is_bootimg "$f" || die "$f is not a boot image"
	cmp -s "$f" "$DIR/$(basename "$f")" || need=$((need + $(stat -c %s "$f")))
done
is_menu "$BOOT" || need=$((need + $(img_size "$BOOT")))
[ ! -f "$DIR/9-TWRP.img" ] && is_bootimg "$BK/recovery.img" && need=$((need + $(img_size "$BK/recovery.img")))
free=$(($(df -k /cache | awk 'END { print $(NF - 2) }') * 1024))   # "Available": long device names wrap
[ "$need" -lt "$free" ] || die "/cache has $((free >> 20)) MB free, the images need $((need >> 20)) MB"
mkdir -p "$DIR"

if is_menu "$BOOT"; then
	[ -f "$DIR/1-LineageOS.img" ] || die "boot holds the menu and $DIR has no LineageOS image"
	say "boot already holds the menu: keeping $DIR/1-LineageOS.img"
else
	is_bootimg "$BOOT" || die "boot holds no boot image"
	copy "$BOOT" "$DIR/1-LineageOS.img" "$(img_size "$BOOT")"
	say "LineageOS: the current boot partition -> $DIR/1-LineageOS.img"
fi
if [ ! -f "$DIR/9-TWRP.img" ] && is_bootimg "$BK/recovery.img"; then
	copy "$BK/recovery.img" "$DIR/9-TWRP.img" "$(img_size "$BK/recovery.img")"
	say "TWRP: the backed-up recovery partition -> $DIR/9-TWRP.img"
fi
for f in "$HERE"/images/*.img; do
	[ -f "$f" ] || continue
	if cmp -s "$f" "$DIR/$(basename "$f")"; then
		say "$(basename "$f"): already in $DIR"
	else
		copy "$f" "$DIR/$(basename "$f")"
		say "$(basename "$f") -> $DIR/"
	fi
done
sync

# 3. The menu into boot, read back.
dd if="$MENU" of="$BOOT" bs=4096 2>/dev/null
sync
n=$(stat -c %s "$MENU")
[ "$(head -c "$n" "$BOOT" | sha1sum | cut -d' ' -f1)" = "$(sha1sum "$MENU" | cut -d' ' -f1)" ] ||
	die "boot read back differs! Restore it: dd if=$DIR/1-LineageOS.img of=$BOOT"
say "boot menu written to $BOOT and verified"

[ "$(getprop persist.sys.recovery_update)" = true ] &&
	say "WARNING: 'Update recovery' is on; LineageOS will rewrite the recovery slot at boot. Turn it off in Developer options."
ls -l "$DIR"
say "done. Reboot: the menu shows up; pick LineageOS once, then it is the default."
