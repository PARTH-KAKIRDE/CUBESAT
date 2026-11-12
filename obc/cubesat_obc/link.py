"""ICD master side: one SlaveLink per STM32 (ADCS, TT&C).

Transaction (ICD section 2):
  1. write the command frame
  2. wait >= 5 ms
  3. read exactly 32 bytes
     - STATUS = BUSY        -> wait 5 ms and re-read (up to 3 times)
     - CRC error on the read -> re-read. The slave keeps its response, so the
                               command is NOT executed twice.
     - STATUS = BAD_CRC / BAD_LENGTH -> the slave did not act on our frame, so
                               it is safe to send it again (once).
Three failed transactions in a row -> COMM_FAULT (logged); polling continues
and the fault clears on the next success.

Lost replies: if the command was written but no reply could be read, the slave
may still have executed it (GET_STATUS clears flags, GET_RX pops a packet).
The slave holds that reply until the next command, so before writing the next
command we read once more and keep the reply in `recovered[cmd]` for the
caller to collect.
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field

from . import icd_defs as I
from .bus import BusError, I2CBus
from .frames import FrameError, Response, build_command, parse_response, status_name


class LinkError(Exception):
    """The transaction failed (no trustworthy response)."""


class SlaveError(Exception):
    """The slave answered, but with an error status."""

    def __init__(self, cmd: int, status: int):
        super().__init__(f"cmd 0x{cmd:02X} -> {status_name(status)}")
        self.cmd = cmd
        self.status = status


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
    extra: dict = field(default_factory=dict)


class SlaveLink:
    def __init__(self, bus: I2CBus, addr: int, name: str, log: logging.Logger | None = None):
        self.bus = bus
        self.addr = addr
        self.name = name
        self.log = log or logging.getLogger(f"link.{name}")
        self.stats = LinkStats()
        self._unread_cmd: int | None = None   # written, but its reply was never read
        self.recovered: dict = {}             # cmd -> Response read back late

    # -------------------------------------------------------------- core
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

    def transact(self, cmd: int, payload: bytes = b"") -> Response:
        """Run one ICD transaction. Returns the response (any status except BUSY).
        Raises LinkError if no trustworthy response was obtained."""
        s = self.stats
        s.transactions += 1
        if self._unread_cmd is not None:
            self._recover_unread()
        frame = build_command(cmd, payload)
        err = "unknown"
        for _send in range(2):  # original + one resend after BAD_CRC/BAD_LENGTH
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
            if resp.status in (I.ICD_ST_BAD_CRC, I.ICD_ST_BAD_LENGTH) and _send == 0:
                s.resends += 1
                continue
            return self._success(resp)
        return self._failure(err)

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
                err = f"echo 0x{resp.cmd:02X} != 0x{cmd:02X}"
                continue
            if resp.status == I.ICD_ST_BUSY:
                s.busy_retries += 1
                err = "busy"
                continue
            return resp, ""
        return None, err

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
        if s.consecutive_failures >= I.ICD_COMM_FAULT_THRESHOLD and not s.comm_fault:
            s.comm_fault = True
            s.comm_fault_events += 1
            self.log.error("%s: COMM_FAULT after %d failed transactions (%s)", self.name,
                           s.consecutive_failures, err)
        raise LinkError(f"{self.name}: {err}")

    # -------------------------------------------------------------- helpers
    def call(self, cmd: int, payload: bytes = b"") -> bytes:
        """Transaction that must return OK. Returns the payload."""
        r = self.transact(cmd, payload)
        if not r.ok:
            raise SlaveError(cmd, r.status)
        return r.payload
