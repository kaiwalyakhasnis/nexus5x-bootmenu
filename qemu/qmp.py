#!/usr/bin/env python3
"""Talks to the QEMU that qemu/fake-aboot.py started.

  qemu/qmp.py shot [file.png]      the 1080x1920 framebuffer (default out/qemu/shot.png)
  qemu/qmp.py key down down ret    keys, QEMU names: up down ret spc esc ...
"""
import json, os, socket, sys, tempfile, time

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOCK = os.path.join(P, "out", "qemu", "qmp.sock")
FB_BASE, W, H = 0x80000000, 1080, 1920      # the QEMU kit's gen-dtb.sh


def qmp(cmd, **arguments):
    s = socket.socket(socket.AF_UNIX)
    s.connect(SOCK)
    f = s.makefile("rw")
    f.readline()
    for c in ({"execute": "qmp_capabilities"}, {"execute": cmd, "arguments": arguments}):
        f.write(json.dumps(c) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                break
        if "error" in r:
            raise RuntimeError(r["error"])
    s.close()


def shot(path):
    from PIL import Image
    with tempfile.NamedTemporaryFile(suffix=".raw") as raw:
        qmp("pmemsave", val=FB_BASE, size=W * H * 4, filename=raw.name)
        Image.frombytes("RGB", (W, H), raw.read(), "raw", "RGBX").save(path)
    print(path)


if len(sys.argv) < 2:
    sys.exit(__doc__)
if sys.argv[1] == "shot":
    shot(sys.argv[2] if len(sys.argv) > 2 else os.path.join(P, "out", "qemu", "shot.png"))
elif sys.argv[1] == "key":
    for k in sys.argv[2:]:
        qmp("send-key", keys=[{"type": "qcode", "data": k}])
        time.sleep(0.3)
else:
    sys.exit(__doc__)
