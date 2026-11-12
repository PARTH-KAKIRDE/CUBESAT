"""OBC configuration: TOML file merged over built-in defaults."""
from __future__ import annotations

import copy
import tomllib
from pathlib import Path

from . import icd_defs as I

DEFAULTS = {
    "link": {"bus": 1, "adcs_addr": I.ICD_ADDR_ADCS, "ttc_addr": I.ICD_ADDR_TTC},
    "sensors": {"bus": 3, "ina226": True, "ina226_addr": I.ICD_ADDR_INA226_EPS, "shunt_ohm": 0.01,
                "max_current_a": 3.2, "tsl2591": True},
    "eps": {"low_batt_mv": 6600, "recover_mv": 7000},
    "pyro": {"enabled": False, "gpio": 17, "max_ms": 2000, "arm_window_s": 10},
    "camera": {"enabled": True, "device": "/dev/video0", "resolution": "1280x720"},
    "obc": {"data_dir": "/home/pi/cubesat_data", "poll_period_s": 1.0, "slow_poll_every": 10,
            "beacon_update_s": 10, "allow_reboot": False, "adcs_auto_nominal": True,
            "max_ground_packets_per_tick": 4},
    "adcs": {"bdot_gain": I.ADCS_BDOT_GAIN_DEFAULT, "exit_rate_cdps": I.ADCS_EXIT_RATE_DEFAULT,
             "enter_rate_cdps": I.ADCS_ENTER_RATE_DEFAULT, "dwell_s": I.ADCS_DWELL_S_DEFAULT,
             "period_ms": I.ADCS_PERIOD_MS_DEFAULT, "settle_ms": I.ADCS_SETTLE_MS_DEFAULT,
             "mag_offset": [0, 0, 0]},
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
