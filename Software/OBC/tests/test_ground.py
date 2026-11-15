"""Ground-link codec, and the ICD frame helpers the OBC uses (from ICD/python/icd_link.py)."""
import subprocess
import sys

import pytest

import icd_defs as I  # noqa: E402
from icd_link import FrameError, build_command, build_response, crc16, parse_response  # noqa: E402
from OBC.comms import ground  # noqa: E402

from .conftest import SOFTWARE


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


def test_key_file_readable_by_others_is_flagged(tmp_path, caplog):
    import os
    f = tmp_path / "k"
    f.write_text(KEY.hex())
    os.chmod(f, 0o644)
    assert ground.load_key(f) == KEY and "chmod 600" in caplog.text
    caplog.clear()
    os.chmod(f, 0o600)
    ground.load_key(f)
    assert "chmod 600" not in caplog.text


def test_counter_store_never_reuses_a_counter(tmp_path):
    t = [1_800_000_000.0]
    c = ground.CounterStore(tmp_path / "obc_ctr", clock=lambda: t[0])
    a, b = c.next(), c.next()
    assert a == 1_800_000_000 and b == a + 1                 # same second: still higher
    t[0] -= 3600                                             # PC clock set back an hour
    assert c.next() == b + 1
    c2 = ground.CounterStore(tmp_path / "obc_ctr", clock=lambda: t[0])   # restart: from the file
    assert c2.next() == b + 2 and c2.last() == b + 2
    t[0] += 7200
    assert c2.next() == 1_800_003_600                        # clock ahead again: jumps up
    lost = ground.CounterStore(tmp_path / "gone", clock=lambda: t[0])
    assert lost.next() == 1_800_003_600 > 0                  # lost file: Unix time, not 1
    assert not list(tmp_path.glob("*.tmp"))
    (tmp_path / "bad").write_text("x")
    with pytest.raises(ground.PacketError):
        ground.CounterStore(tmp_path / "bad").next()


def test_keygen_writes_two_private_keys_outside_the_repo(tmp_path):
    import importlib.util
    import os
    spec = importlib.util.spec_from_file_location("keygen", SOFTWARE / "OBC" / "scripts" / "keygen.py")
    kg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(kg)
    d = tmp_path / "keys"
    assert kg.main(["--dir", str(d)]) == 0
    k1, k2 = ground.load_key(d / "ground_key"), ground.load_key(d / "ttc_key")
    assert len(k1) == len(k2) == 32 and k1 != k2
    assert (d / "ttc_key").stat().st_mode & 0o777 == 0o600 and d.stat().st_mode & 0o777 == 0o700
    assert kg.main(["--dir", str(d)]) == 1                   # no silent overwrite
    assert ground.load_key(d / "ground_key") == k1
    assert kg.main(["--dir", str(d), "--force"]) == 0 and ground.load_key(d / "ground_key") != k1
    assert kg.main(["--dir", str(SOFTWARE / "keys_here")]) == 1   # never inside the public repo
    assert not (SOFTWARE / "keys_here").exists()


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


def test_icd_link_test_script_in_sim(sim_lib):
    """The bench link test (ICD/python/test_link.py) against the simulated ADCS."""
    r = subprocess.run([sys.executable, "-B", str(SOFTWARE / "ICD" / "python" / "test_link.py"),
                        "--sim-lib", str(sim_lib.path), "-n", "2000"], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr[-2000:]


def test_ecmd_is_never_taken_for_a_cmd():
    """GL v4: an emergency command has its own TYPE inside the MAC, so it can never
    be replayed to the OBC as an ordinary command (or the other way round)."""
    e = ground.build_ecmd(3, I.GL_TGT_OBC, I.GL_E_OBC_RESET, b"\x42\x4f\x4f\x54", key=KEY, counter=9)
    c = ground.build_cmd(3, I.GL_TGT_OBC, I.GL_E_OBC_RESET, b"\x42\x4f\x4f\x54", key=KEY, counter=9)
    assert e[I.GL_OFS_TYPE] == I.GL_T_ECMD and len(e) == len(c) and e[-6:-2] != c[-6:-2]
    with pytest.raises(ground.PacketError):
        ground.parse_cmd(e, KEY)
    with pytest.raises(ground.PacketError):
        ground.build_cmd(1, 0, 1, b"", key=KEY, counter=1, ptype=I.GL_T_ACK)
    ack = ground._seal(bytes([I.GL_SAT_ID, I.GL_T_ACK, 3, I.GL_TGT_OBC | I.GL_TGT_EMERGENCY, 1, 0, 0]))
    d = ground.parse_downlink(ack)
    assert d["emergency"] and d["target"] == I.GL_TGT_OBC
    with pytest.raises(ground.PacketError):
        ground.parse_eping(b"\x00" * 3)
