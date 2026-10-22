"""Settings for the qemu/ scripts: environment variables, else local.env at the
repo root (KEY=value lines, as the shell scripts read it)."""
import os

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_file = {}
if os.path.exists(os.path.join(P, "local.env")):
    for line in open(os.path.join(P, "local.env")):
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            _file[k.strip()] = os.path.expandvars(os.path.expanduser(v.strip().strip('"')))


def get(key, default=None):
    v = os.environ.get(key) or _file.get(key) or default
    if v is None:
        raise SystemExit(f"set {key} in local.env (see local.env.example)")
    return v


LINEAGE = lambda: get("LINEAGE")
KIT = lambda: get("NEXUS5X_QEMU_KIT", os.path.expanduser("~/bullhead-qemu"))   # nexus5x-recovery-roadfighter's default
