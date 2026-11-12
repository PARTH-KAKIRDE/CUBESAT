"""ICD frame building/parsing and CRC-16/CCITT-FALSE (mirror of icd/icd_crc16.c)."""
from __future__ import annotations

from dataclasses import dataclass

from . import icd_defs as I


def crc16(data: bytes, crc: int = I.ICD_CRC_INIT) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ I.ICD_CRC_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class FrameError(Exception):
    """A response frame that must not be trusted (bad CRC or bad length)."""


@dataclass(frozen=True)
class Response:
    cmd: int
    status: int
    payload: bytes

    @property
    def ok(self) -> bool:
        return self.status == I.ICD_ST_OK


def build_command(cmd: int, payload: bytes = b"") -> bytes:
    if len(payload) > I.ICD_MAX_PAYLOAD:
        raise ValueError(f"payload too long ({len(payload)} > {I.ICD_MAX_PAYLOAD})")
    body = bytes([cmd & 0xFF, len(payload)]) + bytes(payload)
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def build_response(cmd: int, status: int, payload: bytes = b"") -> bytes:
    body = bytes([cmd, status, len(payload)]) + bytes(payload)
    c = crc16(body)
    frame = body + bytes([c & 0xFF, c >> 8])
    return frame + bytes(I.ICD_RESP_FRAME_LEN - len(frame))


def parse_response(raw: bytes) -> Response:
    if len(raw) < I.ICD_RESP_HDR_LEN + I.ICD_CRC_LEN:
        raise FrameError("short frame")
    n = raw[2]
    if n > I.ICD_MAX_PAYLOAD:
        raise FrameError(f"bad length {n}")
    end = I.ICD_RESP_HDR_LEN + n
    rx_crc = raw[end] | (raw[end + 1] << 8)
    if crc16(raw[:end]) != rx_crc:
        raise FrameError("bad crc")
    return Response(raw[0], raw[1], bytes(raw[3:end]))


STATUS_NAMES = {
    I.ICD_ST_OK: "OK",
    I.ICD_ST_BAD_CRC: "BAD_CRC",
    I.ICD_ST_UNKNOWN_CMD: "UNKNOWN_CMD",
    I.ICD_ST_BAD_LENGTH: "BAD_LENGTH",
    I.ICD_ST_BUSY: "BUSY",
    I.ICD_ST_INVALID_MODE: "INVALID_MODE",
    I.ICD_ST_HW_FAULT: "HW_FAULT",
    I.ICD_ST_NO_DATA: "NO_DATA",
    I.ICD_ST_BAD_PARAM: "BAD_PARAM",
    I.ICD_ST_QUEUE_FULL: "QUEUE_FULL",
}

MODE_NAMES = {I.ICD_MODE_BOOT: "BOOT", I.ICD_MODE_SAFE: "SAFE", I.ICD_MODE_DETUMBLE: "DETUMBLE",
              I.ICD_MODE_NOMINAL: "NOMINAL"}


def status_name(st: int) -> str:
    return STATUS_NAMES.get(st, f"0x{st:02X}")
