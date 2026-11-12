"""End-to-end: the OBC application against the real firmware logic in simulation."""
import itertools
import json
import struct
from pathlib import Path

from cubesat_obc import ground
from cubesat_obc import icd_defs as I
from cubesat_obc.bus import SimBus
from cubesat_obc.factory import build_sim
from cubesat_obc.link import SlaveLink
from cubesat_obc.slaves import AdcsClient, TtcClient

from .conftest import TEST_KEY

_counter = itertools.count(1)


def cmd(seq, target, c, args=b"", key=TEST_KEY, counter=None):
    """A signed ground command with a fresh (increasing) counter."""
    return ground.build_cmd(seq, target, c, args, key=key, counter=counter or next(_counter))


def run(app, ticks):
    for _ in range(ticks):
        app.tick()
        app.clock.sleep(1.0)


def downlink(sim):
    return [ground.parse_downlink(p) for p in sim.downlink()]


def acks_for(packets, seq):
    return [p for p in packets if p["type"] in ("ACK", "TLM") and p["seq"] == seq]


def uplink_and_wait(app, sim, pkt, ticks=4):
    sim.uplink(pkt, -95)
    out = []
    for _ in range(ticks):
        run(app, 1)
        out += downlink(sim)
    return out


def test_start_and_poll(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 5)
    st = app.last["adcs_status"]
    assert st is not None and st.mode == I.ICD_MODE_DETUMBLE
    assert app.last["ttc_status"].mode == I.ICD_MODE_NOMINAL
    assert app.eps.ok and 7000 < app.last["eps"]["vbus_mV"] < 8000
    assert app.last["lux"] and app.last["lux"] > 0
    files = list(Path(cfg["obc"]["data_dir"], "telemetry").glob("*.jsonl"))
    rec = json.loads(files[0].read_text().splitlines()[-1])
    assert rec["adcs"]["status"]["mode"] == "DETUMBLE"
    assert rec["adcs"]["sensors"]["mag_stale"] is False
    assert rec["links"]["adcs"]["fail"] == 0 and rec["links"]["ttc"]["fail"] == 0


def test_ground_ping_hk_and_forwarding(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.downlink()

    out = uplink_and_wait(app, sim, cmd(1, I.GL_TGT_OBC, I.GL_OBC_PING))
    a = acks_for(out, 1)
    assert a and a[0]["status"] == I.ICD_ST_OK

    out = uplink_and_wait(app, sim, cmd(2, I.GL_TGT_OBC, I.GL_OBC_GET_HK))
    hk = ground.reassemble(acks_for(out, 2))
    assert hk is not None and len(hk) == I.GL_HK_LEN
    assert 7000 < struct.unpack_from("<H", hk, I.GL_HK_OFS_VBATT)[0] < 8000

    # Forwarded ICD command to ADCS, then read back with GET_CONFIG.
    out = uplink_and_wait(app, sim, cmd(3, I.GL_TGT_ADCS, I.ADCS_CMD_SET_BDOT_GAIN,
                                                     struct.pack("<I", 777000)))
    assert acks_for(out, 3)[0]["status"] == I.ICD_ST_OK
    out = uplink_and_wait(app, sim, cmd(4, I.GL_TGT_ADCS, I.ADCS_CMD_GET_CONFIG))
    conf = ground.reassemble(acks_for(out, 4))
    assert struct.unpack_from("<I", conf, I.ADCS_CFG_OFS_GAIN)[0] == 777000

    # 24-byte telemetry page comes down as ACK + TLM and reassembles.
    out = uplink_and_wait(app, sim, cmd(5, I.GL_TGT_OBC, I.GL_OBC_GET_SLAVE_TLM,
                                                     bytes([I.GL_TGT_TTC, I.TTC_TLM_PAGE_RADIO])))
    page = ground.reassemble(acks_for(out, 5))
    assert page is not None and len(page) == I.TTC_TLM0_LEN

    # Garbage and foreign packets are ignored (no ACK).
    out = uplink_and_wait(app, sim, b"\xC5\x01\x06garbage!!", ticks=2)
    assert not acks_for(out, 6)


def test_duplicate_ground_command_not_reexecuted(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    pkt = cmd(9, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE)
    a1 = acks_for(uplink_and_wait(app, sim, pkt), 9)
    a2 = acks_for(uplink_and_wait(app, sim, pkt), 9)
    assert a1 and a2 and a1[0]["data"] == a2[0]["data"]
    assert app.images == 1
    assert len(list(Path(cfg["obc"]["data_dir"], "images").glob("*.jpg"))) == 1


def test_pyro_refused_when_disabled(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    out = uplink_and_wait(app, sim, cmd(10, I.GL_TGT_EPS, I.GL_EPS_PYRO_ARM,
                                                     struct.pack("<I", I.GL_PYRO_KEY)))
    assert acks_for(out, 10)[0]["status"] == I.GL_ST_REFUSED
    assert not app.pyro.out.state


def test_pyro_arm_fire_when_enabled(cfg, sim_lib):
    cfg["pyro"]["enabled"] = True
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    out = uplink_and_wait(app, sim, cmd(11, I.GL_TGT_EPS, I.GL_EPS_PYRO_FIRE, struct.pack("<H", 50)))
    assert acks_for(out, 11)[0]["status"] == I.GL_ST_REFUSED  # not armed
    uplink_and_wait(app, sim, cmd(12, I.GL_TGT_EPS, I.GL_EPS_PYRO_ARM, struct.pack("<I", I.GL_PYRO_KEY)))
    out = uplink_and_wait(app, sim, cmd(13, I.GL_TGT_EPS, I.GL_EPS_PYRO_FIRE, struct.pack("<H", 50)))
    assert acks_for(out, 13)[0]["status"] == I.ICD_ST_OK
    assert app.pyro.fire_count == 1


def test_low_battery_goes_safe_and_recovers(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    ina = app.eps.ina.bus.fakes[I.ICD_ADDR_INA226_EPS]
    app.start()
    run(app, 2)
    ina.vbus_mv = 6400
    run(app, 2)
    assert app.mode == I.ICD_MODE_SAFE
    assert sim.lib.sim_adcs_mode() == I.ICD_MODE_SAFE and sim.lib.sim_ttc_mode() == I.ICD_MODE_SAFE
    ina.vbus_mv = 6800  # inside the hysteresis band: stay SAFE
    run(app, 2)
    assert app.mode == I.ICD_MODE_SAFE
    ina.vbus_mv = 7500
    run(app, 2)
    assert app.mode == I.ICD_MODE_NOMINAL
    assert sim.lib.sim_adcs_mode() == I.ICD_MODE_DETUMBLE and sim.lib.sim_ttc_mode() == I.ICD_MODE_NOMINAL


def test_slave_reset_reapplies_config(cfg, sim_lib):
    cfg["adcs"]["bdot_gain"] = 424242
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)  # watchdog reset: defaults again
    probe = AdcsClient(SlaveLink(SimBus(sim), I.ICD_ADDR_ADCS, "probe"))
    assert probe.config()["gain"] == I.ADCS_BDOT_GAIN_DEFAULT
    run(app, 2)                                # OBC sees RESET_OCCURRED
    assert probe.config()["gain"] == 424242


def test_comm_fault_and_beacon_flags(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.lib.sim_set_present(I.ICD_ADDR_ADCS, 0)
    run(app, 3)
    assert app.adcs.link.stats.comm_fault
    assert app.comm_flags() & I.GL_CF_ADCS_COMM_FAULT
    sim.lib.sim_set_present(I.ICD_ADDR_ADCS, 1)
    run(app, 1)
    assert not app.adcs.link.stats.comm_fault
    # The OBC beacon reaches the ground through TT&C.
    sim.downlink()
    run(app, 35)
    beacons = [p for p in downlink(sim) if p["type"] == "BEACON_OBC"]
    assert beacons and beacons[-1]["ttc_mode"] == I.ICD_MODE_NOMINAL


def test_adcs_promoted_to_nominal_when_detumbled(cfg, sim_lib):
    cfg["adcs"]["dwell_s"] = 5
    app, sim = build_sim(cfg, sim=sim_lib)
    sim.lib.sim_adcs_set_rate_dps(0.3, -0.2, 0.1)  # already slow
    app.start()
    run(app, 12)
    assert sim.lib.sim_adcs_mode() == I.ICD_MODE_NOMINAL


# ---------------------------------------------------------------- ground-link security

def test_unsigned_forged_and_replayed_commands_are_ignored(cfg, sim_lib):
    cfg["pyro"]["enabled"] = True
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    arm = struct.pack("<I", I.GL_PYRO_KEY)
    # Signed with the wrong key: no ACK, pyro not armed.
    out = uplink_and_wait(app, sim, cmd(20, I.GL_TGT_EPS, I.GL_EPS_PYRO_ARM, arm, key=bytes(32)))
    assert not acks_for(out, 20) and app.pyro.armed_until == 0.0
    # Genuine command executes.
    genuine = cmd(21, I.GL_TGT_EPS, I.GL_EPS_PYRO_ARM, arm, counter=5_000_000)
    assert acks_for(uplink_and_wait(app, sim, genuine), 21)[0]["status"] == I.ICD_ST_OK
    # An older counter (a recording of an earlier command) is ignored.
    old = cmd(22, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE, counter=4_999_999)
    assert not acks_for(uplink_and_wait(app, sim, old), 22) and app.images == 0
    # The counter survives an OBC restart (same data dir): the old packet is still refused.
    app2, sim2 = build_sim(cfg, sim=sim_lib)
    app2.start()
    run(app2, 2)
    replay = cmd(23, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE, counter=4_000_000)
    assert not acks_for(uplink_and_wait(app2, sim2, replay), 23) and app2.images == 0


def test_no_key_file_rejects_every_command(cfg, sim_lib):
    cfg["ground"]["key_file"] = cfg["obc"]["data_dir"] + "/missing_key"
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    assert not acks_for(uplink_and_wait(app, sim, cmd(30, I.GL_TGT_OBC, I.GL_OBC_PING)), 30)


# ---------------------------------------------------------------- forwarding allowlist

def test_forwarding_allowlist(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.downlink()
    # TT&C queue/radio commands are refused and never reach the slave.
    for seq, (c, a) in enumerate([(I.TTC_CMD_GET_RX, b""), (I.TTC_CMD_QUEUE_TX, b"\x01"),
                                  (I.TTC_CMD_SET_BEACON, b"\x01"),
                                  (I.TTC_CMD_SET_RADIO, bytes(8))], start=40):
        out = uplink_and_wait(app, sim, cmd(seq, I.GL_TGT_TTC, c, a))
        assert acks_for(out, seq)[0]["status"] == I.GL_ST_REFUSED, hex(c)
    # Allowed read-only command goes through.
    out = uplink_and_wait(app, sim, cmd(50, I.GL_TGT_TTC, I.TTC_CMD_GET_RADIO))
    radio = ground.reassemble(acks_for(out, 50))
    assert struct.unpack_from("<I", radio, I.TTC_RADIO_OFS_FREQ_HZ)[0] == I.TTC_FREQ_DEFAULT_HZ


def test_ground_get_status_does_not_hide_a_reset(cfg, sim_lib):
    cfg["adcs"]["bdot_gain"] = 424242
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.downlink()
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)
    # The ground asks for ADCS status before the OBC's next poll ...
    sim.uplink(cmd(60, I.GL_TGT_ADCS, I.ICD_CMD_GET_STATUS), -95)
    run(app, 4)
    a = [p for p in downlink(sim) if p["type"] == "ACK" and p["seq"] == 60]
    assert a and a[0]["status"] == I.ICD_ST_OK
    # ... and the OBC still saw the reset and re-applied its configuration.
    probe = AdcsClient(SlaveLink(SimBus(sim), I.ICD_ADDR_ADCS, "probe"))
    assert probe.config()["gain"] == 424242


def test_burn_wire_refused_in_safe_mode(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    app.set_obc_mode(I.ICD_MODE_SAFE, "test")
    uplink_and_wait(app, sim, cmd(70, I.GL_TGT_TTC, I.TTC_CMD_BURN_ARM, struct.pack("<I", I.TTC_BURN_ARM_KEY)))
    # TT&C in SAFE refuses QUEUE_TX, so this ACK cannot go down; check the OBC's answer.
    (status, _), = [v for k, v in app.ack_cache.items() if k[1] == 70]
    assert status == I.GL_ST_REFUSED
    probe = TtcClient(SlaveLink(SimBus(sim), I.ICD_ADDR_TTC, "probe"))
    assert not probe.radio()["burn_armed"]


def test_ground_setting_survives_slave_reset(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    out = uplink_and_wait(app, sim, cmd(80, I.GL_TGT_ADCS, I.ADCS_CMD_SET_BDOT_GAIN, struct.pack("<I", 555000)))
    assert acks_for(out, 80)[0]["status"] == I.ICD_ST_OK
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)
    run(app, 2)
    probe = AdcsClient(SlaveLink(SimBus(sim), I.ICD_ADDR_ADCS, "probe"))
    assert probe.config()["gain"] == 555000            # not reverted to obc.toml


def test_eps_get_layout(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    out = uplink_and_wait(app, sim, cmd(90, I.GL_TGT_EPS, I.GL_EPS_GET))
    d = ground.reassemble(acks_for(out, 90))
    assert len(d) == I.GL_EPS_LEN
    assert 7000 < struct.unpack_from("<H", d, I.GL_EPS_OFS_VBATT)[0] < 8000
    assert d[I.GL_EPS_OFS_LOW_BATT] == 0 and d[I.GL_EPS_OFS_PYRO_FIRES] == 0


# ---------------------------------------------------------------- lost replies on the real firmware

class LossyBus:
    """SimBus that corrupts the next N reads from one address."""

    def __init__(self, inner, addr):
        self.inner, self.addr, self.corrupt_reads = inner, addr, 0

    def write(self, addr, data):
        self.inner.write(addr, data)

    def read(self, addr, n):
        r = self.inner.read(addr, n)
        if addr == self.addr and self.corrupt_reads:
            self.corrupt_reads -= 1
            r = bytes([r[0], r[1], r[2] ^ 0x55]) + r[3:]
        return r

    def write_read(self, addr, data, n):
        return self.inner.write_read(addr, data, n)

    def wait_ms(self, ms):
        self.inner.wait_ms(ms)


def test_lost_status_reply_after_reset_still_reconfigures(cfg, sim_lib):
    """The firmware clears RESET_OCCURRED when it answers; the answer is lost on
    the bus. The OBC must still notice the reset and re-send its configuration."""
    cfg["adcs"]["bdot_gain"] = 424242
    sim_lib.init(1)
    lossy = LossyBus(SimBus(sim_lib), I.ICD_ADDR_ADCS)
    from cubesat_obc.factory import SimClock, make_pyro
    from cubesat_obc.obc_app import ObcApp
    from cubesat_obc.eps import FakeINA226
    from cubesat_obc.sensors import FakeTSL2591
    sensors = SimBus(sim_lib, {I.ICD_ADDR_INA226_EPS: FakeINA226(), I.ICD_ADDR_TSL2591: FakeTSL2591()})
    app = ObcApp(cfg, lossy, sensors, make_pyro(cfg, dry_run=True), None, SimClock(sim_lib))
    app.start()
    run(app, 3)
    sim_lib.lib.sim_reset_slave(I.ICD_ADDR_ADCS)
    sim_lib.run_ms(2000)              # it has been up a while, so uptime alone would not show it ...
    app.adcs.last_uptime_s = 0        # ... make sure only the recovery path can catch it
    lossy.corrupt_reads = 1 + I.ICD_BUSY_RETRIES   # every read of the next GET_STATUS
    run(app, 3)
    assert app.adcs.link.stats.recovered >= 1
    probe = AdcsClient(SlaveLink(SimBus(sim_lib), I.ICD_ADDR_ADCS, "probe"))
    assert probe.config()["gain"] == 424242
