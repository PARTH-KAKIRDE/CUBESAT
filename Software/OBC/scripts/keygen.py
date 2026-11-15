#!/usr/bin/env python3
"""
Make the two ground-link keys (ICD.md section 11). Run once, on the ground PC.

  python3 OBC/scripts/keygen.py                 -> ~/.cubesat/ground_key and ~/.cubesat/ttc_key
  python3 OBC/scripts/keygen.py --dir PATH      somewhere else (never inside this repo)
  python3 OBC/scripts/keygen.py --force         replace existing keys (old ones stop working)

  ground_key  OBC commands (GL_T_CMD). Copy it to the Pi: /home/pi/.cubesat/ground_key
  ttc_key     TT&C emergency commands (GL_T_ECMD). Never goes to the Pi: it is built into
              the TT&C firmware:  TTC_KEY_FILE=~/.cubesat/ttc_key python3 firmware/build.py arm ttc

Two different keys on purpose: whoever holds the Pi (or its SD card) cannot sign
emergency commands, and a leaked OBC key cannot reset the OBC through TT&C.
Each key is 32 random bytes (secrets module) as hex text, file mode 0600. The
repo is public: this script refuses to write inside it.
"""
import argparse
import os
import secrets
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]    # Cubesat/
KEY_BYTES = 32
NAMES = ("ground_key", "ttc_key")


def inside_repo(p: Path) -> bool:
    try:
        p.resolve().relative_to(REPO)
        return True
    except ValueError:
        return False


def write_key(path: Path) -> None:
    """Create with mode 0600 from the start (no window where it is world-readable)."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(secrets.token_hex(KEY_BYTES) + "\n")
        f.flush()
        os.fsync(f.fileno())
    os.chmod(path, 0o600)  # if the file already existed with looser bits


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=str(Path.home() / ".cubesat"))
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args(argv)
    d = Path(a.dir).expanduser()
    if inside_repo(d):
        print(f"refusing: {d} is inside the repo ({REPO}), which is public")
        return 1
    d.mkdir(parents=True, exist_ok=True)
    os.chmod(d, 0o700)
    paths = [d / n for n in NAMES]
    existing = [p for p in paths if p.exists()]
    if existing and not a.force:
        print("already exist (use --force to replace; the satellite then needs the new keys):")
        for p in existing:
            print("  ", p)
        return 1
    for p in paths:
        write_key(p)
        print("wrote", p)
    print(f"""
next:
  Pi (OBC key):      scp {paths[0]} pi@<pi>:/home/pi/.cubesat/ground_key   then on the Pi: chmod 600 it
  TT&C (ECMD key):   TTC_KEY_FILE={paths[1]} python3 firmware/build.py arm ttc   then flash ttc
                     (build.py also finds {Path.home() / '.cubesat' / 'ttc_key'} by itself)
  Ground station:    keeps both files here; its counters go in the same folder.""")
    return 0


if __name__ == "__main__":
    sys.exit(main())
