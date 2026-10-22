#!/usr/bin/env python3
"""Builds a virtual eMMC for QEMU with the bullhead partitions the menu cares
about, under their real GPT names:

  sbl1, aboot   1 MiB each, filled with a marker: they must never change
  misc          1 MiB of zeros
  boot          32 MiB: out/bootmenu-boot.img (the menu)
  recovery      32 MiB: what was there before the menu (TWRP on the phone;
                here LineageOS recovery.img stands in, or --recovery)
  cache         96 MiB ext4 (Android's make_ext4fs): out/images/ in bootmenu/

Usage: qemu/make-emmc.py [--recovery IMG] [--test-images | --before-install]
  --test-images     also puts a broken and a truncated image into the list
  --before-install  the phone as it is now: LineageOS in boot, no images in cache
Writes out/qemu/emmc.img and out/qemu/emmc-parts.json (offsets + SHA-256).
"""
import argparse, hashlib, json, os, shutil, struct, subprocess, sys, tempfile
import config

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINEAGE = config.LINEAGE()
OUT = os.path.join(P, "out", "qemu")
MiB = 1 << 20
# name, size: bullhead's sizes for boot, recovery and cache (BoardConfig.mk)
LAYOUT = [("sbl1", 1), ("aboot", 1), ("misc", 1), ("boot", 32), ("recovery", 32), ("cache", 96)]
MARKER = {"sbl1": b"SBL1-must-not-change-", "aboot": b"ABOOT-must-not-change-"}

a = argparse.ArgumentParser()
a.add_argument("--recovery", default=os.path.join(LINEAGE, "out/target/product/bullhead/recovery.img"))
a.add_argument("--test-images", action="store_true")
a.add_argument("--before-install", action="store_true",
               help="the phone before phone/install.sh: LineageOS in boot, an empty cache")
args = a.parse_args()

menu = os.path.join(P, "out", "bootmenu-boot.img")
images = os.path.join(P, "out", "images")
for f in (menu, images, args.recovery):
    if not os.path.exists(f):
        sys.exit(f"missing {f} (scripts/build.sh, scripts/collect-images.sh)")
make_ext4fs = os.path.join(LINEAGE, "out/host/linux-x86/bin/make_ext4fs")

os.makedirs(OUT, exist_ok=True)
emmc = os.path.join(OUT, "emmc.img")
total = (sum(s for _, s in LAYOUT) + 2) * MiB          # + 1 MiB in front, 1 MiB for the backup GPT
with open(emmc, "wb") as f:
    f.truncate(total)
cmd = ["sgdisk", "-o"]
start = 2048                                            # sectors of 512 B: first partition at 1 MiB
parts = {}
for i, (name, size) in enumerate(LAYOUT, 1):
    end = start + size * MiB // 512 - 1
    cmd += ["-n", f"{i}:{start}:{end}", "-c", f"{i}:{name}"]
    parts[name] = {"offset": start * 512, "size": size * MiB}
    start = end + 1
subprocess.run(cmd + [emmc], check=True, stdout=subprocess.DEVNULL)


def put(name, data):
    p = parts[name]
    assert len(data) <= p["size"], f"{name}: {len(data)} > {p['size']}"
    with open(emmc, "r+b") as f:
        f.seek(p["offset"])
        f.write(data)


for name, marker in MARKER.items():
    put(name, (marker * (MiB // len(marker) + 1))[:MiB])
put("boot", open(os.path.join(images, "1-LineageOS.img") if args.before_install else menu, "rb").read())
put("recovery", open(args.recovery, "rb").read())

with tempfile.TemporaryDirectory() as tmp:
    d = os.path.join(tmp, "cache", "bootmenu")
    if args.before_install:
        os.makedirs(os.path.join(tmp, "cache", "recovery"))
        open(os.path.join(tmp, "cache", "recovery", "last_log"), "w").write("a file Android left\n")
    else:
        shutil.copytree(images, d)
    if args.test_images:
        open(os.path.join(d, "8-Broken.img"), "wb").write(os.urandom(4096))
        hdr = bytearray(open(os.path.join(images, sorted(os.listdir(images))[0]), "rb").read(4096))
        open(os.path.join(d, "9-Truncated.img"), "wb").write(bytes(hdr) + b"\0" * MiB)
    fs = os.path.join(tmp, "cache.img")
    subprocess.run([make_ext4fs, "-l", str(parts["cache"]["size"]), "-a", "cache", fs,
                    os.path.join(tmp, "cache")], check=True, stdout=subprocess.DEVNULL)
    put("cache", open(fs, "rb").read())

with open(emmc, "rb") as f:
    for name, p in parts.items():
        f.seek(p["offset"])
        p["sha256"] = hashlib.sha256(f.read(p["size"])).hexdigest()
json.dump(parts, open(os.path.join(OUT, "emmc-parts.json"), "w"), indent=1)
for name, p in parts.items():
    print(f"{name:9} {p['offset'] // MiB:4} MiB +{p['size'] // MiB:3} MiB  {p['sha256'][:16]}")
print(emmc)
