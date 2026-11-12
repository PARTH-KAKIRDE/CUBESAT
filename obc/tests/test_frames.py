import subprocess
import sys

import pytest

from cubesat_obc import ground
from cubesat_obc import icd_defs as I
from cubesat_obc.frames import FrameError, build_command, build_response, crc16, parse_response

from .conftest import CODE


def test_crc_check_value():
    assert crc16(b"123456789") == I.ICD_CRC_CHECK == 0x29B1


def test_command_layout():
    f = build_command(I.ICD_CMD_SET_MODE, bytes([2]))
    assert f[:3] == bytes([0x05, 1, 2])
    c = crc16(f[:3])
    assert f[3] == c & 0xFF and f[4] == c >> 8  # low byte first
    assert len(build_command(I.ICD_CMD_PING)) == I.ICD_CMD_MIN_LEN
    assert len(build_command(1, bytes(24))) == I.ICD_CMD_MAX_LEN
    with pytest.raises(ValueError):
        build_command(1, bytes(25))


def test_response_roundtrip_and_rejects():
    raw = build_response(0x04, 0, b"\x01\x02\x03")
    assert len(raw) == 32 and raw[8:] == bytes(24)
    r = parse_response(raw)
    assert (r.cmd, r.status, r.payload) == (4, 0, b"\x01\x02\x03")
    bad = bytearray(raw)
    bad[4] ^= 1
    with pytest.raises(FrameError):
        parse_response(bytes(bad))
    bad = bytearray(raw)
    bad[2] = 30
    with pytest.raises(FrameError):
        parse_response(bytes(bad))


def test_generated_constants_up_to_date():
    r = subprocess.run([sys.executable, str(CODE / "tools" / "gen_icd_py.py"), "--check"], capture_output=True)
    assert r.returncode == 0, r.stdout.decode()


def test_generator_rejects_lines_it_cannot_parse(tmp_path):
    """A #define the generator cannot read must stop it, not vanish from Python."""
    (tmp_path / "tools").mkdir()
    (tmp_path / "icd").mkdir()
    (tmp_path / "obc" / "cubesat_obc").mkdir(parents=True)
    (tmp_path / "tools" / "gen_icd_py.py").write_bytes((CODE / "tools" / "gen_icd_py.py").read_bytes())
    for h in ("cubesat_icd.h", "ground_link.h"):
        (tmp_path / "icd" / h).write_bytes((CODE / "icd" / h).read_bytes())
    gen = [sys.executable, "-I", str(tmp_path / "tools" / "gen_icd_py.py")]
    assert subprocess.run(gen, capture_output=True).returncode == 0
    for bad in ("#define BAD_A 5 // comment", "#define BAD_B 0x10UL", "#define BAD_C (1u << 3)",
                "#define BAD_D (-1)"):
        h = tmp_path / "icd" / "ground_link.h"
        good = h.read_text()
        h.write_text(good + bad + "\n")
        r = subprocess.run(gen, capture_output=True)
        assert r.returncode != 0 and b"cannot parse" in r.stdout + r.stderr, bad
        h.write_text(good)


KEY = bytes(range(32))


def test_ground_cmd_roundtrip():
    p = ground.build_cmd(9, I.GL_TGT_ADCS, I.ADCS_CMD_SET_BDOT_GAIN, b"\x01\x02\x03\x04", key=KEY, counter=77)
    assert len(p) <= I.GL_MAX_LEN and ground.check(p)
    c = ground.parse_cmd(p, KEY)
    assert (c.seq, c.target, c.cmd, c.args, c.counter) == (9, 1, 0x20, b"\x01\x02\x03\x04", 77)
    with pytest.raises(ground.PacketError):
        ground.parse_cmd(p[:-1] + bytes([p[-1] ^ 1]), KEY)          # CRC
    with pytest.raises(ground.PacketError):
        ground.build_cmd(1, 0, 1, bytes(I.GL_CMD_MAX_ARGS + 1), key=KEY, counter=1)
    # The largest command still fits in one packet.
    assert len(ground.build_cmd(1, 0, 1, bytes(I.GL_CMD_MAX_ARGS), key=KEY, counter=1)) == I.GL_MAX_LEN


def _reseal(body: bytes) -> bytes:
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def test_ground_cmd_authentication():
    p = ground.build_cmd(9, I.GL_TGT_EPS, I.GL_EPS_PYRO_ARM, (I.GL_PYRO_KEY).to_bytes(4, "little"),
                         key=KEY, counter=1000)
    with pytest.raises(ground.PacketError, match="MAC"):
        ground.parse_cmd(p, bytes(32))                                # wrong key
    # Tamper with the args / counter and fix up the CRC: the MAC still catches it.
    for i in (I.GL_CMD_OFS_ARGS, I.GL_CMD_OFS_ARGS + 4, I.GL_CMD_OFS_TARGET, I.GL_OFS_SEQ):
        b = bytearray(p[:-2])
        b[i] ^= 0x01
        with pytest.raises(ground.PacketError, match="MAC"):
            ground.parse_cmd(_reseal(bytes(b)), KEY)
    # An old (v1) unauthenticated command is rejected.
    v1 = _reseal(bytes([I.GL_SAT_ID, I.GL_T_CMD, 1, I.GL_TGT_OBC, I.GL_OBC_PING, 0]))
    with pytest.raises(ground.PacketError):
        ground.parse_cmd(v1, KEY)


def test_ground_key_file(tmp_path):
    f = tmp_path / "k"
    f.write_text(KEY.hex() + "\n")
    assert ground.load_key(f) == KEY
    f.write_text("abcd")
    with pytest.raises(ground.PacketError):
        ground.load_key(f)                                           # too short
    f.write_text("not hex at all, not hex at all!!")
    with pytest.raises(ground.PacketError):
        ground.load_key(f)


def test_ack_split_and_reassemble():
    data = bytes(range(24))
    pkts = ground.build_ack(3, I.GL_TGT_ADCS, I.ICD_CMD_GET_TELEMETRY, 0, data)
    assert len(pkts) == 2 and all(len(p) <= I.GL_MAX_LEN for p in pkts)
    decoded = [ground.parse_downlink(p) for p in pkts]
    assert ground.reassemble(decoded) == data


def test_obc_beacon_fits():
    b = ground.build_obc_beacon(1, 100, 3, 7400, -120, 2, 3, 1234, 0, 45, 999)
    assert len(b) == I.GL_BO_LEN
    d = ground.parse_downlink(b)
    assert d["vbatt_mV"] == 7400 and d["ibus_mA"] == -120 and d["adcs_rate_dps"] == 12.34


def test_hw_link_test_script_in_sim(sim_lib):
    """Deliverable 5 script, run against the simulation (-n 2000 to keep it quick).
    sim_lib makes sure the simulation library is built first (clean checkout)."""
    r = subprocess.run([sys.executable, str(CODE / "obc" / "hw_tests" / "link_test.py"), "--sim", "-n", "2000"],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout[-2000:]
    assert "9/9 passed" in r.stdout
