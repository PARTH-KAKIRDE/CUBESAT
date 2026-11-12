#!/usr/bin/env python3
"""
Ground station tool. Talks to a LilyGO LoRa board running lilygo_bridge/ over USB serial.

  python gs_tool.py --port COM5 listen                  print every packet received
  python gs_tool.py --port COM5 cmd obc ping
  python gs_tool.py --port COM5 cmd obc hk
  python gs_tool.py --port COM5 cmd obc mode safe|nominal
  python gs_tool.py --port COM5 cmd obc image
  python gs_tool.py --port COM5 cmd obc tlm adcs 0       slave telemetry page via the OBC
  python gs_tool.py --port COM5 cmd adcs mode detumble   any ICD command, forwarded by the OBC
  python gs_tool.py --port COM5 cmd adcs raw 0x20 e0930400
  python gs_tool.py --port COM5 cmd eps get
  python gs_tool.py --port COM5 cmd eps pyro_arm | pyro_fire 500
  python gs_tool.py --port COM5 cmd ttc burn_arm | burn_fire 3000
  python gs_tool.py keygen                               create the ground-command key (once)

  --sim instead of --port runs the whole satellite (OBC + simulated ADCS/TT&C) in
  this process, so every command can be tried without hardware.

Commands are retransmitted (same sequence number) until an ACK arrives; the OBC
answers a repeat from its cache instead of executing it twice.

Every command is signed with the key in --key-file (default ground/.ground_key)
and carries a counter that only goes up (ground/.gs_ctr). The OBC needs the
same key file (obc.toml [ground] key_file) and ignores anything unsigned,
signed with another key, or older than the last command it accepted.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "obc"))

from cubesat_obc import ground  # noqa: E402
from cubesat_obc import icd_defs as I  # noqa: E402
from cubesat_obc.frames import status_name  # noqa: E402

HERE = Path(__file__).resolve().parent
SEQ_FILE = HERE / ".gs_seq"          # kept next to this script (git-ignored)
CTR_FILE = HERE / ".gs_ctr"          # auth counter (git-ignored)
DEFAULT_KEY_FILE = HERE / ".ground_key"  # secret (git-ignored)
MODES = {"safe": I.ICD_MODE_SAFE, "detumble": I.ICD_MODE_DETUMBLE, "nominal": I.ICD_MODE_NOMINAL}
TARGETS = {"obc": I.GL_TGT_OBC, "adcs": I.GL_TGT_ADCS, "ttc": I.GL_TGT_TTC, "eps": I.GL_TGT_EPS}
GL_STATUS = {I.GL_ST_BAD_PACKET: "BAD_PACKET", I.GL_ST_COMM_FAULT: "COMM_FAULT", I.GL_ST_REFUSED: "REFUSED"}


def next_seq() -> int:
    try:
        s = (int(SEQ_FILE.read_text()) + 1) & 0xFF
    except (OSError, ValueError):
        s = 0
    try:
        SEQ_FILE.write_text(str(s))
    except OSError:
        pass
    return s


def next_counter() -> int:
    """Strictly increasing. Unix time as the floor means a lost .gs_ctr file
    cannot drop the counter below what the OBC has already accepted (as long
    as this PC's clock is right); the file covers several commands per second."""
    try:
        saved = int(CTR_FILE.read_text())
    except (OSError, ValueError):
        saved = 0
    c = max(saved + 1, int(time.time()))
    if c >= 2**32:
        raise SystemExit("auth counter exhausted: generate a new key and reset the OBC counter")
    CTR_FILE.write_text(str(c))  # must persist before sending, so fail loudly
    return c


def do_keygen(path: Path, force: bool) -> int:
    if path.exists() and not force:
        print(f"{path} already exists (use --force to replace it; the OBC then needs the new file too)")
        return 1
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        f.write(secrets.token_hex(32) + "\n")
    print(f"wrote {path}\nCopy it to the Pi (obc.toml [ground] key_file, chmod 600). Never commit it.")
    return 0


# ------------------------------------------------------------------ links

class SerialRadio:
    """LilyGO bridge: 'TX <hex>' out, 'RX <rssi> <snr> <hex>' in."""

    def __init__(self, port: str, baud: int = 115200):
        import serial  # pip install pyserial

        self.s = serial.Serial(port, baud, timeout=0.1)
        time.sleep(2.0)  # ESP32 resets when the port opens
        self.s.reset_input_buffer()

    def send(self, pkt: bytes) -> None:
        self.s.write(f"TX {pkt.hex()}\n".encode())

    def receive(self, timeout: float):
        end = time.monotonic() + timeout
        out = []
        while time.monotonic() < end:
            line = self.s.readline().decode(errors="replace").strip()
            if line.startswith("RX "):
                parts = line.split()
                try:
                    out.append((bytes.fromhex(parts[3]), int(parts[1]), float(parts[2])))
                except (IndexError, ValueError):
                    pass
                end = min(end, time.monotonic() + 0.5)  # collect TLM parts that follow
            elif line:
                print(f"  [bridge] {line}")
        return out


class SimRadio:
    """The whole satellite in-process (OBC app + firmware simulation)."""

    def __init__(self, key_file: Path):
        import tempfile

        from cubesat_obc import config
        from cubesat_obc.factory import build_sim

        cfg = config.load(None)
        cfg["obc"]["data_dir"] = tempfile.mkdtemp(prefix="gs_sim_")
        cfg["pyro"]["enabled"] = True
        cfg["ground"]["key_file"] = str(key_file)
        self.app, self.sim = build_sim(cfg)
        self.app.start()
        self._tick(3)
        self.sim.downlink()

    def _tick(self, n):
        for _ in range(n):
            self.app.tick()
            self.app.clock.sleep(1.0)

    def send(self, pkt: bytes) -> None:
        self.sim.uplink(pkt, -97)

    def receive(self, timeout: float):
        out = []
        for _ in range(max(1, int(timeout))):
            self._tick(1)
            out += [(p, -97, 7.5) for p in self.sim.downlink()]
        return out


# ------------------------------------------------------------------ commands

def build_command(args) -> tuple:
    tgt, verb, rest = args.target, args.verb, args.args
    t = TARGETS[tgt]
    if tgt == "obc":
        if verb == "ping":
            return t, I.GL_OBC_PING, b""
        if verb == "hk":
            return t, I.GL_OBC_GET_HK, b""
        if verb == "mode":
            return t, I.GL_OBC_SET_MODE, bytes([MODES[rest[0]]])
        if verb == "image":
            return t, I.GL_OBC_CAPTURE_IMAGE, b""
        if verb == "tlm":
            return t, I.GL_OBC_GET_SLAVE_TLM, bytes([TARGETS[rest[0]], int(rest[1])])
        if verb == "reboot":
            return t, I.GL_OBC_REBOOT, struct.pack("<I", I.GL_REBOOT_KEY)
    if tgt == "eps":
        if verb == "get":
            return t, I.GL_EPS_GET, b""
        if verb == "pyro_arm":
            return t, I.GL_EPS_PYRO_ARM, struct.pack("<I", I.GL_PYRO_KEY)
        if verb == "pyro_fire":
            return t, I.GL_EPS_PYRO_FIRE, struct.pack("<H", int(rest[0]))
    if tgt in ("adcs", "ttc"):
        if verb == "ping":
            return t, I.ICD_CMD_PING, b""
        if verb == "status":
            return t, I.ICD_CMD_GET_STATUS, b""
        if verb == "version":
            return t, I.ICD_CMD_GET_VERSION, b""
        if verb == "mode":
            return t, I.ICD_CMD_SET_MODE, bytes([MODES[rest[0]]])
        if verb == "tlm":
            return t, I.ICD_CMD_GET_TELEMETRY, bytes([int(rest[0])])
        if tgt == "ttc" and verb == "burn_arm":
            return t, I.TTC_CMD_BURN_ARM, struct.pack("<I", I.TTC_BURN_ARM_KEY)
        if tgt == "ttc" and verb == "burn_fire":
            return t, I.TTC_CMD_BURN_FIRE, struct.pack("<H", int(rest[0]))
        if verb == "raw":
            return t, int(rest[0], 0), bytes.fromhex(rest[1]) if len(rest) > 1 else b""
    raise SystemExit(f"unknown command: {tgt} {verb} {' '.join(rest)}")


def describe(target: int, cmd: int, data: bytes) -> dict:
    u = struct.unpack_from
    if target == I.GL_TGT_OBC and cmd == I.GL_OBC_GET_HK and len(data) == I.GL_HK_LEN:
        return {"uptime_s": u("<I", data, I.GL_HK_OFS_UPTIME_S)[0], "obc_mode": data[I.GL_HK_OFS_OBC_MODE],
                "comm_flags": data[I.GL_HK_OFS_COMM_FLAGS], "vbatt_mV": u("<H", data, I.GL_HK_OFS_VBATT)[0],
                "ibus_mA": u("<h", data, I.GL_HK_OFS_IBUS)[0], "pbus_mW": u("<H", data, I.GL_HK_OFS_PBUS)[0],
                "cpu_temp_C": u("<b", data, I.GL_HK_OFS_CPU_TEMP)[0], "disk_free_pct": data[I.GL_HK_OFS_DISK_FREE],
                "lux": u("<H", data, I.GL_HK_OFS_LUX)[0]}
    if target == I.GL_TGT_EPS and cmd == I.GL_EPS_GET and len(data) == I.GL_EPS_LEN:
        return {"vbatt_mV": u("<H", data, I.GL_EPS_OFS_VBATT)[0], "current_mA": u("<h", data, I.GL_EPS_OFS_IBUS)[0],
                "power_mW": u("<H", data, I.GL_EPS_OFS_PBUS)[0], "low_battery": bool(data[I.GL_EPS_OFS_LOW_BATT]),
                "pyro_fires": data[I.GL_EPS_OFS_PYRO_FIRES]}
    if cmd == I.ICD_CMD_GET_STATUS and len(data) == I.ICD_GET_STATUS_RESP_LEN:
        return {"mode": data[I.ICD_STATUS_OFS_MODE], "flags": data[I.ICD_STATUS_OFS_FLAGS],
                "faults": u("<H", data, I.ICD_STATUS_OFS_FAULTS)[0],
                "uptime_s": u("<I", data, I.ICD_STATUS_OFS_UPTIME_S)[0]}
    return {"hex": data.hex()}


def do_cmd(radio, args, key: bytes) -> int:
    target, cmd, payload = build_command(args)
    seq = next_seq()
    try:
        pkt = ground.build_cmd(seq, target, cmd, payload, key=key, counter=next_counter())
    except ground.PacketError as e:
        raise SystemExit(f"cannot build command: {e}")
    for attempt in range(1, args.retries + 1):
        print(f"-> seq {seq} target {args.target} cmd 0x{cmd:02X} args {payload.hex() or '-'} (try {attempt})")
        radio.send(pkt)
        got = []
        for raw, rssi, snr in radio.receive(args.timeout):
            try:
                d = ground.parse_downlink(raw)
            except ground.PacketError:
                continue
            if d["type"] in ("ACK", "TLM") and d["seq"] == seq:
                got.append(d)
            elif d["type"].startswith("BEACON"):
                print(f"   (beacon {d['type']} rssi {rssi})")
        ack = next((d for d in got if d["type"] == "ACK"), None)
        if ack:
            st = ack["status"]
            data = ground.reassemble(got) or bytes.fromhex(ack["data"])
            name = GL_STATUS.get(st) or status_name(st)
            print(f"<- {name}")
            if data:
                print(json.dumps(describe(target, cmd, data), indent=2))
            return 0 if st == I.ICD_ST_OK else 1
    print("no ACK")
    return 2


def do_listen(radio) -> int:
    print("listening (Ctrl+C to stop)")
    try:
        while True:
            for raw, rssi, snr in radio.receive(5.0):
                try:
                    d = ground.parse_downlink(raw)
                except ground.PacketError:
                    d = {"type": "INVALID", "hex": raw.hex()}
                d.update({"rssi": rssi, "snr": snr, "t": time.strftime("%H:%M:%S")})
                print(json.dumps(d))
    except KeyboardInterrupt:
        return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--port", help="serial port of the LilyGO bridge (COM5, /dev/ttyUSB0)")
    g.add_argument("--sim", action="store_true", help="simulated satellite in this process")
    ap.add_argument("--timeout", type=float, default=8.0, help="seconds to wait for an ACK")
    ap.add_argument("--retries", type=int, default=3)
    ap.add_argument("--key-file", type=Path, default=DEFAULT_KEY_FILE, help="ground-command key (secret)")
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("listen")
    kg = sub.add_parser("keygen", help="create a new random key file")
    kg.add_argument("--force", action="store_true", help="replace an existing key file")
    c = sub.add_parser("cmd")
    c.add_argument("target", choices=list(TARGETS))
    c.add_argument("verb")
    c.add_argument("args", nargs="*")
    args = ap.parse_args()
    if args.action == "keygen":
        return do_keygen(args.key_file, args.force)
    if not (args.sim or args.port):
        ap.error("one of --port or --sim is required")
    key = None
    if args.action == "cmd":
        if args.sim and not args.key_file.exists():
            import tempfile  # simulation only: a throw-away key shared with the simulated OBC
            args.key_file = Path(tempfile.mkdtemp(prefix="gs_key_")) / "key"
            args.key_file.write_text(secrets.token_hex(32))
        try:
            key = ground.load_key(args.key_file)
        except (OSError, ground.PacketError) as e:
            raise SystemExit(f"no usable key ({e}). Run: python gs_tool.py keygen")
    radio = SimRadio(args.key_file) if args.sim else SerialRadio(args.port)
    return do_listen(radio) if args.action == "listen" else do_cmd(radio, args, key)


if __name__ == "__main__":
    sys.exit(main())
