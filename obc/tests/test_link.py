"""SlaveLink retry/fault behaviour against a scripted fake bus."""
import pytest

from cubesat_obc import icd_defs as I
from cubesat_obc.bus import BusError
from cubesat_obc.frames import build_response
from cubesat_obc.link import LinkError, SlaveLink


class ScriptBus:
    """Each read pops the next scripted frame (or raises if it is an exception)."""

    def __init__(self, reads):
        self.reads = list(reads)
        self.writes = []
        self.waited = 0

    def write(self, addr, data):
        if self.reads and isinstance(self.reads[0], BusError) and self.reads[0].args[0] == "nack-write":
            raise self.reads.pop(0)
        self.writes.append(bytes(data))

    def read(self, addr, n):
        r = self.reads.pop(0)
        if isinstance(r, Exception):
            raise r
        return r

    def write_read(self, addr, data, n):
        raise NotImplementedError

    def wait_ms(self, ms):
        self.waited += ms


OK_PING = build_response(I.ICD_CMD_PING, I.ICD_ST_OK)
BUSY_PING = build_response(I.ICD_CMD_PING, I.ICD_ST_BUSY)


def corrupt(f):
    b = bytearray(f)
    b[3] ^= 0xFF
    return bytes(b)


def test_busy_is_retried():
    bus = ScriptBus([BUSY_PING, BUSY_PING, OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok
    assert link.stats.busy_retries == 2
    assert len(bus.writes) == 1                # command written once only
    assert bus.waited == I.ICD_TURNAROUND_MS + 2 * I.ICD_BUSY_RETRY_MS


def test_busy_forever_fails_after_three_retries():
    bus = ScriptBus([BUSY_PING] * 4)
    link = SlaveLink(bus, 0x32, "t")
    with pytest.raises(LinkError):
        link.transact(I.ICD_CMD_PING)
    assert not bus.reads                       # 1 read + 3 retries


def test_read_crc_error_rereads_without_resending():
    bus = ScriptBus([corrupt(OK_PING), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok
    assert len(bus.writes) == 1 and link.stats.read_crc_errors == 1


def test_slave_bad_crc_is_resent_once():
    bus = ScriptBus([build_response(I.ICD_CMD_PING, I.ICD_ST_BAD_CRC), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).ok
    assert len(bus.writes) == 2 and link.stats.resends == 1


def test_wrong_echo_is_not_accepted():
    bus = ScriptBus([build_response(I.ICD_CMD_GET_STATUS, 0, bytes(8)), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    assert link.transact(I.ICD_CMD_PING).cmd == I.ICD_CMD_PING


def test_comm_fault_after_three_failures_and_recovery():
    # 1 failed read, then (recovery read + failed read) x 3, then recovery read + OK
    bus = ScriptBus([BusError("x")] * 8 + [OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    for i in range(4):
        with pytest.raises(LinkError):
            link.transact(I.ICD_CMD_PING)
        assert link.stats.comm_fault == (i >= 2)
    assert link.stats.comm_fault_events == 1    # logged once, not every poll
    link.transact(I.ICD_CMD_PING)
    assert not link.stats.comm_fault and link.stats.consecutive_failures == 0


# ---- lost replies (the slave acted, but the Pi never read the answer)

STATUS_RESET = build_response(I.ICD_CMD_GET_STATUS, I.ICD_ST_OK,
                              bytes([1, I.ICD_SF_RESET_OCCURRED, 0x01, 0, 5, 0, 0, 0]))


def test_lost_reply_is_recovered_before_the_next_command():
    # GET_STATUS: written, all 4 reads corrupt. Next transaction first re-reads
    # the held reply, then runs normally.
    bus = ScriptBus([corrupt(STATUS_RESET)] * 4 + [STATUS_RESET, OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    with pytest.raises(LinkError):
        link.transact(I.ICD_CMD_GET_STATUS)
    assert link.transact(I.ICD_CMD_PING).ok
    assert link.recovered[I.ICD_CMD_GET_STATUS].payload[1] == I.ICD_SF_RESET_OCCURRED
    assert link.stats.recovered == 1 and len(bus.writes) == 2   # nothing re-sent


def test_recovery_ignores_a_different_frame():
    # After a slave reset the held frame is the idle frame (cmd 0): not ours.
    idle = build_response(I.ICD_CMD_ID_NONE, I.ICD_ST_NO_DATA)
    bus = ScriptBus([BusError("x"), idle, OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    with pytest.raises(LinkError):
        link.transact(I.ICD_CMD_GET_STATUS)
    assert link.transact(I.ICD_CMD_PING).ok
    assert not link.recovered and link.stats.recovered == 0


def test_no_recovery_read_after_a_write_nack():
    # The slave never got the command, so there is nothing to recover.
    bus = ScriptBus([BusError("nack-write"), OK_PING])
    link = SlaveLink(bus, 0x32, "t")
    with pytest.raises(LinkError):
        link.transact(I.ICD_CMD_GET_STATUS)
    assert link.transact(I.ICD_CMD_PING).ok and not bus.reads


def test_status_merges_lost_reset_flag_and_faults():
    from cubesat_obc.slaves import SlaveClient
    later = build_response(I.ICD_CMD_GET_STATUS, I.ICD_ST_OK, bytes([1, 0, 0, 0, 6, 0, 0, 0]))
    bus = ScriptBus([corrupt(STATUS_RESET)] * 4 + [STATUS_RESET, later])
    c = SlaveClient(SlaveLink(bus, 0x32, "t"))
    with pytest.raises(LinkError):
        c.status()
    st = c.status()
    assert st.reset_occurred and st.faults == 0x0001 and st.uptime_s == 6


def test_status_detects_reset_from_uptime():
    from cubesat_obc.slaves import SlaveClient

    def st(uptime):
        return build_response(I.ICD_CMD_GET_STATUS, I.ICD_ST_OK, bytes([1, 0, 0, 0]) + uptime.to_bytes(4, "little"))
    c = SlaveClient(SlaveLink(ScriptBus([st(100), st(101), st(3)]), 0x32, "t"))
    assert not c.status().reset_occurred
    assert not c.status().reset_occurred
    assert c.status().reset_occurred        # 101 s -> 3 s: it rebooted
