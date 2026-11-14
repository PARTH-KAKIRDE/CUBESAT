"""Gyro zero-rate offset (ICD v0.5), OBC + real ADCS firmware in simulation.

ADCS measures the offset at boot when the board is still (bench). The OBC saves
the last good value and re-sends it when ADCS boots moving (orbit) and rejects
its own measurement. A manual [adcs] gyro_offset, or one set from the ground,
always wins."""
import json
import logging
import struct

import icd_defs as I  # noqa: E402
from icd_link import SlaveLink  # noqa: E402
from OBC.comms.bus import SimBus  # noqa: E402
from OBC.comms.slaves import AdcsClient  # noqa: E402
from OBC.factory import build_sim  # noqa: E402

from .test_obc_sim import acks_for, cmd, run, uplink_and_wait

BIAS_DPS = (2.0, -1.5, 1.0)                     # |bias| 2.69 dps: above the 2.00 dps "detumbled" limit
BIAS_LSB = [round(b * I.ADCS_GYRO_LSB_PER_DPS_X10 / 10) for b in BIAS_DPS]   # 131, -98, 66


def probe(sim):
    return AdcsClient(SlaveLink(SimBus(sim), I.ICD_ADDR_ADCS, "probe"))


def reboot_adcs(sim, rate=(0.0, 0.0, 0.0)):
    sim.lib.sim_adcs_set_rate_dps(*rate)
    sim.lib.sim_adcs_set_gyro_bias_dps(*BIAS_DPS)
    sim.lib.sim_reset_slave(I.ICD_ADDR_ADCS)


def close_to(a, b, tol=3):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def test_bench_value_is_saved_and_reused_when_adcs_boots_moving(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    reboot_adcs(sim)                                    # on the bench: still
    app.start()
    run(app, 12)                                        # slow polls at ticks 1 and 11
    f = app.gyro_ofs_file
    saved = json.loads(f.read_text())
    assert close_to(saved["offset_lsb"], BIAS_LSB), saved
    assert probe(sim).gyro_cal()["state"] == I.ADCS_GCAL_AUTO

    reboot_adcs(sim, rate=(10.0, -8.0, 6.0))            # in orbit: reset while tumbling
    run(app, 12)
    gc = probe(sim).gyro_cal()
    assert gc["state"] == I.ADCS_GCAL_COMMANDED and gc["offset_lsb"] == saved["offset_lsb"]

    # An OBC restart (same SD card) still has it.
    app2, sim = build_sim(cfg, sim=sim_lib)             # fresh simulation: ADCS boots tumbling
    assert app2.stored_gyro_ofs == saved["offset_lsb"]
    app2.start()
    run(app2, 12)
    gc = probe(sim).gyro_cal()
    assert gc["state"] == I.ADCS_GCAL_COMMANDED and gc["offset_lsb"] == saved["offset_lsb"]
    assert any("gyro_cal" in json.loads(line).get("adcs", {})
               for line in sorted((app2.data_dir / "telemetry").glob("*.jsonl"))[-1].read_text().splitlines())


def test_nothing_stored_and_adcs_boots_moving_warns_once(cfg, sim_lib, caplog):
    app, sim = build_sim(cfg, sim=sim_lib)              # boots tumbling: REJECTED
    with caplog.at_level(logging.WARNING, logger="obc"):
        app.start()
        run(app, 32)
    assert probe(sim).gyro_cal()["state"] == I.ADCS_GCAL_REJECTED
    assert not app.gyro_ofs_file.exists()
    assert sum("gyro offset not measured" in r.message for r in caplog.records) == 1


def test_manual_value_wins_even_after_a_still_boot(cfg, sim_lib):
    cfg["adcs"]["gyro_offset"] = [10, 20, 30]
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    assert probe(sim).gyro_cal()["offset_lsb"] == [10, 20, 30]
    reboot_adcs(sim)                                    # still: ADCS would measure AUTO...
    run(app, 12)                                        # ...but the OBC re-sends the manual value on the reset
    gc = probe(sim).gyro_cal()
    assert gc["state"] == I.ADCS_GCAL_COMMANDED and gc["offset_lsb"] == [10, 20, 30]
    assert not app.gyro_ofs_file.exists()


def test_ground_override_is_forwarded_and_survives_a_reset(cfg, sim_lib):
    app, sim = build_sim(cfg, sim=sim_lib)
    app.start()
    run(app, 2)
    out = uplink_and_wait(app, sim, cmd(70, I.GL_TGT_ADCS, I.ADCS_CMD_SET_GYRO_OFFSET, struct.pack("<hhh", 5, -6, 7)))
    assert acks_for(out, 70)[0]["status"] == I.ICD_ST_OK
    reboot_adcs(sim)
    run(app, 12)
    gc = probe(sim).gyro_cal()
    assert gc["state"] == I.ADCS_GCAL_COMMANDED and gc["offset_lsb"] == [5, -6, 7]
    # Out of range is refused by ADCS and reported to the ground.
    out = uplink_and_wait(app, sim, cmd(71, I.GL_TGT_ADCS, I.ADCS_CMD_SET_GYRO_OFFSET,
                                        struct.pack("<hhh", I.ADCS_GYRO_OFS_MAX + 1, 0, 0)))
    assert acks_for(out, 71)[0]["status"] == I.ICD_ST_BAD_PARAM


def test_corrupt_stored_file_is_ignored(cfg, sim_lib):
    app, _ = build_sim(cfg, sim=sim_lib)
    app.gyro_ofs_file.write_text(json.dumps({"offset_lsb": [99999, 0, 0]}))
    assert app._load_gyro_offset() is None
    app.gyro_ofs_file.write_text("not json")
    assert app._load_gyro_offset() is None
