#!/usr/bin/env python3
"""Plays the Nexus 5X bootloader (aboot/LK) and power button for QEMU.

Each "power on" it does what aboot does: read the reboot reason, pick the
boot or the recovery partition of the virtual eMMC (qemu/make-emmc.py),
load that boot image and start it. Two things are emulated, the rest is real:

- The kernel: QEMU has no Snapdragon 808, so every image is started with the
  QEMU build of the same LineageOS bullhead kernel source (the QEMU kit
  of nexus5x-recovery-roadfighter, NEXUS5X_QEMU_KIT). The ramdisk and the command line are
  the image's own, read from the partition.
- The reboot reason: on the phone the kernel's msm restart driver stores it in
  the PMIC for aboot. That driver has nothing to talk to in QEMU and the
  kernel halts ("Reboot failed"), so this script reads the kernel's
  "Restarting system with command '...'" line instead and powers the next
  boot itself.

Reasons: none -> boot partition (the menu), "recovery" -> recovery partition,
"bootloader" -> stop (no fastboot here), power off -> stop. A kernel panic
reboots normally, as on the phone (msm_poweroff.download_mode=0).

Ctrl-C = hold POWER for 10 s (hard reset: next boot is the menu); Ctrl-C twice
within 2 s quits. SIGUSR1 also does a hard reset (for scripts).

Usage: qemu/fake-aboot.py [--headless] [--verbose] [--max-boots N]
"""
import argparse, json, os, re, shutil, signal, socket, struct, subprocess, sys, time
import config

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(P, "out", "qemu")
KIT = config.KIT()

a = argparse.ArgumentParser()
a.add_argument("--emmc", default=os.path.join(OUT, "emmc.img"))
a.add_argument("--kernel", default=os.path.join(KIT, "kobj/arch/arm64/boot/Image"))
a.add_argument("--dtb", default=os.path.join(KIT, "dtb/virt-fb-6-2048.dtb"))
a.add_argument("--qmp", default=os.path.join(OUT, "qmp.sock"))
a.add_argument("--headless", action="store_true", help="no window (screenshots with qemu/qmp.py)")
a.add_argument("--verbose", action="store_true", help="print the whole kernel log")
a.add_argument("--max-boots", type=int, default=0)
args = a.parse_args()
for f in (args.emmc, args.kernel, args.dtb):
    if not os.path.exists(f):
        sys.exit(f"missing {f}")


def say(msg):
    print(f"\033[1;33m[aboot]\033[0m {msg}", flush=True)


def gpt(path):
    """name -> (offset, size) from the primary GPT."""
    with open(path, "rb") as f:
        f.seek(512)
        h = f.read(92)
        assert h[:8] == b"EFI PART", "no GPT"
        lba, n, esz = struct.unpack_from("<QII", h, 72)
        f.seek(lba * 512)
        parts = {}
        for _ in range(n):
            e = f.read(esz)
            first, last = struct.unpack_from("<QQ", e, 32)
            name = e[56:128].decode("utf-16-le").rstrip("\0")
            if first:
                parts[name] = (first * 512, (last - first + 1) * 512)
        return parts


def load_bootimg(path, part):
    off, size = part
    with open(path, "rb") as f:
        f.seek(off)
        d = f.read(size)
    if d[:8] != b"ANDROID!":
        return None
    ks, _, rs = struct.unpack_from("<III", d, 8)
    page = struct.unpack_from("<I", d, 36)[0]
    name = d[48:64].rstrip(b"\0").decode(errors="replace")
    cmdline = d[64:576].rstrip(b"\0").decode(errors="replace")
    roff = page + (ks + page - 1) // page * page
    return {"name": name, "cmdline": cmdline, "ramdisk": d[roff:roff + rs]}


RESTART = re.compile(r"Restarting system(?: with command '([^']*)')?\.")
# Power off. Without a PMIC driver (QEMU) the kernel halts instead and says so.
HALT = re.compile(r"Power down\.|System halted\.")
state = {"hard_reset": False, "last_int": 0.0, "quit": False}


def on_int(sig, frame):
    now = time.time()
    if sig == signal.SIGINT and now - state["last_int"] < 2:
        state["quit"] = True
    state["last_int"] = now
    state["hard_reset"] = True


signal.signal(signal.SIGINT, on_int)
signal.signal(signal.SIGUSR1, on_int)

parts = gpt(args.emmc)
shutil.rmtree(os.path.join(OUT, "logs"), ignore_errors=True)    # boot numbers restart at 1
os.makedirs(os.path.join(OUT, "logs"))
reason = None
boot = 0
while True:
    boot += 1
    slot = "recovery" if reason == "recovery" else "boot"
    img = load_bootimg(args.emmc, parts[slot])
    say(f"{time.strftime('%H:%M:%S')} power on #{boot}: reason {reason or 'none'} -> {slot} partition")
    if img is None:
        say(f"no boot image in {slot}: on the phone aboot would stop in fastboot")
        break
    rd = os.path.join(OUT, "ramdisk.img")
    open(rd, "wb").write(img["ramdisk"])
    # the image's own command line + what the QEMU kernel needs (see the kit's README.txt)
    # ignore_loglevel: the menu's init lowers the console log level; its log is how this script follows it
    cmdline = img["cmdline"] + " earlyprintk=pl011,0x9000000 keep_bootcon mem=1024M ignore_loglevel"
    log = os.path.join(OUT, "logs", f"boot-{boot:02d}-{slot}.log")
    open(os.path.join(OUT, "current.json"), "w").write(json.dumps({"boot": boot, "slot": slot, "log": log}))
    if os.path.exists(args.qmp):
        os.unlink(args.qmp)
    q = subprocess.Popen([
        "qemu-system-aarch64", "-M", "virt,gic-version=2,virtualization=on", "-cpu", "cortex-a57",
        "-smp", "6", "-m", "2048", "-dtb", args.dtb, "-no-reboot",
        "-device", "ramfb", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",   # tablet: mouse = touch
        "-drive", f"file={args.emmc},if=none,format=raw,id=emmc", "-device", "virtio-blk-device,drive=emmc",
        "-display", "none" if args.headless else "gtk,zoom-to-fit=on",
        "-serial", f"file:{log}", "-monitor", "none", "-qmp", f"unix:{args.qmp},server=on,wait=off",
        "-kernel", args.kernel, "-initrd", rd, "-append", cmdline,
    ], stdin=subprocess.DEVNULL, start_new_session=True)

    # Follow the kernel log until the kernel asks for a restart or power-off.
    reason, outcome, pos, buf = None, None, 0, ""
    while outcome is None:
        time.sleep(0.2)
        if state["hard_reset"]:
            state["hard_reset"] = False
            outcome = "quit" if state["quit"] else "hard reset"
            break
        if q.poll() is not None:
            outcome = "qemu exited"
            break
        try:
            with open(log, "rb") as f:
                f.seek(pos)
                chunk = f.read()
        except FileNotFoundError:
            continue
        pos += len(chunk)
        buf += chunk.decode(errors="replace")
        *lines, buf = buf.split("\n")
        for line in lines:
            line = line.rstrip("\r")
            if args.verbose or "bootmenu:" in line or RESTART.search(line) or HALT.search(line) \
                    or "Kernel panic" in line:
                print(line, flush=True)
            m = RESTART.search(line)
            if m:
                reason, outcome = m.group(1) or None, "restart"
            elif HALT.search(line):
                outcome = "power off"
            elif "Kernel panic - not syncing" in line:
                outcome = "panic"
    time.sleep(0.3)     # let the last lines reach the log
    q.terminate()
    try:
        q.wait(5)
    except subprocess.TimeoutExpired:
        q.kill()
    say(f"{outcome}" + (f" (reason {reason})" if reason else ""))
    if outcome in ("quit", "power off", "qemu exited") or reason == "bootloader":
        if reason == "bootloader":
            say("fastboot mode: nothing to emulate here, stopping")
        break
    if outcome != "restart":
        reason = None
    if args.max_boots and boot >= args.max_boots:
        break
