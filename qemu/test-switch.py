#!/usr/bin/env python3
"""End-to-end test on QEMU: starts qemu/fake-aboot.py headless and drives the
menu with keys, as a person would with the volume keys and power.

For each image: hold POWER (hard reset) -> menu -> pick it -> it is written to
recovery -> reboot -> aboot boots the recovery partition -> screenshot.
Then: re-picking the current image must not write, "Bootloader" and
"Power off" must reboot/power off with the right reason, and every partition
except recovery must be byte-identical to what qemu/make-emmc.py wrote.

Run qemu/make-emmc.py --test-images first. Screenshots + logs: out/qemu/test/
"""
import hashlib, json, os, re, shutil, signal, subprocess, sys, time

Q = os.path.dirname(os.path.abspath(__file__))
P = os.path.dirname(Q)
OUT = os.path.join(P, "out", "qemu")
T = os.path.join(OUT, "test")
shutil.rmtree(T, ignore_errors=True)
os.makedirs(T)
failures = []


def check(ok, what):
    print(("  PASS " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def current():
    try:
        return json.load(open(os.path.join(OUT, "current.json")))
    except (FileNotFoundError, json.JSONDecodeError):
        return {"boot": 0}


def log_text(boot):
    c = current()
    if c["boot"] != boot:
        return ""
    try:
        return open(c["log"], errors="replace").read()
    except FileNotFoundError:
        return ""


def wait(pred, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        r = pred()
        if r:
            return r
        time.sleep(0.3)
    raise SystemExit(f"timeout waiting for {what}")


def wait_menu():
    """Waits for the next boot to show the menu; returns (boot, entries, default)."""
    b0 = current()["boot"]
    boot = wait(lambda: current()["boot"] > b0 and current().get("slot") == "boot" and current()["boot"],
                60, "next power on of the boot partition")
    text = wait(lambda: "menu ready" in log_text(boot) and log_text(boot), 60, "menu ready")
    images = re.findall(r"bootmenu: image (\S+)\.img: (.*?), (\d+) bytes", text)
    entries = [re.sub(r"^\d+[-_]", "", n).replace("_", " ") for n, _, _ in images]
    if not any("in the recovery slot" in s for _, s, _ in images):
        entries.append("Recovery slot as is")
    entries += ["Bootloader", "Power off"]
    print(f"    {time.strftime('%H:%M:%S')} menu ready on boot {boot}", flush=True)
    ready = re.search(r"menu ready: (\d+) entries, default (-?\d+)", text)
    assert int(ready.group(1)) == len(entries), (entries, ready.group(0))
    return boot, entries, max(int(ready.group(2)), 0), text


def keys(*k):
    print(f"    {time.strftime('%H:%M:%S')} keys {' '.join(k)}", flush=True)
    subprocess.run([sys.executable, os.path.join(Q, "qmp.py"), "key", *k], check=True)


def shot(name):
    subprocess.run([sys.executable, os.path.join(Q, "qmp.py"), "shot", os.path.join(T, name)],
                   check=True, stdout=subprocess.DEVNULL)


def hard_reset():
    aboot.send_signal(signal.SIGUSR1)


def select(label):
    """Hard reset, then pick `label` in the menu. Returns the menu boot's log."""
    hard_reset()
    boot, entries, sel, _ = wait_menu()
    keys(*path(entries, sel, label), "ret")
    return boot


def path(entries, sel, label):
    """The shorter way round the list from `sel` to `label`."""
    d = (entries.index(label) - sel) % len(entries)
    return ["down"] * d if d <= len(entries) - d else ["up"] * (len(entries) - d)


def menu_log_after(boot, pattern, timeout=60):
    return wait(lambda: re.search(pattern, log_text(boot), re.S) and log_text(boot), timeout, pattern)


open(os.path.join(OUT, "current.json"), "w").write('{"boot": 0}')
aboot = subprocess.Popen([sys.executable, os.path.join(Q, "fake-aboot.py"), "--headless"],
                         stdout=open(os.path.join(T, "aboot.out"), "w"), stderr=subprocess.STDOUT)
try:
    # Boot 1 is the menu from power-on; use it as is.
    boot, entries, sel, text = wait_menu()
    print("menu:", entries)
    shot("01-menu.png")
    check("8-Broken.img: not a boot image" in text and "9-Truncated.img: truncated" in text,
          "bad images are listed with their error")
    hard_reset()    # so every case below starts the same way

    for i, label in enumerate(["Local LLM", "Hike Buddy", "LineageOS", "Embedded Compose"], 2):
        print(f"{label}:")
        boot = select(label)
        text = menu_log_after(boot, r"Restarting system with command 'recovery'")
        check(f"selected {label}" in text and "written and verified" in text, f"{label} written and verified")
        rboot = wait(lambda: current()["boot"] > boot and current()["slot"] == "recovery" and current()["boot"],
                     30, "recovery boot")
        time.sleep(30)                                 # let the image's own userspace start
        shot(f"{i:02d}-{label.replace(' ', '_')}.png")
        shutil.copy(current()["log"], os.path.join(T, f"{i:02d}-{label.replace(' ', '_')}.log"))
        check(rboot == current()["boot"], f"{label} still running from recovery after 30 s")

    print("current image again:")
    boot = select("Embedded Compose")
    text = menu_log_after(boot, r"Restarting system with command 'recovery'")
    check("in the recovery slot" in text and "writing" not in text, "the current image boots without a write")

    print("broken image:")
    hard_reset()
    boot, entries, sel, _ = wait_menu()
    keys(*path(entries, sel, "Broken"), "ret")
    time.sleep(1.5)
    shot("07-broken-selected.png")
    check("writing" not in log_text(boot) and "Restarting" not in log_text(boot),
          "a broken image is refused, the menu stays")

    print("bootloader:")
    keys(*path(entries, entries.index("Broken"), "Bootloader"), "ret")
    menu_log_after(boot, r"Restarting system with command 'bootloader'")
    aboot.wait(30)
    check("fastboot mode" in open(os.path.join(T, "aboot.out")).read(), "Bootloader reboots with reason bootloader")
finally:
    if aboot.poll() is None:
        aboot.send_signal(signal.SIGINT)
        time.sleep(0.2)
        aboot.send_signal(signal.SIGINT)
        aboot.wait(30)

print("power off:")
open(os.path.join(OUT, "current.json"), "w").write('{"boot": 0}')
aboot = subprocess.Popen([sys.executable, os.path.join(Q, "fake-aboot.py"), "--headless"],
                         stdout=open(os.path.join(T, "aboot-poweroff.out"), "w"), stderr=subprocess.STDOUT)
boot, entries, sel, _ = wait_menu()
keys(*path(entries, sel, "Power off"), "ret")
aboot.wait(60)
check("power off" in open(os.path.join(T, "aboot-poweroff.out")).read(), "Power off powers off")

print("partitions:")
parts = json.load(open(os.path.join(OUT, "emmc-parts.json")))
with open(os.path.join(OUT, "emmc.img"), "rb") as f:
    for name, p in parts.items():
        f.seek(p["offset"])
        data = f.read(p["size"])
        if name == "recovery":
            img = open(os.path.join(P, "out", "images", "4-Embedded_Compose.img"), "rb").read()
            check(data[:len(img)] == img, "recovery holds the last chosen image (Embedded Compose)")
        else:
            check(hashlib.sha256(data).hexdigest() == p["sha256"], f"{name} unchanged")

print("\nFAILED: " + "; ".join(failures) if failures else "\nall passed")
sys.exit(1 if failures else 0)
