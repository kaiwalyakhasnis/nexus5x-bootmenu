#!/bin/bash
# out/phone/: what goes onto the phone for phone/install.sh. LineageOS and TWRP
# are not in it: install.sh takes them from the phone's own boot and recovery.
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$P/out/bootmenu-boot.img" ] || { echo "run scripts/build.sh first" >&2; exit 1; }
[ -d "$P/out/images" ] || { echo "run scripts/collect-images.sh first" >&2; exit 1; }
rm -rf "$P/out/phone"
mkdir -p "$P/out/phone/images"
cp "$P/phone/install.sh" "$P/phone/uninstall.sh" "$P/out/bootmenu-boot.img" "$P/out/phone/"
for f in "$P"/out/images/*.img; do
	case "$(basename "$f")" in 1-LineageOS.img|*TWRP*) continue ;; esac
	cp "$f" "$P/out/phone/images/"
done
ls -l "$P/out/phone" "$P/out/phone/images"
