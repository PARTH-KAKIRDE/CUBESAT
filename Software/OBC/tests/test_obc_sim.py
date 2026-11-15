"""End-to-end: the OBC application against the real firmware logic in simulation."""
import itertools
import json
import struct
import time
from pathlib import Path

import icd_defs as I  # noqa: E402
from icd_link import SlaveLink  # noqa: E402
from OBC.comms import ground  # noqa: E402
from OBC.comms.bus import SimBus  # noqa: E402
from OBC.EPS.EPS import decode_state  # noqa: E402
from OBC.factory import build_sim  # noqa: E402
from OBC.comms.slaves import AdcsClient, TtcClient  # noqa: E402

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
    run(app, 12)
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


def finish_capture(app, timeout=10.0):
    """The camera is a real child process: give it real time to finish."""
    end = time.monotonic() + timeout
    while app.payload.cam.busy and time.monotonic() < end:
        app.tick()
        time.sleep(0.02)
    assert not app.payload.cam.busy


def test_duplicate_ground_command_not_reexecuted(cfg, sim_lib):
    cfg["payload"] = {"camera": {"period_s": 0}}          # only ground-commanded images
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    pkt = cmd(9, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE)
    a1 = acks_for(uplink_and_wait(app, sim, pkt), 9)
    finish_capture(app)
    a2 = acks_for(uplink_and_wait(app, sim, pkt), 9)
    finish_capture(app)
    assert a1 and a2 and a1[0]["data"] == a2[0]["data"] and a1[0]["status"] == I.ICD_ST_OK
    assert app.payload.images == 1
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
    assert app.mode != I.ICD_MODE_SAFE            # a 2-poll dip is a load spike (EPS low_hold_polls = 3)
    run(app, 1)
    assert app.mode == I.ICD_MODE_SAFE
    assert sim.lib.sim_adcs_mode() == I.ICD_MODE_SAFE and sim.lib.sim_ttc_mode() == I.ICD_MODE_SAFE
    ina.vbus_mv = 6800  # inside the hysteresis band: stay SAFE
    run(app, 2)
    assert app.mode == I.ICD_MODE_SAFE
    ina.vbus_mv = 7500
    run(app, 9)
    assert app.mode == I.ICD_MODE_SAFE            # recovery must last 10 polls (recover_hold_polls)
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
    assert not acks_for(uplink_and_wait(app, sim, old), 22) and app.payload.images == 0
    # The counter survives an OBC restart (same data dir): the old packet is still refused.
    app2, sim2 = build_sim(cfg, sim=sim_lib)
    app2.start()
    run(app2, 2)
    replay = cmd(23, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE, counter=4_000_000)
    assert not acks_for(uplink_and_wait(app2, sim2, replay), 23) and app2.payload.images == 0


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
    assert len(radio) == I.TTC_RADIO_LEN
    assert struct.unpack_from("<I", radio, I.TTC_RADIO_OFS_FREQ_DOWN_HZ)[0] == I.TTC_FREQ_DOWN_DEFAULT_HZ
    assert struct.unpack_from("<I", radio, I.TTC_RADIO_OFS_FREQ_UP_HZ)[0] == I.TTC_FREQ_UP_DEFAULT_HZ
    assert radio[I.TTC_RADIO_OFS_CR] == I.TTC_CR_DEFAULT == 8


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
    from OBC.factory import SimClock, make_pyro, sim_sensors
    from OBC.obc_app import ObcApp
    sensors = SimBus(sim_lib, sim_sensors(cfg))
    cfg["payload"] = {"camera": {"enabled": False}}
    app = ObcApp(cfg, lossy, sensors, make_pyro(cfg, dry_run=True), SimClock(sim_lib))
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


# ---------------------------------------------------------------- payload and EPS (v0.4)

def test_payload_logs_only_clean_magnetometer_samples(cfg, sim_lib):
    """ADCS takes every magnetometer sample with the coils off; the OBC logs each
    fresh sample once. Checked through DETUMBLE (B-dot drives the coils) and
    NOMINAL with a manual coil command."""
    cfg["adcs"]["dwell_s"] = 30
    cfg["payload"] = {"camera": {"enabled": False}}
    app, sim = build_sim(cfg, sim=sim_lib)
    sim.lib.sim_adcs_set_rate_dps(1.2, -0.9, 0.6)
    app.start()
    modes, manual = set(), False
    for _ in range(300):
        app.tick()
        st = app.last["adcs_status"]
        if st is not None:
            modes.add(st.mode)
            if st.mode == I.ICD_MODE_NOMINAL and not manual:
                assert app.adcs.mtq_manual(800, -800, 500, 10000) == I.ICD_ST_OK
                manual = True
        app.clock.sleep(1.0)
    app.close()
    assert {I.ICD_MODE_DETUMBLE, I.ICD_MODE_NOMINAL} <= modes and manual
    recs = [json.loads(x) for f in Path(cfg["obc"]["data_dir"], "payload").glob("*.jsonl")
            for x in f.read_text().splitlines()]
    mags = [r["mag"] for r in recs if "mag" in r]
    ts = [m["adcs_sample_ms"] for m in mags]
    assert len(mags) >= 280 and len(ts) == len(set(ts))
    assert all(m["age_ms"] <= cfg["adcs"]["period_ms"] + 500 for m in mags)
    assert sim.lib.sim_adcs_dirty_mag_samples() == 0          # no sample with a coil driven


def test_eps_get_state_over_the_ground_link(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 25)                                 # ADCS sun sensors read on the slow poll
    out = uplink_and_wait(app, sim, cmd(91, I.GL_TGT_EPS, I.GL_EPS_GET_STATE))
    parts = acks_for(out, 91)
    assert len(parts) == 2                       # ACK (15 bytes) + one TLM part (9)
    d = decode_state(ground.reassemble(parts))
    assert 7000 < d["vbatt_mV"] < 8000 and d["iload_mA"] > 0
    assert d["ibatt_mA"] is None and d["psolar_mW"] is None   # INA226 #2 not fitted
    assert d["flags"]["ina_load_ok"] and not d["flags"]["low_batt"]
    assert d["sun"]["adcs_fresh"] and d["state"] != "UNKNOWN" and 0 <= d["soc_pct"] <= 100
    assert d["vmin_mV"] <= d["vmax_mV"] and d["e_load_mWh"] >= 0
    out = uplink_and_wait(app, sim, cmd(92, I.GL_TGT_EPS, I.GL_EPS_GET))    # v0.3 command unchanged
    assert len(ground.reassemble(acks_for(out, 92))) == I.GL_EPS_LEN


def test_capture_refused_in_safe_and_logged_when_done(cfg, sim_lib):
    cfg["payload"] = {"camera": {"period_s": 0}}
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    app.set_obc_mode(I.ICD_MODE_SAFE, "test")
    run(app, 1)
    uplink_and_wait(app, sim, cmd(93, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE))
    (status, _), = [v for k, v in app.ack_cache.items() if k[1] == 93]   # no downlink in SAFE (open issue 1)
    assert status == I.GL_ST_REFUSED and not app.payload.cam.busy
    app.set_obc_mode(I.ICD_MODE_NOMINAL, "test")
    out = uplink_and_wait(app, sim, cmd(94, I.GL_TGT_OBC, I.GL_OBC_CAPTURE_IMAGE))
    assert acks_for(out, 94)[0]["status"] == I.ICD_ST_OK
    finish_capture(app)
    recs = [json.loads(x) for f in Path(cfg["obc"]["data_dir"], "telemetry").glob("*.jsonl")
            for x in f.read_text().splitlines()]
    done = [r["payload"]["camera"] for r in recs if "camera" in r.get("payload", {})]
    assert done and done[-1]["image"].startswith("img_000000_")


def test_telemetry_carries_eps_state_and_payload(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 12)
    rec = app.tick()
    assert rec["eps"]["power_state"] in ("SUN_CHARGING", "SUN_FULL", "SUN_DISCHARGING", "ECLIPSE")
    assert "soc_pct" in rec["eps"] and rec["eps"]["low_battery"] is False
    assert rec["lux"] is not None and rec["lux"] > 0


def test_file_downlink_selective_repeat(cfg, sim_lib):
    """GL v3: an image comes down through the real TT&C firmware, with blocks
    lost on the first pass and only those sent again."""
    import random
    from OBC.comms import arq
    cfg["payload"] = {"camera": {"period_s": 0}}
    cfg["obc"]["file_window"] = 16
    cfg["obc"]["file_block"] = 17                       # v3 size: every block in one QUEUE_TX
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.downlink()
    img = bytes(random.Random(7).randrange(256) for _ in range(17 * 50 + 9))   # 51 blocks
    app.payload.cam.dir.mkdir(parents=True, exist_ok=True)
    (app.payload.cam.dir / "img_000001.jpg").write_bytes(b"older image")
    (app.payload.cam.dir / "img_000002.jpg").write_bytes(img)

    # Errors are ACKed too: wrong length, no such image, FILE_ACK without a transfer.
    out = uplink_and_wait(app, sim, cmd(60, I.GL_TGT_OBC, I.GL_OBC_FILE_START, b"\x00"))
    assert acks_for(out, 60)[0]["status"] == I.ICD_ST_BAD_LENGTH
    out = uplink_and_wait(app, sim, cmd(61, I.GL_TGT_OBC, I.GL_OBC_FILE_START, struct.pack("<H", 2)))
    assert acks_for(out, 61)[0]["status"] == I.ICD_ST_NO_DATA
    out = uplink_and_wait(app, sim, cmd(62, I.GL_TGT_OBC, I.GL_OBC_FILE_ACK, arq.build_ack_args(1, 0, 0)))
    assert acks_for(out, 62)[0]["status"] == I.ICD_ST_NO_DATA

    raw = []
    sim.uplink(cmd(63, I.GL_TGT_OBC, I.GL_OBC_FILE_START, struct.pack("<H", 0)), -95)   # newest
    for _ in range(4):
        run(app, 1)
        raw += sim.downlink()
    pk = [ground.parse_downlink(p) for p in raw]
    ack = acks_for(pk, 63)
    assert ack[0]["status"] == I.ICD_ST_OK
    info = arq.parse_start_info(bytes.fromhex(ack[0]["data"]))
    assert info["size"] == len(img) and info["blocks"] == 51 and info["window"] == 16
    # The ACK goes down before the first block (TT&C queue is FIFO).
    kinds = [p["type"] for p in pk if p["type"] in ("ACK", "FILE")]
    assert kinds[0] == "ACK" and kinds.count("FILE") == 16

    rx = arq.FileReceiver(info)
    lose = {1, 4, 5, 11, 20, 33}                       # first transmission of these is lost
    first_sent = set()
    sent = 0
    for p in raw:
        if p[I.GL_OFS_TYPE] == I.GL_T_FILE:
            sent += 1
            k = arq.parse_block(p)[1]
            if k in lose and k not in first_sent:
                first_sent.add(k)
                continue
            rx.feed(p)
    seq = 64
    for _ in range(30):
        if rx.done:
            break
        # Fresh counter every time: a FILE_ACK is a new request, never a cached retransmission.
        sim.uplink(cmd(seq, I.GL_TGT_OBC, I.GL_OBC_FILE_ACK, rx.ack_args()), -95)
        raw = []
        for _ in range(4):
            run(app, 1)
            raw += sim.downlink()
        a = acks_for([ground.parse_downlink(p) for p in raw], seq)
        assert a and a[0]["status"] == I.ICD_ST_OK           # every FILE_ACK is ACKed
        for p in raw:
            if p[I.GL_OFS_TYPE] == I.GL_T_FILE:
                sent += 1
                k = arq.parse_block(p)[1]
                if k in lose and k not in first_sent:
                    first_sent.add(k)
                    continue
                rx.feed(p)
        seq += 1
    assert rx.done and rx.data() == img
    assert app.xfer is not None                         # sender closes on the final FILE_ACK
    sim.uplink(cmd(seq, I.GL_TGT_OBC, I.GL_OBC_FILE_ACK, rx.ack_args()), -95)
    out = []
    for _ in range(3):
        run(app, 1)
        out += downlink(sim)
    a = acks_for(out, seq)
    assert a[0]["status"] == I.ICD_ST_OK and bytes.fromhex(a[0]["data"]) == b"\x00"
    assert app.xfer is None
    assert sent == 51 + len(lose)                       # only the lost blocks came twice
    assert rx.duplicates == 0

    # SAFE mode refuses a new transfer (TT&C would not transmit it anyway).
    app.set_obc_mode(I.ICD_MODE_SAFE, "test")
    out = uplink_and_wait(app, sim, cmd(seq + 1, I.GL_TGT_OBC, I.GL_OBC_FILE_START, b"\x00\x00"))
    assert acks_for(out, seq + 1)[0]["status"] == I.GL_ST_REFUSED


def test_file_downlink_long_blocks_and_thumbnail(cfg, sim_lib):
    """GL v4: 200-byte blocks through TT&C QUEUE_TX_PART, and the EXIF thumbnail variant."""
    import random
    from OBC.comms import arq
    from .conftest import make_exif_jpeg
    cfg["payload"] = {"camera": {"period_s": 0}}
    app, sim = build_sim(cfg, sim=sim_lib)
    assert app.cfg["obc"]["file_block"] == 200          # the v4 default
    app.start()
    run(app, 2)
    sim.downlink()
    rnd = random.Random(3)
    thumb = b"\xff\xd8" + bytes(rnd.randrange(256) for _ in range(3000)) + b"\xff\xd9"
    full = make_exif_jpeg(thumb, body=bytes(rnd.randrange(256) for _ in range(6000)))
    app.payload.cam.dir.mkdir(parents=True, exist_ok=True)
    (app.payload.cam.dir / "img_000001.jpg").write_bytes(b"\xff\xd8 no exif \xff\xd9")
    (app.payload.cam.dir / "img_000002.jpg").write_bytes(full)

    out = uplink_and_wait(app, sim, cmd(80, I.GL_TGT_OBC, I.GL_OBC_FILE_START, b"\x00\x00\x07"))
    assert acks_for(out, 80)[0]["status"] == I.ICD_ST_BAD_PARAM       # unknown variant
    out = uplink_and_wait(app, sim, cmd(81, I.GL_TGT_OBC, I.GL_OBC_FILE_START, b"\x01\x00\x01"))
    assert acks_for(out, 81)[0]["status"] == I.ICD_ST_NO_DATA         # older image: no thumbnail

    def fetch(seq, args, want):
        sim.uplink(cmd(seq, I.GL_TGT_OBC, I.GL_OBC_FILE_START, args), -95)
        raw = []
        for _ in range(6):
            run(app, 1)
            raw += sim.downlink()
        ack = acks_for([ground.parse_downlink(p) for p in raw], seq)
        assert ack[0]["status"] == I.ICD_ST_OK
        info = arq.parse_start_info(bytes.fromhex(ack[0]["data"]))
        assert info["block"] == 200 and info["size"] == len(want)
        rx = arq.FileReceiver(info)
        seq += 1
        for _ in range(20):
            for p in raw:
                if p[I.GL_OFS_TYPE] == I.GL_T_FILE:
                    assert len(p) <= I.GL_DOWN_MAX_LEN
                    rx.feed(p)
            if rx.done:
                break
            sim.uplink(cmd(seq, I.GL_TGT_OBC, I.GL_OBC_FILE_ACK, rx.ack_args()), -95)
            seq += 1
            raw = []
            for _ in range(6):
                run(app, 1)
                raw += sim.downlink()
        assert rx.done and rx.data() == want
        return seq

    seq = fetch(82, b"\x00\x00\x01", thumb)                # newest image, thumbnail: 16 blocks
    long_pkts = 200 + I.GL_FILE_OFS_DATA + I.GL_CRC_LEN
    assert long_pkts > I.GL_MAX_LEN                         # really more than one QUEUE_TX
    fetch(seq, b"\x00\x00", full)                           # the whole image, old 2-byte args


LINE_OBC_RUN, LINE_OBC_GLOBAL_EN, LINE_ADCS_NRST = 0, 1, 2   # TTC_LINE_* in TTC/ttc_hw.h


_ecounter = itertools.count(1_700_000_000)   # Unix-time-like, as the ground tool will use (above the floor)


def ecmd(seq, target, c, args=b"", counter=None):
    from .conftest import TTC_TEST_KEY
    return ground.build_ecmd(seq, target, c, args, key=TTC_TEST_KEY, counter=counter or next(_ecounter))


def test_emergency_commands_work_with_the_obc_hung(cfg, sim_lib):
    """GL v4: with the OBC application not running at all (hung Pi), TT&C
    still checks, executes and acknowledges emergency commands by itself."""
    sim = sim_lib
    sim.init(31)
    sim.run_ms(40000)                                       # the OBC never talks: master lost
    sim.downlink()

    def send(pkt):
        sim.uplink(pkt, -90)
        sim.run_ms(500)
        return [ground.parse_downlink(p) for p in sim.downlink()]

    out = send(ecmd(1, I.GL_TGT_TTC, I.GL_E_PING))
    acks = [p for p in out if p["type"] == "ACK" and p["emergency"]]
    assert len(acks) == 1 and acks[0]["status"] == I.ICD_ST_OK and acks[0]["target"] == I.GL_TGT_TTC
    info = ground.parse_eping(bytes.fromhex(acks[0]["data"]))
    assert info["obc_silent_s"] >= 39 and info["wdt_min"] == 0

    reset = ecmd(2, I.GL_TGT_OBC, I.GL_E_OBC_RESET, struct.pack("<I", I.GL_REBOOT_KEY))
    out = send(reset)
    assert [p["status"] for p in out if p["type"] == "ACK"] == [I.ICD_ST_OK]
    assert sim.ttc_line_pulses(LINE_OBC_RUN) == 1 and sim.ttc_line_ms(LINE_OBC_RUN) >= I.GL_E_OBC_RESET_PULSE_MS - 2
    out = send(reset)                                       # same packet again: ACK again, no second pulse
    assert [p["status"] for p in out if p["type"] == "ACK"] == [I.ICD_ST_OK]
    assert sim.ttc_line_pulses(LINE_OBC_RUN) == 1
    # Signed with the OBC key instead of the TT&C key: ignored.
    forged = ground.build_ecmd(3, I.GL_TGT_OBC, I.GL_E_OBC_RESET, struct.pack("<I", I.GL_REBOOT_KEY),
                               key=b"\x55" * 32, counter=next(_ecounter))
    assert send(forged) == [] and sim.ttc_line_pulses(LINE_OBC_RUN) == 1
    out = send(ecmd(4, I.GL_TGT_ADCS, I.GL_E_ADCS_RESET, struct.pack("<I", I.GL_REBOOT_KEY)))
    ack = [p for p in out if p["type"] == "ACK"][0]
    assert ack["status"] == I.ICD_ST_OK and sim.ttc_line_pulses(LINE_ADCS_NRST) == 1
    assert bytes.fromhex(ack["data"]) == bytes([I.GL_EA_REPORT_FOLLOWS])
    # Execution reports: the RUN pulse and the ADCS reset each came back as an EVENT.
    sim.run_ms(500)
    out += [ground.parse_downlink(p) for p in sim.downlink()]
    ev = [p for p in out if p["type"] == "EVENT"]
    assert any(e["cmd"] == I.GL_E_ADCS_RESET and e["ecmd_seq"] == 4 and e["source"] == "ecmd"
               and e["result"] == I.ICD_ST_OK for e in ev), ev
    out = send(ecmd(5, I.GL_TGT_TTC, I.GL_E_GET_EVENTS))
    log = ground.parse_events(bytes.fromhex([p for p in out if p["type"] == "ACK"][0]["data"]))
    assert log[0]["cmd"] == I.GL_E_ADCS_RESET and log[1]["cmd"] == I.GL_E_OBC_RESET
    assert log[0]["no"] == log[1]["no"] + 1


def test_ecmd_never_reaches_the_obc(cfg, sim_lib):
    """With the OBC running, an ECMD is answered once, by TT&C; the OBC never sees it."""
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    sim.downlink()
    out = uplink_and_wait(app, sim, ecmd(5, I.GL_TGT_TTC, I.GL_E_PING))
    acks = [p for p in out if p["type"] == "ACK"]
    assert len(acks) == 1 and acks[0]["emergency"]
    # An ordinary command to the OBC is still answered by the OBC (no emergency flag).
    out = uplink_and_wait(app, sim, cmd(6, I.GL_TGT_OBC, I.GL_OBC_PING))
    acks = acks_for(out, 6)
    assert acks and not acks[0]["emergency"]
    r = app.ttc.radio()
    assert "deployed_flag" in r and r["auto_deploy_active"] and not r["deployed_flag"]
    assert app.ttc.counters()["auto_deploy_attempts"] == 0
    assert app.ttc.deploy_reset() == I.ICD_ST_OK            # sim default: RBF plug in (bench)
