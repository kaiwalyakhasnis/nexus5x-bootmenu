#!/usr/bin/env python3
"""docs/qemu.png for the README: the menu, then each image after the menu
switched to it, all on QEMU (qemu/fake-aboot.py, headless)."""
import json, os, re, signal, subprocess, sys, tempfile, time
from PIL import Image, ImageDraw

Q = os.path.dirname(os.path.abspath(__file__))
P = os.path.dirname(Q)
OUT = os.path.join(P, "out", "qemu")
SHOTS = [("Boot menu", None, 0), ("Hike Buddy", 2, 16), ("Local LLM", 3, 40), ("Embedded Compose", 4, 40)]


def current():
    try:
        return json.load(open(os.path.join(OUT, "current.json")))
    except (FileNotFoundError, json.JSONDecodeError):
        return {"boot": 0}


def wait(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return
        time.sleep(0.3)
    raise SystemExit("timeout")


def wait_boot(slot, text=None):
    b0 = current()["boot"]
    wait(lambda: current()["boot"] > b0 and current().get("slot") == slot, 60)
    if text:
        wait(lambda: os.path.exists(current()["log"]) and text in open(current()["log"], errors="replace").read(), 60)


def qmp(*a):
    subprocess.run([sys.executable, os.path.join(Q, "qmp.py"), *a], check=True, stdout=subprocess.DEVNULL)


subprocess.run([sys.executable, os.path.join(Q, "make-emmc.py")], check=True, stdout=subprocess.DEVNULL)
open(os.path.join(OUT, "current.json"), "w").write('{"boot": 0}')
aboot = subprocess.Popen([sys.executable, os.path.join(Q, "fake-aboot.py"), "--headless"], stdout=subprocess.DEVNULL)
tmp = tempfile.mkdtemp()
try:
    for name, entry, delay in SHOTS:
        if entry is None:
            wait_boot("boot", "menu ready")
            time.sleep(0.5)
        else:
            aboot.send_signal(signal.SIGUSR1)     # hold POWER: back to the menu
            wait_boot("boot", "menu ready")
            log = open(current()["log"], errors="replace").read()
            sel = max(int(re.search(r"menu ready: \d+ entries, default (-?\d+)", log).group(1)), 0)
            # entries 0.. are the images in file order; the list wraps, so only go down
            n = int(re.search(r"menu ready: (\d+) entries", log).group(1))
            qmp("key", *(["down"] * ((entry - 1 - sel) % n)), "ret")
            wait(lambda: "Restarting system" in open(current()["log"], errors="replace").read(), 60)
            assert f"selected {name}" in open(current()["log"], errors="replace").read(), name
            wait_boot("recovery")
            time.sleep(delay)
        qmp("shot", os.path.join(tmp, f"{len(os.listdir(tmp))}.png"))
finally:
    aboot.send_signal(signal.SIGINT)
    time.sleep(0.3)
    aboot.send_signal(signal.SIGINT)
    aboot.wait(30)

w, h, pad, label = 300, 533, 12, 30
sheet = Image.new("RGB", (len(SHOTS) * (w + pad) + pad, h + label + 2 * pad), (24, 26, 30))
d = ImageDraw.Draw(sheet)
for i, (name, _, _) in enumerate(SHOTS):
    x = pad + i * (w + pad)
    sheet.paste(Image.open(os.path.join(tmp, f"{i}.png")).resize((w, h), Image.LANCZOS), (x, pad + label))
    d.text((x, pad + 6), name, fill=(230, 230, 230))
os.makedirs(os.path.join(P, "docs"), exist_ok=True)
sheet.save(os.path.join(P, "docs", "qemu.png"), optimize=True)
print("docs/qemu.png")
