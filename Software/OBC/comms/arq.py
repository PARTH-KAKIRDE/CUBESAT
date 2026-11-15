"""Selective Repeat ARQ for file downlink (ground_link.h GL_VERSION 3/4, ICD.md section 11).

Why Selective Repeat and not Go-Back-N: the LoRa link is half duplex, slow
(~0.3 s per 24-byte packet at SF9/125 kHz/CR 4/8) and loses packets one at a
time (fading, interference), not in long runs. Go-Back-N would resend the
whole window after every loss; Selective Repeat resends only the gaps. The
OBC holds the whole file, so keeping out-of-order blocks costs nothing.

Half duplex shapes the protocol: the satellite cannot hear the ground while
it transmits, so there are no per-block ACKs. The OBC sends a burst of up to
W blocks; the ground answers once, after the burst, with FILE_ACK(base,
bitmap); the OBC resends only the blocks whose bit is 0. Retransmission timing
lives on the ground (it knows when a burst has ended); the satellite only
answers. That keeps the satellite from transmitting blindly, e.g. after the
ground station has lost the pass.

Block size (GL_VERSION 4): v3 blocks were 17 bytes so a FILE packet fitted one
24-byte QUEUE_TX. Each LoRa packet costs ~12 symbols of preamble plus a
header whatever its size, so 17-byte blocks spent most of the airtime on
overhead. v4 lets the OBC choose B (1..248, default 200) and announces it in
the FILE_START answer; TT&C takes the long packet in QUEUE_TX_PART pieces.
At SF9/125 kHz/CR 4/8 and W = 16 that is ~110 B/s instead of ~37 B/s.

    FileSender    OBC side: blocks, window, what to (re)send
    FileReceiver  ground side: stores blocks, builds FILE_ACK args, checks the CRC-32
"""
from __future__ import annotations

import struct
import zlib
from collections import deque
from typing import Deque, Dict, Optional

import icd_defs as I

from .ground import PacketError, _seal, check

SHORT_BLOCK = I.GL_FILE_SHORT_DATA      # v3 size: one 24-byte packet
DEFAULT_BLOCK = 200                     # v4 default (see the module docstring)


def n_blocks(size: int, block: int = SHORT_BLOCK) -> int:
    return (size + block - 1) // block


def build_block(xfer: int, block: int, data: bytes) -> bytes:
    if not 0 < len(data) <= I.GL_FILE_MAX_DATA:
        raise PacketError(f"bad block length {len(data)}")
    return _seal(bytes([I.GL_SAT_ID, I.GL_T_FILE, xfer & 0xFF]) + struct.pack("<H", block) + data,
                 I.GL_DOWN_MAX_LEN)


def parse_block(pkt: bytes) -> tuple:
    """-> (xfer, block, data). Raises PacketError."""
    if (not check(pkt, I.GL_DOWN_MAX_LEN) or pkt[I.GL_OFS_TYPE] != I.GL_T_FILE
            or len(pkt) < I.GL_FILE_OFS_DATA + 1 + I.GL_CRC_LEN):
        raise PacketError("not a FILE packet")
    return pkt[I.GL_OFS_SEQ], struct.unpack_from("<H", pkt, I.GL_FILE_OFS_BLOCK)[0], bytes(pkt[I.GL_FILE_OFS_DATA:-2])


def build_ack_args(xfer: int, base: int, bitmap: int) -> bytes:
    return struct.pack("<BHI", xfer & 0xFF, base, bitmap & 0xFFFFFFFF)


def parse_ack_args(a: bytes) -> tuple:
    if len(a) != I.GL_FA_LEN:
        raise PacketError("bad FILE_ACK length")
    return struct.unpack("<BHI", a)


def build_start_info(xfer: int, size: int, crc32: int, window: int, block: int) -> bytes:
    return struct.pack("<BIHIBB", xfer & 0xFF, size, n_blocks(size, block), crc32, window, block)


def parse_start_info(d: bytes) -> dict:
    if len(d) != I.GL_FS_LEN:
        raise PacketError("bad FILE_START response length")
    x, size, blocks, crc, w, b = struct.unpack("<BIHIBB", d)
    return {"xfer": x, "size": size, "blocks": blocks, "crc32": crc, "window": w, "block": b}


class FileSender:
    """One file being sent. The OBC feeds `next_packet()` into the TT&C queue."""

    def __init__(self, xfer: int, data: bytes, window: int, now: float, block: int = SHORT_BLOCK):
        if not data:
            raise ValueError("empty file")
        if not 1 <= block <= I.GL_FILE_MAX_DATA:
            raise ValueError("block size out of range")
        if n_blocks(len(data), block) > I.GL_FILE_MAX_BLOCKS:
            raise ValueError("file too large for 16-bit block numbers")
        if not 1 <= window <= I.GL_FILE_WINDOW_MAX:
            raise ValueError("window out of range")
        self.xfer, self.data, self.window, self.block = xfer & 0xFF, data, window, block
        self.blocks = n_blocks(len(data), block)
        self.crc32 = zlib.crc32(data) & 0xFFFFFFFF
        self.base = 0                       # every block below base is acknowledged
        self.acked = bytearray(self.blocks)  # selective acknowledgements above base
        self.pending: Deque[int] = deque(range(min(window, self.blocks)))  # first burst
        self.last_activity = now
        self.sent_total = 0                 # blocks handed to TT&C, retransmissions included

    @property
    def done(self) -> bool:
        return self.base >= self.blocks

    def start_info(self) -> bytes:
        return build_start_info(self.xfer, len(self.data), self.crc32, self.window, self.block)

    def packet(self, k: int) -> bytes:
        b = self.block
        return build_block(self.xfer, k, self.data[k * b:(k + 1) * b])

    def on_ack(self, base: int, bitmap: int, now: float) -> int:
        """Apply a FILE_ACK; queue the missing blocks of the new window. -> blocks queued."""
        if base > self.blocks:
            raise ValueError("base beyond the end of the file")
        self.last_activity = now
        for k in range(self.base, min(base, self.blocks)):
            self.acked[k] = 1
        self.base = max(self.base, base)     # never slide back
        for i in range(I.GL_FILE_WINDOW_MAX):
            k = base + i
            if bitmap >> i & 1 and k < self.blocks:
                self.acked[k] = 1
        while self.base < self.blocks and self.acked[self.base]:
            self.base += 1
        # The request replaces whatever was still waiting: no block is sent twice
        # for one FILE_ACK, and a stale queue never outlives a newer request.
        self.pending = deque(k for k in range(self.base, min(self.base + self.window, self.blocks))
                             if not self.acked[k])
        return len(self.pending)

    def next_packet(self) -> Optional[bytes]:
        return self.packet(self.pending[0]) if self.pending else None

    def pop(self) -> None:
        self.pending.popleft()
        self.sent_total += 1


class FileReceiver:
    """Ground side. Feed it FILE packets; ask it for the FILE_ACK args after each burst."""

    def __init__(self, info: dict):
        self.xfer, self.size, self.blocks = info["xfer"], info["size"], info["blocks"]
        self.crc32, self.window, self.block = info["crc32"], info["window"], info["block"]
        if not 1 <= self.block <= I.GL_FILE_MAX_DATA or self.blocks != n_blocks(self.size, self.block):
            raise PacketError("FILE_START: block count does not match the size")
        self.got: Dict[int, bytes] = {}
        self.duplicates = 0

    @property
    def base(self) -> int:
        b = 0
        while b in self.got:
            b += 1
        return b

    @property
    def done(self) -> bool:
        return len(self.got) == self.blocks

    def feed(self, pkt: bytes) -> bool:
        """True if the packet was a new block of this transfer."""
        try:
            x, k, d = parse_block(pkt)
        except PacketError:
            return False
        last = self.size - (self.blocks - 1) * self.block
        if x != self.xfer or k >= self.blocks or len(d) != (last if k == self.blocks - 1 else self.block):
            return False
        if k in self.got:
            self.duplicates += 1
            return False
        self.got[k] = d
        return True

    def ack_args(self) -> bytes:
        base = self.base
        bitmap = 0
        for i in range(I.GL_FILE_WINDOW_MAX):
            if base + i in self.got:
                bitmap |= 1 << i
        return build_ack_args(self.xfer, base, bitmap)

    def data(self) -> bytes:
        if not self.done:
            raise PacketError("transfer not complete")
        d = b"".join(self.got[k] for k in range(self.blocks))
        if zlib.crc32(d) & 0xFFFFFFFF != self.crc32:
            raise PacketError("file CRC-32 mismatch")
        return d
