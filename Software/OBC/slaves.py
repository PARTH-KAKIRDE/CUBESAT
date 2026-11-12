"""Typed clients for the ADCS and TT&C slaves, with telemetry decoding.

All decoding uses the offsets from icd_defs (generated from the C header),
so a layout change in the header shows up here automatically.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

import icd_defs as I
from icd_link import MODE_NAMES, SlaveError, SlaveLink


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def i16(b, o): return struct.unpack_from("<h", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


@dataclass
class SlaveStatus:
    mode: int
    flags: int
    faults: int
    uptime_s: int

    @property
    def mode_name(self) -> str:
        return MODE_NAMES.get(self.mode, str(self.mode))

    @property
    def fault_active(self): return bool(self.flags & I.ICD_SF_FAULT_ACTIVE)
    @property
    def ground_pkt_pending(self): return bool(self.flags & I.ICD_SF_GROUND_PKT_PENDING)
    @property
    def no_master_contact(self): return bool(self.flags & I.ICD_SF_NO_MASTER_CONTACT)
    @property
    def reset_occurred(self): return bool(self.flags & I.ICD_SF_RESET_OCCURRED)

    def as_dict(self):
        return {"mode": self.mode_name, "flags": self.flags, "faults": self.faults, "uptime_s": self.uptime_s,
                "fault_active": self.fault_active, "reset_occurred": self.reset_occurred,
                "no_master_contact": self.no_master_contact}


def _parse_status(p: bytes) -> SlaveStatus:
    return SlaveStatus(p[I.ICD_STATUS_OFS_MODE], p[I.ICD_STATUS_OFS_FLAGS],
                       u16(p, I.ICD_STATUS_OFS_FAULTS), u32(p, I.ICD_STATUS_OFS_UPTIME_S))


# One-shot flags the slave clears once it has answered a GET_STATUS.
_ONE_SHOT_FLAGS = I.ICD_SF_RESET_OCCURRED | I.ICD_SF_NO_MASTER_CONTACT


class SlaveClient:
    def __init__(self, link: SlaveLink):
        self.link = link
        self.last_uptime_s: Optional[int] = None

    def ping(self) -> None:
        self.link.call(I.ICD_CMD_PING)

    def version(self) -> dict:
        p = self.link.call(I.ICD_CMD_GET_VERSION)
        return {"fw": f"{p[0]}.{p[1]}.{p[2]}", "icd": f"{p[3]}.{p[4]}",
                "icd_major": p[3], "icd_minor": p[4]}

    def status(self) -> SlaveStatus:
        """GET_STATUS, made safe against lost replies.

        The slave clears RESET_OCCURRED, NO_MASTER_CONTACT and gone-away sticky
        faults as soon as it has answered, so a reply lost on the bus would
        hide them. Two guards:
          - a lost reply recovered by the link is merged into this one;
          - uptime going backwards means the slave rebooted.
        """
        st = _parse_status(self.link.call(I.ICD_CMD_GET_STATUS))
        lost = self.link.recovered.pop(I.ICD_CMD_GET_STATUS, None)
        if lost is not None and lost.ok and len(lost.payload) >= I.ICD_GET_STATUS_RESP_LEN:
            old = _parse_status(lost.payload)
            st.flags |= old.flags & _ONE_SHOT_FLAGS
            st.faults |= old.faults
        # uptime_s is now_ms/1000 on a u32 ms clock, so it also falls back to 0
        # after 49.7 days; that only causes one harmless re-configuration.
        if self.last_uptime_s is not None and st.uptime_s < self.last_uptime_s:
            st.flags |= I.ICD_SF_RESET_OCCURRED
        self.last_uptime_s = st.uptime_s
        return st

    def set_mode(self, mode: int) -> int:
        """Returns the ICD status (OK, INVALID_MODE, HW_FAULT...)."""
        return self.link.transact(I.ICD_CMD_SET_MODE, bytes([mode])).status

    def telemetry_raw(self, page: int) -> bytes:
        return self.link.call(I.ICD_CMD_GET_TELEMETRY, bytes([page]))


# ---------------------------------------------------------------- ADCS

ADCS_FAULT_NAMES = {I.ADCS_FLT_MAG: "MAG", I.ADCS_FLT_IMU: "IMU", I.ADCS_FLT_DRVX: "DRVX",
                    I.ADCS_FLT_DRVY: "DRVY", I.ADCS_FLT_DRVZ: "DRVZ", I.ADCS_FLT_LOCAL_I2C: "LOCAL_I2C",
                    I.ADCS_FLT_OBC_I2C: "OBC_I2C", I.ADCS_FLT_SUN_ADC: "SUN_ADC"}
TTC_FAULT_NAMES = {I.TTC_FLT_RADIO_INIT: "RADIO_INIT", I.TTC_FLT_RADIO_TX_TIMEOUT: "RADIO_TX_TIMEOUT",
                   I.TTC_FLT_TEMP_SENSOR: "TEMP_SENSOR", I.TTC_FLT_RX_OVERFLOW: "RX_OVERFLOW",
                   I.TTC_FLT_TX_OVERFLOW: "TX_OVERFLOW", I.TTC_FLT_OBC_I2C: "OBC_I2C",
                   I.TTC_FLT_LOCAL_I2C: "LOCAL_I2C"}


def fault_names(bits: int, table: dict) -> list:
    return [n for b, n in table.items() if bits & b]


MAG_UT = I.ADCS_MAG_NT_PER_LSB / 1000.0
GYRO_DPS = 10.0 / I.ADCS_GYRO_LSB_PER_DPS_X10


class AdcsClient(SlaveClient):
    # stale if older than one control period plus margin
    MAG_STALE_MS = I.ADCS_PERIOD_MS_DEFAULT + 500

    def sensors(self) -> dict:
        p = self.telemetry_raw(I.ADCS_TLM_PAGE_SENSORS)
        age = u16(p, I.ADCS_TLM0_OFS_MAG_AGE_MS)
        valid = age != I.ADCS_MAG_AGE_INVALID
        return {
            "uptime_ms": u32(p, I.ADCS_TLM0_OFS_UPTIME_MS),
            # The magnetometer is time-multiplexed with the coils: keep its age
            # and never present an old sample as fresh.
            "mag_uT": [i16(p, o) * MAG_UT for o in (I.ADCS_TLM0_OFS_MAG_X, I.ADCS_TLM0_OFS_MAG_Y,
                                                    I.ADCS_TLM0_OFS_MAG_Z)] if valid else None,
            "mag_age_ms": age if valid else None,
            "mag_stale": (not valid) or age > self.MAG_STALE_MS,
            "gyro_dps": [i16(p, o) * GYRO_DPS for o in (I.ADCS_TLM0_OFS_GYRO_X, I.ADCS_TLM0_OFS_GYRO_Y,
                                                        I.ADCS_TLM0_OFS_GYRO_Z)],
        }

    def sun_mtq(self) -> dict:
        p = self.telemetry_raw(I.ADCS_TLM_PAGE_SUN_MTQ)
        return {
            "sun_raw": [u16(p, I.ADCS_TLM1_OFS_SUN1 + 2 * i) for i in range(6)],
            "duty_permille": [i16(p, o) for o in (I.ADCS_TLM1_OFS_DUTY_X, I.ADCS_TLM1_OFS_DUTY_Y,
                                                  I.ADCS_TLM1_OFS_DUTY_Z)],
        }

    def control(self) -> dict:
        p = self.telemetry_raw(I.ADCS_TLM_PAGE_CTRL)
        cf = p[I.ADCS_TLM2_OFS_CTRL_FLAGS]
        return {
            "dbdt_uT_s": [i16(p, o) / 100.0 for o in (I.ADCS_TLM2_OFS_DBDT_X, I.ADCS_TLM2_OFS_DBDT_Y,
                                                      I.ADCS_TLM2_OFS_DBDT_Z)],
            "rate_dps": u16(p, I.ADCS_TLM2_OFS_RATE_NORM) / 100.0,
            "imu_temp_C": i16(p, I.ADCS_TLM2_OFS_IMU_TEMP) / 100.0,
            "detumbled": bool(cf & I.ADCS_CF_DETUMBLED),
            "coils_active": bool(cf & I.ADCS_CF_COILS_ACTIVE),
            "manual_active": bool(cf & I.ADCS_CF_MANUAL_ACTIVE),
            "mag_valid": bool(cf & I.ADCS_CF_MAG_VALID),
            "mode_reason": p[I.ADCS_TLM2_OFS_MODE_REASON],
            "obc_i2c_resets": u16(p, I.ADCS_TLM2_OFS_OBC_I2C_RESETS),
            "local_i2c_errors": u16(p, I.ADCS_TLM2_OFS_LOCAL_I2C_ERR),
        }

    def set_bdot_gain(self, gain: int) -> int:
        return self.link.transact(I.ADCS_CMD_SET_BDOT_GAIN, struct.pack("<I", gain)).status

    def set_detumble_thresh(self, exit_cdps: int, enter_cdps: int, dwell_s: int) -> int:
        return self.link.transact(I.ADCS_CMD_SET_DETUMBLE_THRESH,
                                  struct.pack("<HHH", exit_cdps, enter_cdps, dwell_s)).status

    def set_bdot_timing(self, period_ms: int, settle_ms: int) -> int:
        return self.link.transact(I.ADCS_CMD_SET_BDOT_TIMING, struct.pack("<HH", period_ms, settle_ms)).status

    def set_mag_offset(self, x: int, y: int, z: int) -> int:
        return self.link.transact(I.ADCS_CMD_SET_MAG_OFFSET, struct.pack("<hhh", x, y, z)).status

    def mtq_manual(self, dx: int, dy: int, dz: int, duration_ms: int) -> int:
        return self.link.transact(I.ADCS_CMD_MTQ_MANUAL, struct.pack("<hhhH", dx, dy, dz, duration_ms)).status

    def config(self) -> dict:
        p = self.link.call(I.ADCS_CMD_GET_CONFIG)
        return {
            "gain": u32(p, I.ADCS_CFG_OFS_GAIN), "exit_rate": u16(p, I.ADCS_CFG_OFS_EXIT_RATE),
            "enter_rate": u16(p, I.ADCS_CFG_OFS_ENTER_RATE), "dwell_s": u16(p, I.ADCS_CFG_OFS_DWELL_S),
            "period_ms": u16(p, I.ADCS_CFG_OFS_PERIOD_MS), "settle_ms": u16(p, I.ADCS_CFG_OFS_SETTLE_MS),
            "mag_offset": [i16(p, I.ADCS_CFG_OFS_MAG_OFS_X + 2 * i) for i in range(3)],
        }


# ---------------------------------------------------------------- TT&C

class TtcClient(SlaveClient):
    def get_rx(self) -> Optional[bytes]:
        r = self.link.transact(I.TTC_CMD_GET_RX)
        if r.status == I.ICD_ST_NO_DATA:
            return None
        if not r.ok:
            raise SlaveError(I.TTC_CMD_GET_RX, r.status)
        return r.payload

    def queue_tx(self, data: bytes) -> int:
        return self.link.transact(I.TTC_CMD_QUEUE_TX, data).status

    def set_beacon(self, data: bytes) -> int:
        return self.link.transact(I.TTC_CMD_SET_BEACON, data).status

    def set_beacon_period(self, seconds: int) -> int:
        return self.link.transact(I.TTC_CMD_SET_BEACON_PERIOD, struct.pack("<H", seconds)).status

    def set_radio(self, freq_hz: int, sf: int, bw: int, cr: int, power_dbm: int, sync: int) -> int:
        return self.link.transact(I.TTC_CMD_SET_RADIO,
                                  struct.pack("<IBBBbB", freq_hz, sf, bw, cr, power_dbm, sync)).status

    def get_radio(self) -> dict:
        p = self.link.call(I.TTC_CMD_GET_RADIO)
        f, sf, bw, cr, pw, sync = struct.unpack("<IBBBbB", p[: I.TTC_RADIO_LEN])
        return {"freq_hz": f, "sf": sf, "bw": bw, "cr": cr, "power_dbm": pw, "sync": sync}

    def burn_arm(self) -> int:
        return self.link.transact(I.TTC_CMD_BURN_ARM, struct.pack("<I", I.TTC_BURN_ARM_KEY)).status

    def burn_fire(self, duration_ms: int) -> int:
        return self.link.transact(I.TTC_CMD_BURN_FIRE, struct.pack("<H", duration_ms)).status

    def radio(self) -> dict:
        p = self.telemetry_raw(I.TTC_TLM_PAGE_RADIO)
        df = p[I.TTC_TLM0_OFS_DEPLOY_FLAGS]
        return {
            "uptime_ms": u32(p, I.TTC_TLM0_OFS_UPTIME_MS),
            "radio_state": p[I.TTC_TLM0_OFS_RADIO_STATE],
            "rx_queue": p[I.TTC_TLM0_OFS_RX_QUEUE], "tx_queue": p[I.TTC_TLM0_OFS_TX_QUEUE],
            "deploy_sw": bool(df & I.TTC_DF_DEPLOY_SW), "rbf_in": bool(df & I.TTC_DF_RBF_IN),
            "burn_armed": bool(df & I.TTC_DF_ARMED), "burning": bool(df & I.TTC_DF_BURNING),
            "burn_done": bool(df & I.TTC_DF_BURN_DONE),
            "last_rssi_dbm": i16(p, I.TTC_TLM0_OFS_LAST_RSSI),
            "last_snr_db": struct.unpack_from("<b", p, I.TTC_TLM0_OFS_LAST_SNR)[0] / 4.0,
            "rx_count": u16(p, I.TTC_TLM0_OFS_RX_COUNT), "tx_count": u16(p, I.TTC_TLM0_OFS_TX_COUNT),
            "rx_crc_err": u16(p, I.TTC_TLM0_OFS_RX_CRC_ERR),
            "board_temp_C": i16(p, I.TTC_TLM0_OFS_BOARD_TEMP) / 100.0,
            "beacon_period_s": u16(p, I.TTC_TLM0_OFS_BEACON_PERIOD),
            "obc_i2c_resets": u16(p, I.TTC_TLM0_OFS_OBC_I2C_RESETS),
        }

    def counters(self) -> dict:
        p = self.telemetry_raw(I.TTC_TLM_PAGE_COUNTERS)
        return {
            "tx_timeouts": u16(p, I.TTC_TLM1_OFS_TX_TIMEOUTS), "rx_dropped": u16(p, I.TTC_TLM1_OFS_RX_DROPPED),
            "tx_dropped": u16(p, I.TTC_TLM1_OFS_TX_DROPPED), "beacon_count": u16(p, I.TTC_TLM1_OFS_BEACON_COUNT),
            "burn_count": p[I.TTC_TLM1_OFS_BURN_COUNT], "last_burn_ms": u16(p, I.TTC_TLM1_OFS_LAST_BURN_MS),
        }
