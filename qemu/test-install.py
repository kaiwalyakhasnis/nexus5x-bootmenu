#!/usr/bin/env python3
"""Runs phone/install.sh and phone/uninstall.sh against the virtual eMMC, on
the QEMU bullhead kernel, then boots the result with qemu/fake-aboot.py.

The phone's shell tools (toybox) are replaced by busybox, and the phone-only
bits by small shims: getprop, and /dev/block/mmcblk0pN nodes behind by-name
links like the phone's (the virtual eMMC is /dev/vda). /sdcard is a second
virtual disk holding out/phone/ (scripts/package.sh) as bootmenu-install/.

Steps: phone state before install -> install.sh -> install.sh again (must keep
everything) -> the menu boots and lists LineageOS + TWRP + the package's
images -> pick LineageOS -> uninstall.sh -> boot is LineageOS, recovery TWRP.
"""
import hashlib, json, os, re, shutil, signal, subprocess, sys, tempfile, time
import config

Q = os.path.dirname(os.path.abspath(__file__))
P = os.path.dirname(Q)
OUT = os.path.join(P, "out", "qemu")
T = os.path.join(OUT, "test-install")
KIT = config.KIT()
LINEAGE = config.LINEAGE()
MiB = 1 << 20
failures = []
shutil.rmtree(T, ignore_errors=True)
os.makedirs(T)


def check(ok, what):
    print(("  PASS " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


INIT = r"""#!/bin/busybox sh
/bin/busybox mkdir -p /usr/bin /usr/sbin /sbin
/bin/busybox --install -s
export GLIBC_TUNABLES=glibc.pthread.rseq=0 PATH=/bin:/sbin:/usr/bin:/usr/sbin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
log() { echo "T: $*" > /dev/kmsg; }
# the eMMC is the disk with partitions; give them the phone's names
for s in /sys/class/block/vd*[0-9]; do
	n=${s##*/vd?}; name=$(sed -n 's/^PARTNAME=//p' $s/uevent)
	mkdir -p /dev/block/platform/soc.0/f9824900.sdhci/by-name
	mknod /dev/block/mmcblk0p$n b $(tr : ' ' < $s/dev)
	ln -s /dev/block/mmcblk0p$n /dev/block/platform/soc.0/f9824900.sdhci/by-name/$name
done
for d in vda vdb; do [ -e /sys/class/block/${d}1 ] || SD=/dev/$d; done
mkdir -p /cache /sdcard
mount -t ext4 /dev/block/platform/soc.0/f9824900.sdhci/by-name/cache /cache
mount -t ext4 $SD /sdcard
printf '#!/bin/sh\n[ "$1" = ro.product.device ] && echo bullhead\nexit 0\n' > /bin/getprop
chmod 755 /bin/getprop
for step in STEPS; do
	set -- $(echo $step | tr : ' ')
	sh /sdcard/bootmenu-install/$1 $2 2>&1 | while read -r l; do log "$l"; done
	log "RESULT $step $(cat /tmp/rc 2>/dev/null)"
done
log "cache: $(ls /cache/bootmenu 2>&1 | tr '\n' ' ')"
log "backup: $(ls -l /sdcard/bootmenu-backup 2>&1 | tr '\n' ' ')"
sync
umount /cache /sdcard
log "DONE"
poweroff -f
"""


def run_guest(steps, name):
    """Boots the QEMU kernel with a busybox ramdisk that runs the given scripts."""
    with tempfile.TemporaryDirectory() as tmp:
        init = os.path.join(tmp, "init")
        # Each step's exit code: the pipe hides it, so the step writes it to /tmp/rc.
        body = INIT.replace("STEPS", " ".join(steps)).replace(
            "sh /sdcard/bootmenu-install/$1 $2 2>&1",
            "{ sh /sdcard/bootmenu-install/$1 $2; echo $? > /tmp/rc; } 2>&1")
        open(init, "w").write(body)
        lst = os.path.join(tmp, "list")
        open(lst, "w").write(f"""dir /dev 755 0 0
nod /dev/console 600 0 0 c 5 1
dir /proc 755 0 0
dir /sys 755 0 0
dir /tmp 1777 0 0
dir /bin 755 0 0
file /bin/busybox {KIT}/initramfs/busybox 755 0 0
file /init {init} 755 0 0
""")
        rd = os.path.join(tmp, "rd.gz")
        cpio = subprocess.run([os.path.join(KIT, "kobj/usr/gen_init_cpio"), lst], check=True, capture_output=True).stdout
        open(rd, "wb").write(subprocess.run(["gzip", "-1"], input=cpio, check=True, capture_output=True).stdout)
        log = os.path.join(T, f"{name}.log")
        # poweroff -f only halts (no PMIC driver in QEMU): stop QEMU once the log says DONE
        q = subprocess.Popen([
            "qemu-system-aarch64", "-M", "virt,gic-version=2,virtualization=on", "-cpu", "cortex-a57",
            "-smp", "2", "-m", "2048", "-dtb", os.path.join(KIT, "dtb/virt-fb-6-2048.dtb"), "-no-reboot",
            "-display", "none", "-monitor", "none", "-serial", f"file:{log}",
            "-drive", f"file={os.path.join(OUT, 'emmc.img')},if=none,format=raw,id=emmc",
            "-device", "virtio-blk-device,drive=emmc",
            "-drive", f"file={os.path.join(T, 'sdcard.img')},if=none,format=raw,id=sd",
            "-device", "virtio-blk-device,drive=sd",
            "-kernel", os.path.join(KIT, "kobj/arch/arm64/boot/Image"), "-initrd", rd,
            "-append", "earlyprintk=pl011,0x9000000 keep_bootcon mem=1024M",
        ], stdin=subprocess.DEVNULL)
        end = time.time() + 600
        while time.time() < end and q.poll() is None:
            time.sleep(1)
            if os.path.exists(log) and "T: DONE" in open(log, errors="replace").read():
                time.sleep(1)
                break
        q.kill()
        q.wait()
    out = [re.sub(r"^\[[ 0-9.]+\] T: ", "", l) for l in open(log, errors="replace").read().splitlines() if "] T: " in l]
    for l in out:
        print("    " + l)
    return out


def partitions():
    parts = json.load(open(os.path.join(OUT, "emmc-parts.json")))
    with open(os.path.join(OUT, "emmc.img"), "rb") as f:
        for p in parts.values():
            f.seek(p["offset"])
            p["data"] = f.read(p["size"])
    return parts


def cache_files():
    """name -> bytes of /cache/bootmenu/*, read from the ext4 with debugfs."""
    with tempfile.TemporaryDirectory() as tmp:
        fs = os.path.join(tmp, "cache.img")
        open(fs, "wb").write(partitions()["cache"]["data"])
        ls = subprocess.run(["debugfs", "-R", "ls -l /bootmenu", fs], capture_output=True, text=True).stdout
        files = {}
        for name in re.findall(r"\s(\S+\.img)\s*$", ls, re.M):
            dst = os.path.join(tmp, name)
            subprocess.run(["debugfs", "-R", f"dump /bootmenu/{name} {dst}", fs], capture_output=True)
            files[name] = open(dst, "rb").read()
        return files


img = lambda n: open(os.path.join(P, "out", "images", n), "rb").read()
LINEAGE_BOOT = img("1-LineageOS.img")
TWRP = open(os.path.join(LINEAGE, "out/target/product/bullhead/recovery.img"), "rb").read()
MENU = open(os.path.join(P, "out", "bootmenu-boot.img"), "rb").read()

print("phone before install:")
subprocess.run([sys.executable, os.path.join(Q, "make-emmc.py"), "--before-install"], check=True, stdout=subprocess.DEVNULL)
before = partitions()
with tempfile.TemporaryDirectory() as tmp:
    root = os.path.join(tmp, "sd")
    shutil.copytree(os.path.join(P, "out", "phone"), os.path.join(root, "bootmenu-install"))
    subprocess.run([os.path.join(LINEAGE, "out/host/linux-x86/bin/make_ext4fs"), "-l", str(256 * MiB), "-a", "sdcard",
                    os.path.join(T, "sdcard.img"), root], check=True, stdout=subprocess.DEVNULL)

print("install.sh, twice:")
out = run_guest(["install.sh", "install.sh"], "install")
check("RESULT install.sh 0" in out and out.count("RESULT install.sh 0") == 2, "install.sh succeeds, twice")
check(any("boot already holds the menu" in l for l in out), "the second run sees the menu and keeps LineageOS")
after = partitions()
check(after["boot"]["data"][:len(MENU)] == MENU, "boot holds the menu")
check(after["recovery"]["data"] == before["recovery"]["data"], "recovery untouched by install.sh")
for n in ("sbl1", "aboot", "misc"):
    check(after[n]["data"] == before[n]["data"], f"{n} untouched")
files = cache_files()
print("    /cache/bootmenu:", sorted(files))
check(files.get("1-LineageOS.img") == LINEAGE_BOOT, "1-LineageOS.img is the phone's boot partition image")
check(files.get("9-TWRP.img") == TWRP, "9-TWRP.img is the recovery partition's image")
for n in ("2-Hike_Buddy.img", "3-Local_LLM.img", "4-Embedded_Compose.img"):
    check(files.get(n) == img(n), f"{n} copied")

print("the menu after install:")
shutil.rmtree(os.path.join(OUT, "logs"), ignore_errors=True)    # no stale boot-01 log from an earlier run
aboot = subprocess.Popen([sys.executable, os.path.join(Q, "fake-aboot.py"), "--headless"],
                         stdout=open(os.path.join(T, "aboot.out"), "w"), stderr=subprocess.STDOUT)
try:
    def menu_text():
        try:
            return open(os.path.join(OUT, "logs", "boot-01-boot.log"), errors="replace").read()
        except FileNotFoundError:
            return ""
    end = time.time() + 90
    while "menu ready" not in menu_text() and time.time() < end:
        time.sleep(0.5)
    text = menu_text()
    print("    " + "\n    ".join(l.split("] ", 1)[1] for l in text.splitlines() if "bootmenu: image" in l or "menu ready" in l))
    check("9-TWRP.img: ok, in the recovery slot" in text and "default 4, timeout 5 s" in text,
          "TWRP is current and the default")
    subprocess.run([sys.executable, os.path.join(Q, "qmp.py"), "shot", os.path.join(T, "menu.png")], stdout=subprocess.DEVNULL)
    subprocess.run([sys.executable, os.path.join(Q, "qmp.py"), "key", "up", "up", "up", "up", "ret"], check=True)
    end = time.time() + 60
    while "Restarting system" not in menu_text() and time.time() < end:
        time.sleep(0.5)
    check("selected LineageOS" in menu_text() and "written and verified" in menu_text(), "LineageOS picked and written")
finally:
    aboot.send_signal(signal.SIGINT)
    time.sleep(0.3)
    aboot.send_signal(signal.SIGINT)
    aboot.wait(30)
check(partitions()["recovery"]["data"][:len(LINEAGE_BOOT)] == LINEAGE_BOOT, "recovery holds LineageOS")

print("uninstall.sh:")
out = run_guest(["uninstall.sh"], "uninstall")
check("RESULT uninstall.sh 0" in out, "uninstall.sh succeeds")
final = partitions()
check(final["boot"]["data"][:len(LINEAGE_BOOT)] == LINEAGE_BOOT, "boot is LineageOS again")
check(final["recovery"]["data"] == before["recovery"]["data"], "recovery is the original (TWRP) again, byte for byte")
for n in ("sbl1", "aboot", "misc"):
    check(final[n]["data"] == before[n]["data"], f"{n} untouched")

print("\nFAILED: " + "; ".join(failures) if failures else "\nall passed")
sys.exit(1 if failures else 0)
