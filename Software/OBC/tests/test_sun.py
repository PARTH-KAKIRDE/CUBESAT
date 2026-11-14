"""Sun sensors (BPW34 on all 6 faces; ICD v0.6/v0.7): OBC + real ADCS firmware in simulation."""
import math
import struct

import icd_defs as I  # noqa: E402
from icd_link import SlaveLink  # noqa: E402
from OBC.comms.bus import SimBus  # noqa: E402
from OBC.comms.slaves import AdcsClient  # noqa: E402
from OBC.factory import build_sim  # noqa: E402

from .test_obc_sim import acks_for, cmd, run, uplink_and_wait


def probe(sim):
    return AdcsClient(SlaveLink(SimBus(sim), I.ICD_ADDR_ADCS, "probe"))


def hold_still(sim, x, y, z):
    sim.lib.sim_adcs_set_rate_dps(0.0, 0.0, 0.0)
    sim.lib.sim_adcs_set_sun_body(x, y, z)


def test_sun_direction_and_faces(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 1)
    app.adcs.set_mode(I.ICD_MODE_NOMINAL)               # coils off: nothing turns the satellite
    for want, lit in (((0.6, 0.48, 0.64), {"+X", "+Y", "+Z"}), ((-0.6, 0.2, -0.775), {"-X", "-Z"})):
        hold_still(sim, *want)
        run(app, 2)
        s = probe(sim).sun()
        assert s["fitted_faces"] == ["+X", "-X", "+Y", "-Y", "+Z", "-Z"] and s["dead_faces"] == []
        assert s["lit"] and set(s["lit_faces"]) == lit
        n = math.sqrt(sum(w * w for w in want))
        dot = sum(a * b / n for a, b in zip(s["vector"], want))
        assert math.degrees(math.acos(min(1.0, dot))) < 5.0, (want, s)
        assert not s["blind_side"] and not s["hidden"]    # six faces: always measured directly


def test_eclipse_from_sun_sensors_drives_the_eps(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    hold_still(sim, 0.0, 0.6, 0.8)
    run(app, 22)                                        # slow polls at ticks 1, 11, 21
    assert app.eps.sunlit is True
    sim.lib.sim_adcs_set_eclipse(1)
    run(app, 30)                                        # > sun_hold_s (15 s) and two slow polls
    assert probe(sim).sun()["lit"] is False
    assert app.eps.sunlit is False and app.eps.state == I.GL_PS_ECLIPSE
    sim.lib.sim_adcs_set_eclipse(0)
    run(app, 30)
    assert app.eps.sunlit is True


def test_sun_config_sent_and_ground_override_survives_reset(cfg, sim_lib):
    cfg["adcs"]["sun_lit_threshold"], cfg["adcs"]["sun_full_scale"] = 1100, 3300
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    s = probe(sim).sun()
    assert (s["lit_threshold"], s["full_scale"]) == (1100, 3300)
    out = uplink_and_wait(app, sim, cmd(60, I.GL_TGT_ADCS, I.ADCS_CMD_SET_SUN_CONFIG, struct.pack("<HH", 900, 3100)))
    assert acks_for(out, 60)[0]["status"] == I.ICD_ST_OK
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)
    run(app, 2)
    s = probe(sim).sun()
    assert (s["lit_threshold"], s["full_scale"]) == (900, 3100)   # not back to obc.toml
    out = uplink_and_wait(app, sim, cmd(61, I.GL_TGT_ADCS, I.ADCS_CMD_SET_SUN_CONFIG, struct.pack("<HH", 3000, 2000)))
    assert acks_for(out, 61)[0]["status"] == I.ICD_ST_BAD_PARAM


def turn_onto(app, sim, axis_cond, rate=(0.0, 0.0, 4.0), max_s=200):
    """Spin (the gyro sees it) until axis_cond(sun_body) is true, then stop."""
    import ctypes
    sim.lib.sim_adcs_set_rate_dps(*rate)
    b = (ctypes.c_double * 3)()
    for _ in range(max_s * 10):
        sim.run_ms(100)
        sim.lib.sim_adcs_get_sun_body(b)
        if axis_cond(list(b)):
            break
    sim.lib.sim_adcs_set_rate_dps(0.0, 0.0, 0.0)


def test_sun_behind_a_broken_diode_is_unknown_not_eclipse(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    app.adcs.set_mode(I.ICD_MODE_NOMINAL)
    hold_still(sim, 0.0, 0.95, 0.31)                    # Sun on +Y
    run(app, 22)
    assert app.eps.sunlit is True
    sim.lib.sim_adcs_set_sun_dead(I.ADCS_FACE_PX)       # +X diode breaks
    turn_onto(app, sim, lambda b: b[0] > 0.97)          # ADCS learns it on the way, then the Sun sits on it
    run(app, 30)                                        # > sun_hold_s and two slow polls
    s = probe(sim).sun()
    assert s["hidden"] and not s["lit"] and s["dead_faces"] == ["+X"]
    assert app.eps.sunlit is not False                  # never an eclipse: unknown (or the TSL's vote)
    assert app.eps.state != I.GL_PS_ECLIPSE
    st = probe(sim).status()
    from OBC.comms.slaves import ADCS_FAULT_NAMES, fault_names
    assert "SUN_DIODE" in fault_names(st.faults, ADCS_FAULT_NAMES)


def test_disabled_faces_from_config_and_from_the_ground(cfg, sim_lib):
    cfg["adcs"]["sun_disabled_faces"] = ["-Z"]
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    assert probe(sim).sun()["dead_faces"] == ["-Z"]
    out = uplink_and_wait(app, sim, cmd(62, I.GL_TGT_ADCS, I.ADCS_CMD_SET_SUN_DISABLED, bytes([I.ADCS_FACE_PY])))
    assert acks_for(out, 62)[0]["status"] == I.ICD_ST_OK
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)
    run(app, 2)
    assert probe(sim).sun()["dead_faces"] == ["+Y"]     # the ground's choice, re-sent after the reset
    out = uplink_and_wait(app, sim, cmd(63, I.GL_TGT_ADCS, I.ADCS_CMD_SET_SUN_DISABLED, bytes([0x40])))
    assert acks_for(out, 63)[0]["status"] == I.ICD_ST_BAD_PARAM
