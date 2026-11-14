"""OBC configuration: TOML file merged over built-in defaults.

Only the values the OBC itself needs are listed here. The [eps] and [payload]
tables are passed whole to EPS/EPS.py and Payload/Payload.py, which merge them over their
own DEFAULTS, so any key documented there can be set in obc.toml.
"""
from __future__ import annotations

import copy
import tomllib
from pathlib import Path

import icd_defs as I

DEFAULTS = {
    "link": {"bus": 1, "adcs_addr": I.ICD_ADDR_ADCS, "ttc_addr": I.ICD_ADDR_TTC},
    # Pi sensor bus (i2c-3): EPS INA226 + payload sensors.
    "sensors": {"bus": 3, "ina226": True, "ina226_addr": I.ICD_ADDR_INA226_EPS,
                "shunt_ohm": 0.1, "max_current_a": 0.8},       # R100 module as bought: CHECK at bring-up
    "eps": {"low_batt_mv": 6600, "recover_mv": 7000},           # rest: EPS.DEFAULTS
    "payload": {"enabled": True},                               # rest: Payload.DEFAULTS
    "pyro": {"enabled": False, "gpio": 17, "max_ms": 2000, "arm_window_s": 10},
    "obc": {"data_dir": "/home/pi/cubesat_data", "poll_period_s": 1.0, "slow_poll_every": 10,
            "beacon_update_s": 10, "allow_reboot": False, "adcs_auto_nominal": True,
            "max_ground_packets_per_tick": 4},
    "adcs": {"bdot_gain": I.ADCS_BDOT_GAIN_DEFAULT, "exit_rate_cdps": I.ADCS_EXIT_RATE_DEFAULT,
             "enter_rate_cdps": I.ADCS_ENTER_RATE_DEFAULT, "dwell_s": I.ADCS_DWELL_S_DEFAULT,
             "period_ms": I.ADCS_PERIOD_MS_DEFAULT, "settle_ms": I.ADCS_SETTLE_MS_DEFAULT,
             "mag_offset": [0, 0, 0],
             # [] = automatic: ADCS measures it at boot, the OBC keeps the last good value.
             # [x, y, z] (gyro LSB, 65.5 per dps) = always send this value instead.
             "gyro_offset": [],
             # Sun sensors (BPW34, ICD v0.6), ADC counts: full = Sun straight onto a diode;
             # lit = a face at or above this is sunlit (keep it above the Earth-albedo glow).
             "sun_lit_threshold": I.ADCS_SUN_LIT_DEFAULT, "sun_full_scale": I.ADCS_SUN_FULL_DEFAULT,
             # Sun diodes ADCS must ignore, e.g. ["-Z"] if one is known broken (it also finds
             # dead ones itself). [] = use all. Sending it resets what ADCS has learned.
             "sun_disabled_faces": []},
    "ttc": {"beacon_period_s": I.TTC_BEACON_PERIOD_DEFAULT_S},
    # Secret key for ground-command MACs (hex text). Missing -> every ground command is rejected.
    "ground": {"key_file": "/home/pi/.cubesat/ground_key"},
}


def _merge(base: dict, over: dict) -> dict:
    for k, v in over.items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _merge(base[k], v)
        else:
            base[k] = v
    return base


def load(path: str | Path | None) -> dict:
    cfg = copy.deepcopy(DEFAULTS)
    if path:
        with open(path, "rb") as f:
            _merge(cfg, tomllib.load(f))
    return cfg
