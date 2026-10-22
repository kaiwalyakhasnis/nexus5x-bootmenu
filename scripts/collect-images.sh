#!/bin/bash
# Copies the menu's entries into out/images/, named the way the menu lists them
# ("<order>-<Label>.img", '_' shows as a space). Each comes from its project's out/;
# the projects default to clones next to this one (local.env.example).
#   TWRP=/path/twrp.img   adds TWRP as the last entry (recommended on the phone)
set -euo pipefail
P=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$P/local.env" ] && . "$P/local.env"
: "${LINEAGE:?set LINEAGE in local.env (see local.env.example)}"
HIKE_BUDDY=${HIKE_BUDDY:-$P/../nexus5x-hike-buddy}
LOCAL_LLM=${LOCAL_LLM:-$P/../nexus5x-local-llm}
EMBEDDED_COMPOSE=${EMBEDDED_COMPOSE:-$P/../embedded-compose}
rm -rf "$P/out/images"
mkdir -p "$P/out/images"
add() {   # name source
	[ -f "$2" ] || { echo "missing $2" >&2; exit 1; }
	cp "$2" "$P/out/images/$1"
	printf '%-26s %9d  %s\n' "$1" "$(stat -c %s "$2")" "$2"
}
add 1-LineageOS.img        "$LINEAGE/out/target/product/bullhead/boot.img"
add 2-Hike_Buddy.img       "$HIKE_BUDDY/out/hike-buddy-boot.img"
add 3-Local_LLM.img        "$LOCAL_LLM/out/boot.img"
add 4-Embedded_Compose.img "$EMBEDDED_COMPOSE/out/nexus5x/boot.img"
[ -n "${TWRP:-}" ] && add 5-TWRP.img "$TWRP"
du -sh "$P/out/images" | cut -f1 | sed 's/^/total: /'
