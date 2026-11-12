"""Ground-link packet codec (icd/ground_link.h). Used by the OBC and by ground/gs_tool.py.

CMD packets carry a counter and a MAC (GL_VERSION 2):
  SAT_ID | TYPE | SEQ | TARGET | CMD | LEN | ARGS | COUNTER u32 | MAC(4) | CRC16
MAC = HMAC-SHA256(key, SAT_ID .. COUNTER)[:4].
"""
from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

from . import icd_defs as I
from .frames import crc16

MIN_KEY_BYTES = 16


class PacketError(Exception):
    pass


def _seal(body: bytes) -> bytes:
    c = crc16(body)
    pkt = body + bytes([c & 0xFF, c >> 8])
    if len(pkt) > I.GL_MAX_LEN:
        raise PacketError(f"packet too long ({len(pkt)})")
    return pkt


def check(pkt: bytes) -> bool:
    if not I.GL_HDR_LEN + I.GL_CRC_LEN <= len(pkt) <= I.GL_MAX_LEN:
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
    """Key file: hex text (as written by `gs_tool.py keygen`)."""
    try:
        key = bytes.fromhex(Path(path).read_text().strip())
    except ValueError as e:
        raise PacketError(f"key file {path} is not hex") from e
    if len(key) < MIN_KEY_BYTES:
        raise PacketError(f"key file {path}: key shorter than {MIN_KEY_BYTES} bytes")
    return key


def _mac(key: bytes, data: bytes) -> bytes:
    return hmac.new(key, data, hashlib.sha256).digest()[: I.GL_AUTH_MAC_LEN]


def build_cmd(seq: int, target: int, cmd: int, args: bytes = b"", *, key: bytes, counter: int) -> bytes:
    if len(args) > I.GL_CMD_MAX_ARGS:
        raise PacketError(f"too many args ({len(args)} > {I.GL_CMD_MAX_ARGS})")
    if not 0 < counter < 2**32:
        raise PacketError("counter out of range")
    body = bytes([I.GL_SAT_ID, I.GL_T_CMD, seq & 0xFF, target, cmd, len(args)]) + args + struct.pack("<I", counter)
    return _seal(body + _mac(key, body))


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
    """Decode any satellite->ground packet into a dict (for the ground tool)."""
    if not check(pkt):
        raise PacketError("bad packet")
    t, seq = pkt[I.GL_OFS_TYPE], pkt[I.GL_OFS_SEQ]
    if t == I.GL_T_ACK:
        n = pkt[I.GL_ACK_OFS_LEN]
        d = pkt[I.GL_ACK_OFS_DATA:-2]
        return {"type": "ACK", "seq": seq, "target": pkt[I.GL_ACK_OFS_TARGET], "cmd": pkt[I.GL_ACK_OFS_CMD],
                "status": pkt[I.GL_ACK_OFS_STATUS], "total_len": n, "data": d.hex()}
    if t == I.GL_T_TLM:
        return {"type": "TLM", "seq": seq, "target": pkt[I.GL_TLM_OFS_TARGET], "cmd": pkt[I.GL_TLM_OFS_CMD],
                "offset": pkt[I.GL_TLM_OFS_OFFSET], "data": pkt[I.GL_TLM_OFS_DATA:-2].hex()}
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
