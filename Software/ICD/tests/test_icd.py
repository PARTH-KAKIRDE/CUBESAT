"""Tests for the ICD package itself (run from ICD/: python -m pytest -q tests).

Needs gcc for the C parts. Covers:
  - headers: generator in sync and strict, every layout adds up
  - C and Python CRC agree; icd_pack.h round-trips
  - the Python master (python/icd_link.py) against scripted buses
  - the Python master against the C reference slave (slave/), end to end
"""
import ctypes
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ICD = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ICD / "python"))

import icd_defs as I  # noqa: E402
from icd_link import (BusError, LinkError, SlaveClient, SlaveLink, build_command,  # noqa: E402
                      build_response, crc16, parse_response)

needs_gcc = pytest.mark.skipif(shutil.which("gcc") is None, reason="gcc not found")


# ------------------------------------------------------------------ headers

def test_generated_constants_up_to_date():
    r = subprocess.run([sys.executable, "-I", str(ICD / "tools" / "gen_icd_py.py"), "--check"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout


def test_generator_rejects_lines_it_cannot_parse(tmp_path):
    (tmp_path / "tools").mkdir()
    (tmp_path / "python").mkdir()
    shutil.copy(ICD / "tools" / "gen_icd_py.py", tmp_path / "tools")
    for h in ("cubesat_icd.h", "ground_link.h"):
        shutil.copy(ICD / h, tmp_path)
    gen = [sys.executable, "-I", str(tmp_path / "tools" / "gen_icd_py.py")]
    assert subprocess.run(gen, capture_output=True).returncode == 0
    h = tmp_path / "ground_link.h"
    good = h.read_text()
    for bad in ("#define BAD_A 5 // comment", "#define BAD_B 0x10UL", "#define BAD_C (1u << 3)", "#define BAD_D (-1)"):
        h.write_text(good + bad + "\n")
        r = subprocess.run(gen, capture_output=True, text=True)
        assert r.returncode != 0 and "cannot parse" in r.stdout + r.stderr, bad
    h.write_text(good)


SIZES = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4}
LAYOUTS = {  # prefix, length constant, first offset, trailing CRC bytes
    "ICD_STATUS_OFS_": ("ICD_GET_STATUS_RESP_LEN", 0, 0), "ADCS_CFG_OFS_": ("ADCS_CFG_LEN", 0, 0),
    "ADCS_TLM0_OFS_": ("ADCS_TLM0_LEN", 0, 0), "ADCS_TLM1_OFS_": ("ADCS_TLM1_LEN", 0, 0),
    "ADCS_TLM2_OFS_": ("ADCS_TLM2_LEN", 0, 0), "TTC_RADIO_OFS_": ("TTC_RADIO_LEN", 0, 0),
    "TTC_TLM0_OFS_": ("TTC_TLM0_LEN", 0, 0), "TTC_TLM1_OFS_": ("TTC_TLM1_LEN", 0, 0),
    "DUMMY_TLM0_OFS_": ("DUMMY_TLM0_LEN", 0, 0), "GL_HK_OFS_": ("GL_HK_LEN", 0, 0),
    "GL_EPS_OFS_": ("GL_EPS_LEN", 0, 0), "GL_BO_OFS_": ("GL_BO_LEN", 3, 2), "GL_BT_OFS_": ("GL_BT_LEN", 3, 2),
    "GL_EPSS_OFS_": ("GL_EPSS_LEN", 0, 0),
}


def _field_types():
    types, prev = {}, None
    for h in ("cubesat_icd.h", "ground_link.h"):
        for line in (ICD / h).read_text().splitlines():
            m = re.match(r"\s*#define\s+(\w+_OFS_\w+)\s+\d+\s*(?:/\*\s*(u8|i8|u16|i16|u32)\b)?", line)
            if m:
                prev = m.group(2) or prev
                types[m.group(1)] = prev
            elif "#define" in line:
                prev = None
    return types


@pytest.mark.parametrize("prefix", LAYOUTS)
def test_layout_has_no_gaps_or_overlaps(prefix):
    length_name, start, crc = LAYOUTS[prefix]
    types = _field_types()
    fields = sorted((getattr(I, k), k) for k in vars(I) if k.startswith(prefix))
    assert fields and fields[0][0] == start
    ends = [o for o, _ in fields[1:]] + [getattr(I, length_name) - crc]
    for (off, name), nxt in zip(fields, ends):
        assert off + SIZES[types[name]] == nxt, f"{name} @ {off} ({types[name]}) does not end at {nxt}"
    assert getattr(I, length_name) <= max(I.ICD_MAX_PAYLOAD, I.GL_MAX_LEN)


def test_constant_arithmetic():
    assert I.ICD_CMD_MIN_LEN == I.ICD_CMD_HDR_LEN + I.ICD_CRC_LEN
    assert I.ICD_CMD_MAX_LEN == I.ICD_CMD_HDR_LEN + I.ICD_MAX_PAYLOAD + I.ICD_CRC_LEN
    assert I.ICD_RESP_HDR_LEN + I.ICD_MAX_PAYLOAD + I.ICD_CRC_LEN <= I.ICD_RESP_FRAME_LEN
    assert (I.ICD_CMD_OFS_PAYLOAD, I.ICD_RESP_OFS_PAYLOAD) == (I.ICD_CMD_HDR_LEN, I.ICD_RESP_HDR_LEN)
    assert I.ICD_TURNAROUND_MS + I.ICD_BUSY_RETRIES * I.ICD_BUSY_RETRY_MS == 20   # slave reply budget
    assert I.ICD_CMD_ADCS_FIRST <= min(v for k, v in vars(I).items() if k.startswith("ADCS_CMD_"))
    assert max(v for k, v in vars(I).items() if k.startswith("ADCS_CMD_")) <= I.ICD_CMD_ADCS_LAST
    assert I.ICD_CMD_TTC_FIRST <= min(v for k, v in vars(I).items() if k.startswith("TTC_CMD_"))
    assert max(v for k, v in vars(I).items() if k.startswith("TTC_CMD_")) <= I.ICD_CMD_TTC_LAST
    assert I.ICD_SF_RESERVED_MASK & (I.ICD_SF_FAULT_ACTIVE | I.ICD_SF_GROUND_PKT_PENDING |
                                     I.ICD_SF_NO_MASTER_CONTACT | I.ICD_SF_RESET_OCCURRED) == 0
    assert I.GL_CMD_MAX_ARGS == I.GL_MAX_LEN - I.GL_CMD_OFS_ARGS - I.GL_AUTH_CTR_LEN - I.GL_AUTH_MAC_LEN - I.GL_CRC_LEN
    assert I.GL_ACK_MAX_DATA == I.GL_MAX_LEN - I.GL_ACK_OFS_DATA - I.GL_CRC_LEN
    assert I.GL_TLM_MAX_DATA == I.GL_MAX_LEN - I.GL_TLM_OFS_DATA - I.GL_CRC_LEN
    assert I.ADCS_MTQ_MANUAL_LEN <= I.GL_CMD_MAX_ARGS            # largest ground-forwarded command fits
    assert I.GL_EPSS_LEN <= I.GL_ACK_MAX_DATA + I.GL_TLM_MAX_DATA   # ACK + one TLM part (v0.4)
    for k, v in vars(I).items():                                  # flag bytes: single bits, fit in a u8
        if k.startswith(("GL_EF_", "GL_SUNF_")):
            assert v and v & (v - 1) == 0 and v <= 0xFF, k
    for name, text in (("TTC_BURN_ARM_KEY", b"BURN"), ("GL_REBOOT_KEY", b"BOOT"), ("GL_PYRO_KEY", b"PYRO")):
        assert getattr(I, name).to_bytes(4, "little") == text
    assert max(v for k, v in vars(I).items() if k.startswith("ICD_ST_")) < min(
        v for k, v in vars(I).items() if k.startswith("GL_ST_"))


@pytest.mark.parametrize("prefix", ["ICD_ST_", "ICD_CMD_", "ADCS_CMD_", "TTC_CMD_", "ICD_MODE_", "ICD_MR_",
                                    "ADCS_FLT_", "TTC_FLT_", "ICD_SF_", "GL_T_", "GL_TGT_", "GL_ST_",
                                    "GL_PS_", "GL_EF_", "GL_SUNF_", "GL_EPS_"])
def test_codes_are_unique(prefix):
    skip = ("_LEN", "_COUNT", "_MASK", "_NONE", "_FIRST", "_LAST")
    vals = [v for k, v in vars(I).items() if k.startswith(prefix) and "_OFS_" not in k and not k.endswith(skip)]
    assert len(vals) == len(set(vals))


# ------------------------------------------------------------------ C code

def _build(tmp_path, name, sources, extra=()):
    lib = tmp_path / f"{name}.so"
    cmd = ["gcc", "-std=c11", "-O2", "-shared", "-fPIC", "-Wall", "-Wextra", "-Wshadow", "-Werror",
           f"-I{ICD}", f"-I{ICD / 'slave'}", *extra, *map(str, sources), "-o", str(lib)]
    subprocess.run(cmd, check=True)
    return ctypes.CDLL(str(lib))


@needs_gcc
def test_c_and_python_crc_agree(tmp_path):
    lib = _build(tmp_path, "crc", [ICD / "icd_crc16.c"])
    lib.icd_crc16.restype = ctypes.c_uint16
    lib.icd_crc16.argtypes = (ctypes.c_char_p, ctypes.c_size_t)
    lib.icd_crc16_update.restype = ctypes.c_uint16
    lib.icd_crc16_update.argtypes = (ctypes.c_uint16, ctypes.c_char_p, ctypes.c_size_t)
    assert lib.icd_crc16(b"123456789", 9) == crc16(b"123456789") == I.ICD_CRC_CHECK
    rnd = random.Random(3)
    for _ in range(5000):
        b = bytes(rnd.randrange(256) for _ in range(rnd.randrange(40)))
        assert lib.icd_crc16(b, len(b)) == crc16(b)
        k = rnd.randrange(len(b) + 1)                 # streaming == one shot
        assert lib.icd_crc16_update(lib.icd_crc16(b[:k], k), b[k:], len(b) - k) == crc16(b)


@needs_gcc
def test_icd_pack_roundtrip(tmp_path):
    src = tmp_path / "pack.c"
    src.write_text('#include "icd_pack.h"\n'
                   "uint32_t rt32(uint32_t v){uint8_t b[4]; icd_put_u32(b,v); return icd_get_u32(b);}\n"
                   "int rt16(int v){uint8_t b[2]; icd_put_i16(b,(int16_t)v); return icd_get_i16(b);}\n"
                   "void put32(uint8_t*b,uint32_t v){icd_put_u32(b,v);}\n"
                   "int sat16(int v){return icd_sat_i16(v);} int satu16(int v){return icd_sat_u16(v);}\n")
    lib = _build(tmp_path, "pack", [src])
    lib.rt32.restype = ctypes.c_uint32
    lib.rt32.argtypes = lib.put32.argtypes = None
    for v in (0, 1, 0x12345678, 0xFFFFFFFF):
        assert lib.rt32(ctypes.c_uint32(v)) == v
    for v in (-32768, -1, 0, 1, 32767):
        assert lib.rt16(v) == v
    buf = (ctypes.c_uint8 * 4)()
    lib.put32(buf, ctypes.c_uint32(0x11223344))
    assert bytes(buf) == bytes([0x44, 0x33, 0x22, 0x11])     # little-endian on the wire
    assert (lib.sat16(40000), lib.sat16(-40000), lib.satu16(-5), lib.satu16(70000)) == (32767, -32768, 0, 65535)


# ------------------------------------------------------------------ master against scripted buses

class ScriptBus:
    """Each read pops the next scripted frame (or raises it if it is an exception)."""

    def __init__(self, reads, nack_write=False):
        self.reads, self.writes, self.waited, self.nack_write = list(reads), [], 0, nack_write

    def write(self, addr, data):
        if self.nack_write:
            self.nack_write = False
            raise BusError("nack")
        self.writes.append(bytes(data))

    def read(self, addr, n):
        r = self.reads.pop(0)
        if isinstance(r, Exception):
            raise r
        return r

    def wait_ms(self, ms):
        self.waited += ms


OK_PING = build_response(I.ICD_CMD_PING, I.ICD_ST_OK)
BUSY_PING = build_response(I.ICD_CMD_PING, I.ICD_ST_BUSY)


def corrupt(f):
    return f[:3] + bytes([f[3] ^ 0xFF]) + f[4:]


def test_frame_format():
    f = build_command(I.ICD_CMD_SET_MODE, bytes([2]))
    c = crc16(f[:3])
    assert f == bytes([0x05, 1, 2, c & 0xFF, c >> 8])          # CRC low byte first
    r = parse_response(build_response(4, 0, b"\x01\x02"))
    assert (r.cmd, r.status, r.payload) == (4, 0, b"\x01\x02")
    with pytest.raises(Exception):
        parse_response(build_response(4, 0)[:31])                 # must be exactly 32 bytes
    with pytest.raises(ValueError):
        build_command(1, bytes(I.ICD_MAX_PAYLOAD + 1))


def test_busy_reread_without_resend():
    bus = ScriptBus([BUSY_PING, BUSY_PING, OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok
    assert len(bus.writes) == 1 and link.stats.busy_retries == 2
    assert bus.waited == I.ICD_TURNAROUND_MS + 2 * I.ICD_BUSY_RETRY_MS


def test_read_crc_error_rereads_and_bad_crc_status_resends_once():
    bus = ScriptBus([corrupt(OK_PING), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok and len(bus.writes) == 1
    bus = ScriptBus([build_response(I.ICD_CMD_PING, I.ICD_ST_BAD_CRC), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok and len(bus.writes) == 2


def test_comm_fault_once_and_recovery():
    bus = ScriptBus([BusError("x")] * 8 + [OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    for i in range(4):
        with pytest.raises(LinkError):
            link.transact(I.ICD_CMD_PING)
        assert link.stats.comm_fault == (i >= I.ICD_COMM_FAULT_THRESHOLD - 1)
    assert link.stats.comm_fault_events == 1
    link.transact(I.ICD_CMD_PING)
    assert not link.stats.comm_fault


def test_no_recovery_read_after_write_nack():
    bus = ScriptBus([OK_PING], nack_write=True)
    link = SlaveLink(bus, 0x32, "t")
    with pytest.raises(LinkError):
        link.transact(I.ICD_CMD_GET_STATUS)
    assert link.transact(I.ICD_CMD_PING).ok and not bus.reads


# ------------------------------------------------------------------ master against the C reference slave

class HostSlaveBus:
    def __init__(self, lib):
        self.lib, self.corrupt_reads, self.present = lib, 0, True

    def write(self, addr, data):
        if not self.present:
            raise BusError("nack")
        self.lib.hs_write(bytes(data), len(data))

    def read(self, addr, n):
        if not self.present:
            raise BusError("nack")
        buf = ctypes.create_string_buffer(n)
        self.lib.hs_read(buf, n)
        raw = buf.raw
        if self.corrupt_reads:
            self.corrupt_reads -= 1
            raw = corrupt(raw)
        return raw

    def wait_ms(self, ms):
        self.lib.hs_run_ms(int(round(ms)))


@pytest.fixture
def slave(tmp_path):
    if shutil.which("gcc") is None:
        pytest.skip("gcc not found")
    lib = _build(tmp_path, "slave", [ICD / "tests" / "host_slave.c", ICD / "slave" / "icd_node.c",
                                     ICD / "slave" / "icd_link.c", ICD / "icd_crc16.c"])
    lib.hs_overwritten.restype = ctypes.c_uint32
    lib.hs_init()
    bus = HostSlaveBus(lib)
    return lib, bus, SlaveLink(bus, I.ICD_ADDR_ADCS, "ref")


def test_ref_slave_boot_frame_and_common_commands(slave):
    lib, bus, link = slave
    r = parse_response(bus.read(0x32, I.ICD_RESP_FRAME_LEN))
    assert (r.cmd, r.status) == (I.ICD_CMD_ID_NONE, I.ICD_ST_NO_DATA)       # idle frame at boot
    assert link.transact(I.ICD_CMD_PING).ok
    v = link.call(I.ICD_CMD_GET_VERSION)
    assert list(v) == [9, 8, 7, I.ICD_VERSION_MAJOR, I.ICD_VERSION_MINOR]
    assert link.call(I.ICD_CMD_GET_TELEMETRY, b"\x00") == bytes([0xA0, 0xA1, 0xA2, 0xA3])
    assert link.transact(I.ICD_CMD_GET_TELEMETRY, b"\x05").status == I.ICD_ST_BAD_PARAM
    assert link.transact(0x7E).status == I.ICD_ST_UNKNOWN_CMD


def test_ref_slave_rejects_bad_frames_without_acting(slave):
    lib, bus, link = slave
    good = build_command(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_SAFE]))
    for frame, want in ((good[:-1] + bytes([good[-1] ^ 1]), I.ICD_ST_BAD_CRC),
                        (good[:2] + bytes([good[2] ^ 1]) + good[3:], I.ICD_ST_BAD_CRC),
                        (bytes([I.ICD_CMD_SET_MODE, 25]) + good[3:], I.ICD_ST_BAD_LENGTH),
                        (good + b"\x00", I.ICD_ST_BAD_LENGTH),
                        (bytes(29), I.ICD_ST_BAD_LENGTH)):                  # over-long write
        assert link.transact_raw(frame, frame[0]).status == want
    assert lib.hs_mode() == I.ICD_MODE_BOOT                                  # nothing executed


def test_ref_slave_busy_until_processed_and_newer_command_wins(slave):
    lib, bus, link = slave
    bus.write(0x32, build_command(I.ICD_CMD_PING))
    r = parse_response(bus.read(0x32, 32))
    assert (r.cmd, r.status) == (I.ICD_CMD_PING, I.ICD_ST_BUSY)              # not processed yet
    bus.write(0x32, build_command(I.ICD_CMD_GET_VERSION))                    # a newer command arrives
    lib.hs_run_ms(1)
    r = parse_response(bus.read(0x32, 32))
    assert r.cmd == I.ICD_CMD_GET_VERSION                                    # never the stale PING answer
    assert lib.hs_overwritten() == 1


def test_ref_slave_mode_rules(slave):
    lib, bus, link = slave
    assert link.transact(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_DETUMBLE])).status == I.ICD_ST_INVALID_MODE
    assert link.transact(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_SAFE])).ok
    assert link.transact(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_NOMINAL])).ok
    assert link.transact(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_BOOT])).status == I.ICD_ST_INVALID_MODE
    assert link.transact(I.ICD_CMD_SET_MODE, bytes([I.ICD_MODE_COUNT])).status == I.ICD_ST_INVALID_MODE
    assert lib.hs_mode() == I.ICD_MODE_NOMINAL


def test_ref_slave_status_flags(slave):
    lib, bus, link = slave
    c = SlaveClient(link)
    st = c.status()
    assert st.reset_occurred and st.flags & I.ICD_SF_RESERVED_MASK == 0
    assert not c.status().reset_occurred                                     # cleared once reported
    lib.hs_set_live_faults(0x0004)
    lib.hs_run_ms(5)
    lib.hs_set_live_faults(0)                                                # brief fault ...
    lib.hs_run_ms(5)
    st = c.status()
    assert st.faults == 0x0004 and not st.fault_active                       # ... still reported once
    assert c.status().faults == 0
    lib.hs_run_ms(I.ICD_NO_MASTER_TIMEOUT_MS + 10)
    assert c.status().no_master_contact


def test_ref_slave_lost_status_reply_is_recovered(slave):
    lib, bus, link = slave
    c = SlaveClient(link)
    c.last_uptime_s = 0                     # uptime alone cannot reveal anything here
    bus.corrupt_reads = 1 + I.ICD_BUSY_RETRIES
    with pytest.raises(LinkError):
        c.status()                          # the slave answered (and cleared RESET_OCCURRED) ...
    assert c.status().reset_occurred        # ... but the reply was recovered and merged
    assert link.stats.recovered == 1


def test_ref_slave_reset_seen_from_uptime(slave):
    lib, bus, link = slave
    c = SlaveClient(link)
    lib.hs_run_ms(5000)
    c.status()
    c.status()
    lib.hs_init()                           # reboot ...
    lib.hs_run_ms(1000)
    link.call(I.ICD_CMD_GET_STATUS)         # ... and someone else consumes the reset flag
    assert c.status().reset_occurred        # uptime went from 5 s back to 0
