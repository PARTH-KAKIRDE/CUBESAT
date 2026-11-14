"""ICD master side (OBC): CRC, framing, transactions, COMM_FAULT tracking.

Standard library only (from v0.1), with the v0.2 transaction rules:

  1. write the command frame, then STOP
  2. wait >= ICD_TURNAROUND_MS
  3. read exactly ICD_RESP_FRAME_LEN bytes
     - STATUS = BUSY            -> wait ICD_BUSY_RETRY_MS and re-read (up to ICD_BUSY_RETRIES times)
     - bad CRC / bad LEN / wrong echo on the read -> re-read. The slave keeps its
                                   response, so the command is NOT executed twice.
     - STATUS = BAD_CRC / BAD_LENGTH -> the slave did not act on our frame, so it
                                   is safe to send it again (once).
  ICD_COMM_FAULT_THRESHOLD failed transactions in a row -> COMM_FAULT (logged
  once); polling continues and the fault clears on the next success.

Lost replies: if a command was written but none of its reads succeeded, the
slave may still have executed it (GET_STATUS clears flags, GET_RX pops a
packet). The slave holds that reply until the next command, so before writing
the next command we read once more and keep the reply in `recovered[cmd]`.
SlaveClient.status() merges a recovered GET_STATUS and also treats uptime
going backwards as a reset.
"""
from __future__ import annotations

import errno
import logging
import os
import struct
import time
from dataclasses import dataclass, field
from typing import Optional

import icd_defs as I

I2C_SLAVE = 0x0703  # ioctl from <linux/i2c-dev.h>


# ------------------------------------------------------------------ CRC and frames

def crc16(data: bytes, crc: int = I.ICD_CRC_INIT) -> int:
    """CRC-16/CCITT-FALSE. crc16(b"123456789") == ICD_CRC_CHECK (0x29B1)."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ I.ICD_CRC_POLY) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc_selftest() -> None:
    got = crc16(b"123456789")
    if got != I.ICD_CRC_CHECK:
        raise AssertionError(f"CRC self-test failed: 0x{got:04X} != 0x{I.ICD_CRC_CHECK:04X}")


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
    """Slave-side frame builder (for tests and simulators)."""
    body = bytes([cmd, status, len(payload)]) + bytes(payload)
    c = crc16(body)
    frame = body + bytes([c & 0xFF, c >> 8])
    return frame + bytes(I.ICD_RESP_FRAME_LEN - len(frame))


def parse_response(raw: bytes) -> Response:
    if len(raw) != I.ICD_RESP_FRAME_LEN:
        raise FrameError(f"read {len(raw)} bytes, expected {I.ICD_RESP_FRAME_LEN}")
    n = raw[I.ICD_RESP_OFS_LEN]
    if n > I.ICD_MAX_PAYLOAD:
        raise FrameError(f"bad LEN {n}")
    end = I.ICD_RESP_HDR_LEN + n
    if crc16(raw[:end]) != (raw[end] | (raw[end + 1] << 8)):
        raise FrameError("bad crc")
    return Response(raw[I.ICD_RESP_OFS_ID], raw[I.ICD_RESP_OFS_STATUS], bytes(raw[I.ICD_RESP_OFS_PAYLOAD:end]))


STATUS_NAMES = {v: k[len("ICD_ST_"):] for k, v in vars(I).items() if k.startswith("ICD_ST_")}
STATUS_NAMES.update({v: k[len("GL_ST_"):] for k, v in vars(I).items() if k.startswith("GL_ST_")})
MODE_NAMES = {I.ICD_MODE_BOOT: "BOOT", I.ICD_MODE_SAFE: "SAFE", I.ICD_MODE_DETUMBLE: "DETUMBLE",
              I.ICD_MODE_NOMINAL: "NOMINAL"}


def status_name(st: int) -> str:
    return STATUS_NAMES.get(st, f"0x{st:02X}")


# ------------------------------------------------------------------ bus

class BusError(Exception):
    """NACK, timeout or I/O error on the bus."""


class I2CDev:
    """Linux /dev/i2c-N with plain write and read transfers (each ends in STOP).
    Standard library only, so it runs on a fresh Pi image."""

    def __init__(self, bus_num: int):
        import fcntl

        self._ioctl = fcntl.ioctl
        self.bus_num = bus_num
        self.fd = os.open(f"/dev/i2c-{bus_num}", os.O_RDWR)
        self._addr = None

    def _select(self, addr: int) -> None:
        if addr != self._addr:
            self._ioctl(self.fd, I2C_SLAVE, addr)
            self._addr = addr

    def write(self, addr: int, data: bytes) -> None:
        try:
            self._select(addr)
            os.write(self.fd, bytes(data))
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} write: {errno.errorcode.get(e.errno, e)}") from e

    def read(self, addr: int, n: int) -> bytes:
        try:
            self._select(addr)
            return os.read(self.fd, n)
        except OSError as e:
            raise BusError(f"i2c-{self.bus_num} 0x{addr:02X} read: {errno.errorcode.get(e.errno, e)}") from e

    def wait_ms(self, ms: float) -> None:
        time.sleep(ms / 1000.0)

    def close(self) -> None:
        os.close(self.fd)


def bus_speed_hz(bus_num: int) -> Optional[int]:
    """Configured clock of /dev/i2c-<bus> from the device tree, or None (from v0.1)."""
    try:
        with open(f"/sys/class/i2c-adapter/i2c-{bus_num}/of_node/clock-frequency", "rb") as f:
            return int.from_bytes(f.read(4), "big")
    except OSError:
        return None


# ------------------------------------------------------------------ link

class LinkError(Exception):
    """The transaction failed (no trustworthy response)."""


class SlaveError(Exception):
    """The slave answered, but with an error status."""

    def __init__(self, cmd: int, status: int):
        super().__init__(f"cmd 0x{cmd:02X} -> {status_name(status)}")
        self.cmd, self.status = cmd, status


@dataclass
class LinkStats:
    transactions: int = 0
    ok: int = 0
    failures: int = 0
    busy_retries: int = 0
    read_crc_errors: int = 0
    resends: int = 0
    nacks: int = 0
    recovered: int = 0
    consecutive_failures: int = 0
    last_error: str = ""
    comm_fault: bool = False
    comm_fault_events: int = 0
    errors: dict = field(default_factory=dict)   # error text -> count (from v0.1)


class SlaveLink:
    """One STM32 slave. `bus` needs write(addr, data), read(addr, n), wait_ms(ms)."""

    def __init__(self, bus, addr: int, name: str, log: Optional[logging.Logger] = None):
        self.bus, self.addr, self.name = bus, addr, name
        self.log = log or logging.getLogger(f"link.{name}")
        self.stats = LinkStats()
        self._unread_cmd: Optional[int] = None   # written, but its reply was never read
        self.recovered: dict = {}                # cmd -> Response read back late

    def _recover_unread(self) -> None:
        """One quiet read of the reply we missed. Not counted as a failure."""
        cmd, self._unread_cmd = self._unread_cmd, None
        try:
            resp = parse_response(self.bus.read(self.addr, I.ICD_RESP_FRAME_LEN))
        except (BusError, FrameError):
            return
        # The echo check rejects anything else (e.g. the idle frame after a reset).
        if resp.cmd == cmd and resp.status != I.ICD_ST_BUSY:
            self.recovered[cmd] = resp
            self.stats.recovered += 1
            self.log.warning("%s: recovered lost reply to cmd 0x%02X", self.name, cmd)

    def _read_response(self, cmd: int):
        s = self.stats
        err = "no response"
        for attempt in range(1 + I.ICD_BUSY_RETRIES):
            if attempt:
                self.bus.wait_ms(I.ICD_BUSY_RETRY_MS)
            try:
                raw = self.bus.read(self.addr, I.ICD_RESP_FRAME_LEN)
            except BusError as e:
                s.nacks += 1
                return None, f"read: {e}"
            try:
                resp = parse_response(raw)
            except FrameError as e:
                s.read_crc_errors += 1
                err = f"read frame: {e}"
                continue
            if resp.cmd != (cmd & 0xFF):
                err = f"echo 0x{resp.cmd:02X} != 0x{cmd & 0xFF:02X}"
                continue
            if resp.status == I.ICD_ST_BUSY:
                s.busy_retries += 1
                err = "still BUSY after retries"
                continue
            return resp, ""
        return None, err

    def transact(self, cmd: int, payload: bytes = b"") -> Response:
        """One ICD transaction. Returns the response (any status except BUSY).
        Raises LinkError if no trustworthy response was obtained."""
        s = self.stats
        s.transactions += 1
        if self._unread_cmd is not None:
            self._recover_unread()
        frame = build_command(cmd, payload)
        err = "unknown"
        for send in range(2):  # original + one resend after BAD_CRC/BAD_LENGTH
            try:
                self.bus.write(self.addr, frame)
            except BusError as e:
                s.nacks += 1
                err = f"write: {e}"
                break
            self.bus.wait_ms(I.ICD_TURNAROUND_MS)
            resp, err = self._read_response(cmd)
            if resp is None:
                self._unread_cmd = cmd & 0xFF
                break
            if resp.status in (I.ICD_ST_BAD_CRC, I.ICD_ST_BAD_LENGTH) and send == 0:
                s.resends += 1
                continue
            return self._success(resp)
        return self._failure(err)

    def transact_raw(self, frame: bytes, cmd: int) -> Response:
        """Send a hand-made (possibly broken) frame once, no resend. For tests."""
        self.bus.write(self.addr, frame)
        self.bus.wait_ms(I.ICD_TURNAROUND_MS)
        resp, err = self._read_response(cmd)
        if resp is None:
            raise LinkError(f"{self.name}: {err}")
        return resp

    def call(self, cmd: int, payload: bytes = b"") -> bytes:
        """Transaction that must return OK. Returns the payload."""
        r = self.transact(cmd, payload)
        if not r.ok:
            raise SlaveError(cmd, r.status)
        return r.payload

    def _success(self, resp: Response) -> Response:
        s = self.stats
        s.ok += 1
        s.consecutive_failures = 0
        if s.comm_fault:
            s.comm_fault = False
            self.log.warning("%s: link recovered", self.name)
        return resp

    def _failure(self, err: str):
        s = self.stats
        s.failures += 1
        s.consecutive_failures += 1
        s.last_error = err
        s.errors[err] = s.errors.get(err, 0) + 1
        if s.consecutive_failures >= I.ICD_COMM_FAULT_THRESHOLD and not s.comm_fault:
            s.comm_fault = True
            s.comm_fault_events += 1
            self.log.error("%s: COMM_FAULT after %d failed transactions (%s)", self.name,
                           s.consecutive_failures, err)
        raise LinkError(f"{self.name}: {err}")


# ------------------------------------------------------------------ common commands

@dataclass
class SlaveStatus:
    mode: int
    flags: int
    faults: int
    uptime_s: int

    @property
    def reset_occurred(self) -> bool:
        return bool(self.flags & I.ICD_SF_RESET_OCCURRED)

    @property
    def fault_active(self) -> bool:
        return bool(self.flags & I.ICD_SF_FAULT_ACTIVE)

    @property
    def no_master_contact(self) -> bool:
        return bool(self.flags & I.ICD_SF_NO_MASTER_CONTACT)

    @property
    def ground_pkt_pending(self) -> bool:
        return bool(self.flags & I.ICD_SF_GROUND_PKT_PENDING)


def _parse_status(p: bytes) -> SlaveStatus:
    return SlaveStatus(p[I.ICD_STATUS_OFS_MODE], p[I.ICD_STATUS_OFS_FLAGS],
                       struct.unpack_from("<H", p, I.ICD_STATUS_OFS_FAULTS)[0],
                       struct.unpack_from("<I", p, I.ICD_STATUS_OFS_UPTIME_S)[0])


_ONE_SHOT_FLAGS = I.ICD_SF_RESET_OCCURRED | I.ICD_SF_NO_MASTER_CONTACT


class SlaveClient:
    """The common commands of section 5, with the lost-reply guards."""

    def __init__(self, link: SlaveLink):
        self.link = link
        self.last_uptime_s: Optional[int] = None

    def ping(self) -> None:
        self.link.call(I.ICD_CMD_PING)

    def version(self) -> dict:
        p = self.link.call(I.ICD_CMD_GET_VERSION)
        return {"fw": f"{p[0]}.{p[1]}.{p[2]}", "icd_major": p[3], "icd_minor": p[4]}

    def status(self) -> SlaveStatus:
        st = _parse_status(self.link.call(I.ICD_CMD_GET_STATUS))
        lost = self.link.recovered.pop(I.ICD_CMD_GET_STATUS, None)
        if lost is not None and lost.ok and len(lost.payload) >= I.ICD_GET_STATUS_RESP_LEN:
            old = _parse_status(lost.payload)
            st.flags |= old.flags & _ONE_SHOT_FLAGS
            st.faults |= old.faults
        # uptime_s comes from a u32 ms clock, so it also falls back to 0 after
        # 49.7 days; that only causes one harmless re-configuration.
        if self.last_uptime_s is not None and st.uptime_s < self.last_uptime_s:
            st.flags |= I.ICD_SF_RESET_OCCURRED
        self.last_uptime_s = st.uptime_s
        return st

    def set_mode(self, mode: int) -> int:
        return self.link.transact(I.ICD_CMD_SET_MODE, bytes([mode])).status

    def telemetry(self, page: int) -> bytes:
        return self.link.call(I.ICD_CMD_GET_TELEMETRY, bytes([page]))
