"""Ground-link packet codec (ICD/ground_link.h). Used by the OBC and by the ground tool.

CMD packets carry a counter and a MAC (GL_VERSION 2):
  SAT_ID | TYPE | SEQ | TARGET | CMD | LEN | ARGS | COUNTER u32 | MAC(4) | CRC16
MAC = HMAC-SHA256(key, SAT_ID .. COUNTER)[:4].

ECMD packets (GL_VERSION 4) are the same with TYPE = GL_T_ECMD and the TT&C
emergency key. TT&C checks and executes them itself (no OBC) and answers with
an ACK whose TARGET has GL_TGT_EMERGENCY set.
"""
from __future__ import annotations

import hashlib
import hmac
import logging
import os
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import icd_defs as I
from icd_link import crc16

MIN_KEY_BYTES = 16


class PacketError(Exception):
    pass


def _seal(body: bytes, max_len: int = I.GL_MAX_LEN) -> bytes:
    c = crc16(body)
    pkt = body + bytes([c & 0xFF, c >> 8])
    if len(pkt) > max_len:
        raise PacketError(f"packet too long ({len(pkt)})")
    return pkt


def check(pkt: bytes, max_len: int = I.GL_MAX_LEN) -> bool:
    if not I.GL_HDR_LEN + I.GL_CRC_LEN <= len(pkt) <= max_len:
        return False
    if pkt[I.GL_OFS_SAT] != I.GL_SAT_ID:
        return False
    c = crc16(pkt[:-2])
    return pkt[-2] == (c & 0xFF) and pkt[-1] == (c >> 8)


@dataclass
class Command:
    seq: int
    target: int
    cmd: int
    args: bytes
    counter: int = 0


def load_key(path: str | Path) -> bytes:
    """Key file: hex text (as written by OBC/scripts/keygen.py). A file others
    can read still works (a missing uplink is worse) but is logged loudly."""
    try:
        key = bytes.fromhex(Path(path).read_text().strip())
    except ValueError as e:
        raise PacketError(f"key file {path} is not hex") from e
    if len(key) < MIN_KEY_BYTES:
        raise PacketError(f"key file {path}: key shorter than {MIN_KEY_BYTES} bytes")
    if os.name == "posix" and Path(path).stat().st_mode & 0o077:
        logging.getLogger(__name__).warning("key file %s can be read by other users: chmod 600 it", path)
    return key


class CounterStore:
    """Ground-side command counter for one key (GL_VERSION 2 rule: every new
    command needs a higher counter than the last one the satellite accepted).

    next() = max(last + 1, Unix time), and the value is on disk (written,
    fsynced, renamed) BEFORE it is returned, so a crash can never make the
    ground reuse a counter. Unix time means a lost counter file does not lock
    the ground out, and it stays above the TT&C build-time floor
    (ttc_ecmd_ctr_floor) - provided the PC clock is right. Keep one file per
    key: the OBC and TT&C counters are independent."""

    def __init__(self, path: str | Path, clock=time.time):
        self.path, self.clock = Path(path), clock

    def last(self) -> int:
        try:
            return int(self.path.read_text().strip() or 0)
        except FileNotFoundError:
            return 0
        except ValueError as e:
            raise PacketError(f"counter file {self.path} is damaged: fix or delete it") from e

    def next(self) -> int:
        n = max(self.last() + 1, int(self.clock()))
        if n >= 2**32:
            raise PacketError("counter exhausted (u32)")
        tmp = self.path.with_name(self.path.name + ".tmp")
        with open(tmp, "w") as f:
            f.write(f"{n}\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)
        return n


def _mac(key: bytes, data: bytes) -> bytes:
    return hmac.new(key, data, hashlib.sha256).digest()[: I.GL_AUTH_MAC_LEN]


def build_cmd(seq: int, target: int, cmd: int, args: bytes = b"", *, key: bytes, counter: int,
              ptype: int = I.GL_T_CMD) -> bytes:
    if ptype not in (I.GL_T_CMD, I.GL_T_ECMD):
        raise PacketError("not a command type")
    if len(args) > I.GL_CMD_MAX_ARGS:
        raise PacketError(f"too many args ({len(args)} > {I.GL_CMD_MAX_ARGS})")
    if not 0 < counter < 2**32:
        raise PacketError("counter out of range")
    body = bytes([I.GL_SAT_ID, ptype, seq & 0xFF, target, cmd, len(args)]) + args + struct.pack("<I", counter)
    return _seal(body + _mac(key, body))


def build_ecmd(seq: int, target: int, cmd: int, args: bytes = b"", *, key: bytes, counter: int) -> bytes:
    """Emergency command for TT&C (GL_T_ECMD), signed with the TT&C emergency key."""
    return build_cmd(seq, target, cmd, args, key=key, counter=counter, ptype=I.GL_T_ECMD)


def parse_events(data: bytes) -> list:
    """GL_E_GET_EVENTS answer: the last events, newest first (empty slots left out)."""
    if len(data) != I.GL_EVL_COUNT * I.GL_EVL_ENTRY_LEN:
        raise PacketError("bad E_GET_EVENTS length")
    out = []
    for i in range(I.GL_EVL_COUNT):
        no, tgt, cmd, res = struct.unpack_from("<HBBB", data, i * I.GL_EVL_ENTRY_LEN)
        if no:
            out.append({"no": no, "target": tgt, "cmd": cmd, "result": res})
    return out


def parse_eping(data: bytes) -> dict:
    """GL_E_PING answer (GL_EP_* layout)."""
    if len(data) != I.GL_EP_LEN:
        raise PacketError("bad E_PING length")
    return {"uptime_s": struct.unpack_from("<I", data, I.GL_EP_OFS_UPTIME_S)[0],
            "mode": data[I.GL_EP_OFS_MODE],
            "faults": struct.unpack_from("<H", data, I.GL_EP_OFS_FAULTS)[0],
            "obc_silent_s": struct.unpack_from("<H", data, I.GL_EP_OFS_OBC_SILENT_S)[0],
            "deploy_flags": data[I.GL_EP_OFS_DEPLOY_FLAGS],
            "auto_attempts": data[I.GL_EP_OFS_AUTO_ATTEMPTS],
            "wdt_min": struct.unpack_from("<H", data, I.GL_EP_OFS_WDT_MIN)[0],
            "wdt_actions": data[I.GL_EP_OFS_WDT_ACTIONS],
            "silence_min": data[I.GL_EP_OFS_SILENCE_MIN]}


def parse_cmd(pkt: bytes, key: bytes) -> Command:
    """Check framing and the MAC. The caller checks the counter (replay)."""
    if not check(pkt):
        raise PacketError("bad packet (id/crc/length)")
    if pkt[I.GL_OFS_TYPE] != I.GL_T_CMD:
        raise PacketError("not a command")
    n = pkt[I.GL_CMD_OFS_LEN]
    trailer = I.GL_AUTH_CTR_LEN + I.GL_AUTH_MAC_LEN + I.GL_CRC_LEN
    if n > I.GL_CMD_MAX_ARGS or I.GL_CMD_OFS_ARGS + n + trailer != len(pkt):
        raise PacketError("bad args length (or an old unauthenticated packet)")
    mac_at = len(pkt) - I.GL_CRC_LEN - I.GL_AUTH_MAC_LEN
    if not hmac.compare_digest(_mac(key, pkt[:mac_at]), pkt[mac_at:mac_at + I.GL_AUTH_MAC_LEN]):
        raise PacketError("bad MAC (wrong key or forged packet)")
    a = I.GL_CMD_OFS_ARGS
    return Command(pkt[I.GL_OFS_SEQ], pkt[I.GL_CMD_OFS_TARGET], pkt[I.GL_CMD_OFS_CMD], bytes(pkt[a:a + n]),
                   struct.unpack_from("<I", pkt, a + n)[0])


def build_ack(seq: int, target: int, cmd: int, status: int, data: bytes = b"") -> List[bytes]:
    """ACK with the first 15 bytes of data, then TLM packets for the rest."""
    first = data[: I.GL_ACK_MAX_DATA]
    out = [_seal(bytes([I.GL_SAT_ID, I.GL_T_ACK, seq & 0xFF, target, cmd, status, len(data)]) + first)]
    off = len(first)
    while off < len(data):
        part = data[off: off + I.GL_TLM_MAX_DATA]
        out.append(_seal(bytes([I.GL_SAT_ID, I.GL_T_TLM, seq & 0xFF, target, cmd, off]) + part))
        off += len(part)
    return out


@dataclass
class Ack:
    seq: int
    target: int
    cmd: int
    status: int
    total_len: int
    data: bytes


def parse_downlink(pkt: bytes) -> dict:
    """Decode any satellite->ground packet into a dict (for the ground tool).
    Only FILE packets may be longer than GL_MAX_LEN (GL_VERSION 4)."""
    long_file = len(pkt) > I.GL_OFS_TYPE and pkt[I.GL_OFS_TYPE] == I.GL_T_FILE
    if not check(pkt, I.GL_DOWN_MAX_LEN if long_file else I.GL_MAX_LEN):
        raise PacketError("bad packet")
    t, seq = pkt[I.GL_OFS_TYPE], pkt[I.GL_OFS_SEQ]
    if t == I.GL_T_ACK:
        n = pkt[I.GL_ACK_OFS_LEN]
        d = pkt[I.GL_ACK_OFS_DATA:-2]
        tgt = pkt[I.GL_ACK_OFS_TARGET]
        return {"type": "ACK", "seq": seq, "target": tgt & ~I.GL_TGT_EMERGENCY & 0xFF,
                "emergency": bool(tgt & I.GL_TGT_EMERGENCY), "cmd": pkt[I.GL_ACK_OFS_CMD],
                "status": pkt[I.GL_ACK_OFS_STATUS], "total_len": n, "data": d.hex()}
    if t == I.GL_T_TLM:
        return {"type": "TLM", "seq": seq, "target": pkt[I.GL_TLM_OFS_TARGET], "cmd": pkt[I.GL_TLM_OFS_CMD],
                "offset": pkt[I.GL_TLM_OFS_OFFSET], "data": pkt[I.GL_TLM_OFS_DATA:-2].hex()}
    if t == I.GL_T_FILE and len(pkt) > I.GL_FILE_OFS_DATA + I.GL_CRC_LEN:
        return {"type": "FILE", "xfer": seq, "block": struct.unpack_from("<H", pkt, I.GL_FILE_OFS_BLOCK)[0],
                "data": pkt[I.GL_FILE_OFS_DATA:-2].hex()}
    if t == I.GL_T_EVENT and len(pkt) == I.GL_EV_LEN:
        return {"type": "EVENT", "seq": seq, "no": struct.unpack_from("<H", pkt, I.GL_EV_OFS_NO)[0],
                "source": {I.GL_EVS_ECMD: "ecmd", I.GL_EVS_AUTO: "auto"}.get(pkt[I.GL_EV_OFS_SOURCE], "?"),
                "target": pkt[I.GL_EV_OFS_TARGET], "cmd": pkt[I.GL_EV_OFS_CMD],
                "result": pkt[I.GL_EV_OFS_RESULT], "ecmd_seq": pkt[I.GL_EV_OFS_ECMD_SEQ],
                "detail": struct.unpack_from("<H", pkt, I.GL_EV_OFS_DETAIL)[0],
                "uptime_s": struct.unpack_from("<I", pkt, I.GL_EV_OFS_UPTIME_S)[0]}
    if t == I.GL_T_BEACON_OBC and len(pkt) == I.GL_BO_LEN:
        return {"type": "BEACON_OBC", "seq": seq,
                "uptime_s": struct.unpack_from("<I", pkt, I.GL_BO_OFS_UPTIME_S)[0],
                "obc_mode": pkt[I.GL_BO_OFS_OBC_MODE],
                "vbatt_mV": struct.unpack_from("<H", pkt, I.GL_BO_OFS_VBATT_MV)[0],
                "ibus_mA": struct.unpack_from("<h", pkt, I.GL_BO_OFS_IBUS_MA)[0],
                "adcs_mode": pkt[I.GL_BO_OFS_ADCS_MODE], "ttc_mode": pkt[I.GL_BO_OFS_TTC_MODE],
                "adcs_rate_dps": struct.unpack_from("<H", pkt, I.GL_BO_OFS_ADCS_RATE)[0] / 100.0,
                "comm_flags": pkt[I.GL_BO_OFS_COMM_FLAGS],
                "cpu_temp_C": struct.unpack_from("<b", pkt, I.GL_BO_OFS_CPU_TEMP)[0],
                "lux": struct.unpack_from("<H", pkt, I.GL_BO_OFS_LUX)[0]}
    if t == I.GL_T_BEACON_TTC and len(pkt) == I.GL_BT_LEN:
        return {"type": "BEACON_TTC", "seq": seq,
                "uptime_s": struct.unpack_from("<I", pkt, I.GL_BT_OFS_UPTIME_S)[0],
                "mode": pkt[I.GL_BT_OFS_MODE], "status_flags": pkt[I.GL_BT_OFS_STATUS_FLAGS],
                "faults": struct.unpack_from("<H", pkt, I.GL_BT_OFS_FAULTS)[0],
                "rx_count": struct.unpack_from("<H", pkt, I.GL_BT_OFS_RX_COUNT)[0],
                "temp_C": struct.unpack_from("<h", pkt, I.GL_BT_OFS_TEMP)[0] / 100.0,
                "last_rssi": struct.unpack_from("<h", pkt, I.GL_BT_OFS_LAST_RSSI)[0],
                "obc_contact": bool(pkt[I.GL_BT_OFS_OBC_CONTACT])}
    return {"type": f"0x{t:02X}", "seq": seq, "raw": pkt.hex()}


def build_obc_beacon(seq: int, uptime_s: int, obc_mode: int, vbatt_mv: int, ibus_ma: int, adcs_mode: int,
                     ttc_mode: int, adcs_rate_cdps: int, comm_flags: int, cpu_temp_c: int, lux: int) -> bytes:
    clamp = lambda v, lo, hi: max(lo, min(hi, int(v)))  # noqa: E731
    body = bytearray(I.GL_BO_LEN - 2)
    body[0:3] = bytes([I.GL_SAT_ID, I.GL_T_BEACON_OBC, seq & 0xFF])
    struct.pack_into("<I", body, I.GL_BO_OFS_UPTIME_S, clamp(uptime_s, 0, 2**32 - 1))
    body[I.GL_BO_OFS_OBC_MODE] = obc_mode & 0xFF
    struct.pack_into("<H", body, I.GL_BO_OFS_VBATT_MV, clamp(vbatt_mv, 0, 65535))
    struct.pack_into("<h", body, I.GL_BO_OFS_IBUS_MA, clamp(ibus_ma, -32768, 32767))
    body[I.GL_BO_OFS_ADCS_MODE] = adcs_mode & 0xFF
    body[I.GL_BO_OFS_TTC_MODE] = ttc_mode & 0xFF
    struct.pack_into("<H", body, I.GL_BO_OFS_ADCS_RATE, clamp(adcs_rate_cdps, 0, 65535))
    body[I.GL_BO_OFS_COMM_FLAGS] = comm_flags & 0xFF
    struct.pack_into("<b", body, I.GL_BO_OFS_CPU_TEMP, clamp(cpu_temp_c, -128, 127))
    struct.pack_into("<H", body, I.GL_BO_OFS_LUX, clamp(lux, 0, 65535))
    return _seal(bytes(body))


def reassemble(packets: List[dict]) -> Optional[bytes]:
    """Join an ACK and its TLM parts (from parse_downlink) back into one response."""
    ack = next((p for p in packets if p["type"] == "ACK"), None)
    if ack is None:
        return None
    data = bytearray(bytes.fromhex(ack["data"]))
    for p in sorted((p for p in packets if p["type"] == "TLM" and p["seq"] == ack["seq"]),
                    key=lambda p: p["offset"]):
        if p["offset"] == len(data):
            data += bytes.fromhex(p["data"])
    return bytes(data) if len(data) == ack["total_len"] else None
