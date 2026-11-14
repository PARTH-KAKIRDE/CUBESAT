"""OBC main application (Raspberry Pi 4B).

One tick per second (ICD section 9):
  - GET_STATUS from ADCS and TT&C, GET_TELEMETRY from ADCS
  - re-send ADCS/TT&C configuration whenever a slave reports RESET_OCCURRED
  - drain ground packets (TTC_GET_RX) when GROUND_PKT_PENDING, execute them,
    queue ACKs back through TTC_QUEUE_TX
  - EPS (EPS/EPS.py: INA226, power state, low-battery SAFE logic)
  - payload (Payload/Payload.py: TSL2591, Pi camera, magnetometer samples from ADCS)
  - refresh the OBC beacon in TT&C every few seconds
  - write one JSON line of telemetry
Every subsystem call is wrapped: one failing board never stops the loop.
"""
from __future__ import annotations

import json
import logging
import os
import shutil
import socket
import struct
import time
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Optional

import icd_defs as I
from icd_link import MODE_NAMES, BusError, LinkError, SlaveError, SlaveLink, status_name

from .comms import ground
from .comms.bus import I2CBus
from .EPS.EPS import Pyro, make_eps
from .Payload.Payload import Payload
from .comms.slaves import AdcsClient, TtcClient, face_mask, faces

log = logging.getLogger("obc")
LINK_ERRORS = (LinkError, SlaveError, BusError)

# ICD commands the ground may send through the OBC (ICD.md, "Ground link").
# Left out on purpose:
#   GET_STATUS  - clears the slave's one-shot flags; answered from the OBC's copy instead
#   TTC GET_RX / QUEUE_TX / SET_BEACON - the OBC owns the TT&C queues and beacon
#   TTC SET_RADIO - a wrong setting cuts the uplink with no way back; set it in obc.toml
GROUND_FORWARD = {
    I.GL_TGT_ADCS: {I.ICD_CMD_PING, I.ICD_CMD_GET_VERSION, I.ICD_CMD_GET_TELEMETRY, I.ICD_CMD_SET_MODE,
                    I.ADCS_CMD_SET_BDOT_GAIN, I.ADCS_CMD_SET_DETUMBLE_THRESH, I.ADCS_CMD_SET_BDOT_TIMING,
                    I.ADCS_CMD_GET_CONFIG, I.ADCS_CMD_MTQ_MANUAL, I.ADCS_CMD_SET_MAG_OFFSET,
                    I.ADCS_CMD_SET_GYRO_OFFSET, I.ADCS_CMD_SET_SUN_CONFIG,
                    I.ADCS_CMD_SET_SUN_DISABLED},
    I.GL_TGT_TTC: {I.ICD_CMD_PING, I.ICD_CMD_GET_VERSION, I.ICD_CMD_GET_TELEMETRY, I.ICD_CMD_SET_MODE,
                   I.TTC_CMD_GET_RADIO, I.TTC_CMD_SET_BEACON_PERIOD, I.TTC_CMD_BURN_ARM, I.TTC_CMD_BURN_FIRE},
}


class Clock:
    def monotonic(self) -> float:
        return time.monotonic()

    def sleep(self, s: float) -> None:
        if s > 0:
            time.sleep(s)


class ObcApp:
    def __init__(self, cfg: dict, link_bus: I2CBus, sensor_bus: Optional[I2CBus], pyro: Pyro,
                 clock: Optional[Clock] = None, reboot_fn: Optional[Callable[[], None]] = None):
        self.cfg = cfg
        self.clock = clock or Clock()
        self.reboot_fn = reboot_fn
        self.t0 = self.clock.monotonic()
        L = cfg["link"]
        self.adcs = AdcsClient(SlaveLink(link_bus, L["adcs_addr"], "ADCS"))
        self.ttc = TtcClient(SlaveLink(link_bus, L["ttc_addr"], "TTC"))
        self.adcs.MAG_STALE_MS = cfg["adcs"]["period_ms"] + 500
        self.eps = make_eps(cfg, sensor_bus, clock=self.clock.monotonic)
        self.pyro = pyro

        self.data_dir = Path(cfg["obc"]["data_dir"])
        P = cfg.get("payload", {})
        self.payload = Payload(P, sensor_bus, self.data_dir, clock=self.clock.monotonic) \
            if P.get("enabled", True) else None
        (self.data_dir / "telemetry").mkdir(parents=True, exist_ok=True)
        self.mode = I.ICD_MODE_BOOT
        self.safe_reason = ""
        self.tick_count = 0
        self.beacon_seq = 0
        self.last_beacon_update = -1e9
        self.outbox: list = []
        self.ack_cache: "OrderedDict[tuple, tuple]" = OrderedDict()
        self.gl_key = self._load_ground_key()
        self.ctr_file = self.data_dir / "ground_counter"
        self.last_counter = self._load_counter()
        # Gyro zero-rate offset (ICD v0.5): ADCS measures it at boot when still
        # (on the bench); the OBC keeps the last good value here and re-sends it
        # when ADCS boots moving (in orbit) and cannot measure it.
        self.gyro_ofs_file = self.data_dir / "adcs_gyro_offset.json"
        self.stored_gyro_ofs = self._load_gyro_offset()
        self._gyro_warned = False
        self.reboot_requested = False
        self.last = {"adcs_status": None, "ttc_status": None, "adcs_ctrl": {}, "eps": {}, "lux": None}
        self._notify_sock = None
        self._notify_ready_sent = False

    # ------------------------------------------------------------ ground auth state
    def _load_ground_key(self) -> Optional[bytes]:
        path = self.cfg["ground"]["key_file"]
        try:
            return ground.load_key(path)
        except (OSError, ground.PacketError) as e:
            # Fail closed: without a key every ground command is rejected.
            log.error("ground key not loaded (%s): ALL ground commands will be rejected. "
                      "Create it with 'gs_tool.py keygen' and copy it to %s", e, path)
            return None

    def _load_counter(self) -> int:
        try:
            return int(self.ctr_file.read_text().strip())
        except (OSError, ValueError):
            return 0

    def _save_counter(self, value: int) -> None:
        # Written before the command runs, so a crash cannot re-open a replay window.
        tmp = self.ctr_file.with_suffix(".tmp")
        try:
            tmp.write_text(str(value))
            os.replace(tmp, self.ctr_file)
        except OSError as e:
            log.error("cannot save ground counter: %s", e)

    def _load_gyro_offset(self) -> Optional[list]:
        try:
            v = json.loads(self.gyro_ofs_file.read_text())["offset_lsb"]
            if len(v) == 3 and all(isinstance(x, int) and abs(x) <= I.ADCS_GYRO_OFS_MAX for x in v):
                return v
            log.error("ignoring %s: bad offset %r", self.gyro_ofs_file, v)
        except (OSError, ValueError, KeyError, TypeError):
            pass
        return None

    def _save_gyro_offset(self, cal: dict) -> None:
        rec = {"offset_lsb": cal["offset_lsb"], "offset_dps": cal["offset_dps"], "samples": cal["samples"],
               "gyro_spread_dps": cal["gyro_spread_dps"], "mag_spread_uT": cal["mag_spread_uT"],
               "measured_utc": datetime.now(timezone.utc).isoformat(timespec="seconds")}
        tmp = self.gyro_ofs_file.with_suffix(".tmp")
        try:
            tmp.write_text(json.dumps(rec))
            os.replace(tmp, self.gyro_ofs_file)
            self.stored_gyro_ofs = list(cal["offset_lsb"])
            log.info("ADCS gyro offset measured at boot: %s dps (saved)", cal["offset_dps"])
        except OSError as e:
            log.error("cannot save the gyro offset: %s", e)

    def _gyro_offset_logic(self, cal: dict) -> None:
        """Automatic mode only ([adcs] gyro_offset = []). A manual value is
        sent by configure_adcs() instead, at start-up and after every reset."""
        if self.cfg["adcs"].get("gyro_offset"):
            return
        st = cal["state"]
        if st == I.ADCS_GCAL_AUTO and cal["offset_lsb"] != self.stored_gyro_ofs:
            self._save_gyro_offset(cal)
        elif st in (I.ADCS_GCAL_NONE, I.ADCS_GCAL_REJECTED) and self.stored_gyro_ofs:
            # ADCS booted moving (or without its magnetometer): use the bench value.
            r = self._try("adcs gyro offset", self.adcs.set_gyro_offset, *self.stored_gyro_ofs)
            if r == I.ICD_ST_OK:
                log.info("ADCS could not measure its gyro offset (%s): sent the stored %s LSB",
                         cal["state_name"], self.stored_gyro_ofs)
            elif r is not None:
                log.error("ADCS rejected the stored gyro offset: %s", status_name(r))
        elif st in (I.ADCS_GCAL_NONE, I.ADCS_GCAL_REJECTED) and not self._gyro_warned:
            self._gyro_warned = True
            log.warning("ADCS gyro offset not measured (%s) and none stored: boot ADCS once while it "
                        "is still (on the bench) so the OBC can keep the value", cal["state_name"])


    # ------------------------------------------------------------ helpers
    def uptime_s(self) -> int:
        return int(self.clock.monotonic() - self.t0)

    def _try(self, what: str, fn, *args):
        try:
            return fn(*args)
        except LINK_ERRORS as e:
            log.debug("%s failed: %s", what, e)
            return None

    def configure_adcs(self) -> None:
        a = self.cfg["adcs"]
        steps = [
            ("gain", self.adcs.set_bdot_gain, a["bdot_gain"]),
            ("thresh", self.adcs.set_detumble_thresh, a["exit_rate_cdps"], a["enter_rate_cdps"], a["dwell_s"]),
            ("timing", self.adcs.set_bdot_timing, a["period_ms"], a["settle_ms"]),
            ("mag_offset", self.adcs.set_mag_offset, *a["mag_offset"]),
            ("sun_config", self.adcs.set_sun_config, a["sun_lit_threshold"], a["sun_full_scale"]),
        ]
        if a.get("sun_disabled_faces"):   # only when set: it also clears the dead diodes ADCS learned
            steps.append(("sun_disabled", self.adcs.set_sun_disabled, face_mask(a["sun_disabled_faces"])))
        if a.get("gyro_offset"):   # manual value overrides the boot measurement
            steps.append(("gyro_offset", self.adcs.set_gyro_offset, *a["gyro_offset"]))
        for name, fn, *args in steps:
            st = self._try(f"adcs {name}", fn, *args)
            if st is not None and st != I.ICD_ST_OK:
                log.error("ADCS rejected %s: %s", name, status_name(st))
        log.info("ADCS configuration sent")

    def configure_ttc(self) -> None:
        st = self._try("ttc beacon period", self.ttc.set_beacon_period, self.cfg["ttc"]["beacon_period_s"])
        if st is not None and st != I.ICD_ST_OK:
            log.error("TTC rejected beacon period: %s", status_name(st))

    def set_obc_mode(self, mode: int, reason: str) -> None:
        if mode == self.mode:
            return
        log.warning("OBC mode %s -> %s (%s)", MODE_NAMES.get(self.mode), MODE_NAMES.get(mode), reason)
        self.mode = mode
        if mode == I.ICD_MODE_SAFE:
            self.safe_reason = reason
            self.pyro.safe()
            self._try("adcs safe", self.adcs.set_mode, I.ICD_MODE_SAFE)
            self._try("ttc safe", self.ttc.set_mode, I.ICD_MODE_SAFE)
        elif mode == I.ICD_MODE_NOMINAL:
            self.safe_reason = ""
            self._try("ttc nominal", self.ttc.set_mode, I.ICD_MODE_NOMINAL)
            # ADCS restarts detumbling; it is promoted to NOMINAL once detumbled.
            st = self.last["adcs_status"]
            if st is None or st.mode != I.ICD_MODE_NOMINAL:
                self._try("adcs detumble", self.adcs.set_mode, I.ICD_MODE_DETUMBLE)

    def comm_flags(self) -> int:
        f = 0
        if self.adcs.link.stats.comm_fault:
            f |= I.GL_CF_ADCS_COMM_FAULT
        if self.ttc.link.stats.comm_fault:
            f |= I.GL_CF_TTC_COMM_FAULT
        if self.eps.ina is not None and not self.eps.ok:
            f |= I.GL_CF_EPS_FAULT
        if self.last["adcs_status"] is not None and self.last["adcs_status"].fault_active:
            f |= I.GL_CF_ADCS_FAULT_ACTIVE
        if self.last["ttc_status"] is not None and self.last["ttc_status"].fault_active:
            f |= I.GL_CF_TTC_FAULT_ACTIVE
        if self.eps.low_battery:
            f |= I.GL_CF_LOW_BATTERY
        return f

    @staticmethod
    def cpu_temp_c() -> Optional[float]:
        p = Path("/sys/class/thermal/thermal_zone0/temp")
        try:
            return int(p.read_text()) / 1000.0
        except (OSError, ValueError):
            return None

    # ------------------------------------------------------------ start / tick
    def start(self) -> None:
        log.info("OBC starting, ICD v%d.%d", I.ICD_VERSION_MAJOR, I.ICD_VERSION_MINOR)
        for name, c in (("ADCS", self.adcs), ("TTC", self.ttc)):
            v = self._try(f"{name} version", c.version)
            if v is None:
                log.error("%s not answering at start-up", name)
                continue
            log.info("%s fw %s ICD %s", name, v["fw"], v["icd"])
            if v["icd_major"] != I.ICD_VERSION_MAJOR:
                log.error("%s ICD major version %d != OBC %d", name, v["icd_major"], I.ICD_VERSION_MAJOR)
        self.configure_adcs()
        self.configure_ttc()
        self.mode = I.ICD_MODE_NOMINAL

    def tick(self) -> dict:
        self.tick_count += 1
        slow = (self.tick_count % self.cfg["obc"]["slow_poll_every"]) == 1
        rec: dict = {"t": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                     "uptime_s": self.uptime_s(), "obc_mode": MODE_NAMES.get(self.mode)}

        self._poll_adcs(rec, slow)
        self._poll_ttc(rec, slow)

        if self.payload is not None:
            # Magnetometer: ADCS sampled it with the coils off; the payload only logs fresh samples.
            p = self.payload.tick(self.mode, self.eps.low_battery, rec.get("adcs", {}).get("sensors"))
            if p:
                rec["payload"] = p
            rec["lux"] = self.last["lux"] = self.payload.last_lux
            self.eps.update_sun(lux=self.payload.last_lux)
        rec["eps"] = self.last["eps"] = self.eps.poll()
        if slow:
            t = self.cpu_temp_c()
            if t is not None:
                rec["cpu_temp_C"] = t

        self._mode_logic()
        self._update_beacon()
        self._flush_outbox()

        rec["comm_flags"] = self.comm_flags()
        rec["links"] = {n: {"ok": c.link.stats.ok, "fail": c.link.stats.failures,
                            "busy": c.link.stats.busy_retries, "comm_fault": c.link.stats.comm_fault}
                        for n, c in (("adcs", self.adcs), ("ttc", self.ttc))}
        self._write_record("telemetry", rec)
        self._sd_notify("WATCHDOG=1")
        return rec

    def run(self, ticks: Optional[int] = None) -> None:
        self.start()
        self._sd_notify("READY=1")
        period = self.cfg["obc"]["poll_period_s"]
        nxt = self.clock.monotonic()
        n = 0
        try:
            while ticks is None or n < ticks:
                try:
                    self.tick()
                except Exception:  # noqa: BLE001 - the loop must survive anything
                    log.exception("tick failed")
                n += 1
                if self.reboot_requested and self.reboot_fn:
                    self.reboot_fn()
                nxt += period
                self.clock.sleep(max(0.0, nxt - self.clock.monotonic()))
        finally:
            self.close()

    def close(self) -> None:
        """Stop a running capture and flush the payload log (shutdown, SIGTERM)."""
        if self.payload is not None:
            self.payload.close()

    # ------------------------------------------------------------ polling
    def _poll_adcs(self, rec: dict, slow: bool) -> None:
        st = self._try("adcs status", self.adcs.status)
        self.last["adcs_status"] = st
        if st is None:
            rec["adcs"] = {"error": self.adcs.link.stats.last_error}
            return
        if st.reset_occurred:
            log.warning("ADCS reported a reset: re-sending configuration")
            self.configure_adcs()
        d = {"status": st.as_dict()}
        s = self._try("adcs sensors", self.adcs.sensors)
        if s is not None:
            d["sensors"] = s
        c = self._try("adcs control", self.adcs.control)
        if c is not None:
            d["control"] = self.last["adcs_ctrl"] = c
        if slow:
            sm = self._try("adcs sun sensors", self.adcs.sun_mtq)
            if sm is not None:
                d["sun_mtq"] = sm
            sun = self._try("adcs sun", self.adcs.sun)       # page 4: ADCS's own sun / eclipse decision
            if sun is not None:
                d["sun"] = sun
                self.eps.update_sun(adcs_lit=sun["lit"], adcs_hidden=sun["hidden"])
            elif sm is not None:                             # ADCS fw without page 4 (ICD < v0.6)
                self.eps.update_sun(sun_raw=sm["sun_raw"])
            gc = self._try("adcs gyro cal", self.adcs.gyro_cal)
            if gc is not None:
                d["gyro_cal"] = gc
                self._gyro_offset_logic(gc)
        rec["adcs"] = d

    def _poll_ttc(self, rec: dict, slow: bool) -> None:
        st = self._try("ttc status", self.ttc.status)
        self.last["ttc_status"] = st
        if st is None:
            rec["ttc"] = {"error": self.ttc.link.stats.last_error}
            return
        if st.reset_occurred:
            log.warning("TTC reported a reset: re-sending configuration")
            self.configure_ttc()
            self.last_beacon_update = -1e9
        d = {"status": st.as_dict()}
        self._handle_recovered_rx()
        if st.ground_pkt_pending:
            d["ground_packets"] = self._drain_ground()
        if slow:
            r = self._try("ttc radio", self.ttc.radio)
            if r is not None:
                d["radio"] = r
        rec["ttc"] = d

    # ------------------------------------------------------------ ground commands
    def _handle_recovered_rx(self) -> None:
        """A GET_RX whose reply was lost still popped a packet off the TT&C
        queue; the link read it back later, so process it now."""
        r = self.ttc.link.recovered.pop(I.TTC_CMD_GET_RX, None)
        if r is not None and r.ok and r.payload:
            self._uplink_packet(r.payload)

    def _drain_ground(self) -> int:
        n = 0
        for _ in range(self.cfg["obc"]["max_ground_packets_per_tick"]):
            pkt = self._try("ttc get_rx", self.ttc.get_rx)
            self._handle_recovered_rx()
            if pkt is None:
                break
            n += 1
            self._uplink_packet(pkt)
        return n

    def _uplink_packet(self, pkt: bytes) -> None:
        self._write_record("ground", {"t": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                                      "dir": "up", "hex": pkt.hex()})
        self.handle_ground_packet(pkt)

    def handle_ground_packet(self, pkt: bytes) -> None:
        if self.gl_key is None:
            log.warning("ground packet rejected: no ground key loaded (%s)", pkt.hex())
            return
        try:
            c = ground.parse_cmd(pkt, self.gl_key)
        except ground.PacketError as e:
            log.warning("ground packet rejected: %s (%s)", e, pkt.hex())
            return
        key = (c.counter, c.seq, c.target, c.cmd, c.args)
        if key in self.ack_cache:
            # Ground retransmitted (our ACK was lost): answer again, do not re-execute.
            status, data = self.ack_cache[key]
            log.info("duplicate ground cmd seq %d: re-sending ACK", c.seq)
        elif c.counter <= self.last_counter:
            # Authentic but old: a replay (or a ground PC whose counter went back).
            log.warning("ground cmd rejected: counter %d <= last accepted %d (replay?)",
                        c.counter, self.last_counter)
            return
        else:
            self.last_counter = c.counter
            self._save_counter(c.counter)
            status, data = self._execute(c)
            self.ack_cache[key] = (status, data)
            while len(self.ack_cache) > 32:
                self.ack_cache.popitem(last=False)
        log.info("ground cmd seq=%d tgt=%d cmd=0x%02X -> %s", c.seq, c.target, c.cmd, status_name(status))
        self.outbox.extend(ground.build_ack(c.seq, c.target, c.cmd, status, data))

    def _execute(self, c: ground.Command):
        try:
            if c.target == I.GL_TGT_OBC:
                return self._exec_obc(c)
            if c.target == I.GL_TGT_EPS:
                return self._exec_eps(c)
            if c.target in (I.GL_TGT_ADCS, I.GL_TGT_TTC):
                return self._forward(c)
        except LINK_ERRORS as e:
            log.error("ground cmd failed: %s", e)
            return I.GL_ST_COMM_FAULT, b""
        return I.ICD_ST_UNKNOWN_CMD, b""

    def _forward(self, c: ground.Command):
        """Pass a ground command on to ADCS or TT&C, within GROUND_FORWARD."""
        adcs = c.target == I.GL_TGT_ADCS
        client = self.adcs if adcs else self.ttc
        if c.cmd == I.ICD_CMD_GET_STATUS:
            # Answer from the last poll: a real GET_STATUS would clear the
            # slave's reset/fault flags before the OBC had seen them.
            st = self.last["adcs_status" if adcs else "ttc_status"]
            if st is None:
                return I.GL_ST_COMM_FAULT, b""
            return I.ICD_ST_OK, struct.pack("<BBHI", st.mode, st.flags, st.faults, st.uptime_s)
        if c.cmd not in GROUND_FORWARD[c.target]:
            log.warning("ground cmd 0x%02X to %s refused: not allowed from the ground",
                        c.cmd, "ADCS" if adcs else "TTC")
            return I.GL_ST_REFUSED, b""
        if not adcs and c.cmd in (I.TTC_CMD_BURN_ARM, I.TTC_CMD_BURN_FIRE) and \
                (self.mode == I.ICD_MODE_SAFE or self.eps.low_battery):
            return I.GL_ST_REFUSED, b""   # same rule as the pyro
        r = client.link.transact(c.cmd, c.args)
        if r.ok:
            self._remember_ground_setting(c)
        return r.status, r.payload

    def _remember_ground_setting(self, c: ground.Command) -> None:
        """Keep settings changed from the ground, so that a slave reset re-applies
        them instead of the obc.toml values. (In memory only: an OBC restart
        goes back to obc.toml.)"""
        a, ad = c.args, self.cfg["adcs"]
        if c.target == I.GL_TGT_ADCS:
            if c.cmd == I.ADCS_CMD_SET_BDOT_GAIN and len(a) == I.ADCS_SET_BDOT_GAIN_LEN:
                ad["bdot_gain"] = struct.unpack("<I", a)[0]
            elif c.cmd == I.ADCS_CMD_SET_DETUMBLE_THRESH and len(a) == I.ADCS_SET_DETUMBLE_THRESH_LEN:
                ad["exit_rate_cdps"], ad["enter_rate_cdps"], ad["dwell_s"] = struct.unpack("<HHH", a)
            elif c.cmd == I.ADCS_CMD_SET_BDOT_TIMING and len(a) == I.ADCS_SET_BDOT_TIMING_LEN:
                ad["period_ms"], ad["settle_ms"] = struct.unpack("<HH", a)
                self.adcs.MAG_STALE_MS = ad["period_ms"] + 500
            elif c.cmd == I.ADCS_CMD_SET_MAG_OFFSET and len(a) == I.ADCS_SET_MAG_OFFSET_LEN:
                ad["mag_offset"] = list(struct.unpack("<hhh", a))
            elif c.cmd == I.ADCS_CMD_SET_SUN_DISABLED and len(a) == I.ADCS_SET_SUN_DISABLED_LEN:
                ad["sun_disabled_faces"] = faces(a[0])
            elif c.cmd == I.ADCS_CMD_SET_SUN_CONFIG and len(a) == I.ADCS_SET_SUN_CONFIG_LEN:
                ad["sun_lit_threshold"], ad["sun_full_scale"] = struct.unpack("<HH", a)
            elif c.cmd == I.ADCS_CMD_SET_GYRO_OFFSET and len(a) == I.ADCS_SET_GYRO_OFFSET_LEN:
                ad["gyro_offset"] = list(struct.unpack("<hhh", a))   # manual until the OBC restarts
        elif c.cmd == I.TTC_CMD_SET_BEACON_PERIOD and len(a) == 2:
            self.cfg["ttc"]["beacon_period_s"] = struct.unpack("<H", a)[0]

    def _lux_u16(self) -> int:
        """Beacon/HK light value: 0 = no reading, 65535 = saturated (direct sun)."""
        lux = self.last["lux"]
        if lux is None:
            return 0
        return 65535 if lux == float("inf") else int(max(0, min(65535, lux)))

    def _hk(self) -> bytes:
        e = self.last["eps"]
        b = bytearray(I.GL_HK_LEN)
        struct.pack_into("<I", b, I.GL_HK_OFS_UPTIME_S, self.uptime_s())
        b[I.GL_HK_OFS_OBC_MODE] = self.mode
        b[I.GL_HK_OFS_COMM_FLAGS] = self.comm_flags()
        struct.pack_into("<H", b, I.GL_HK_OFS_VBATT, int(min(65535, e.get("vbus_mV", 0))))
        struct.pack_into("<h", b, I.GL_HK_OFS_IBUS, int(max(-32768, min(32767, e.get("current_mA", 0)))))
        struct.pack_into("<H", b, I.GL_HK_OFS_PBUS, int(min(65535, e.get("power_mW", 0))))
        struct.pack_into("<b", b, I.GL_HK_OFS_CPU_TEMP, int(max(-128, min(127, self.cpu_temp_c() or 0))))
        try:
            du = shutil.disk_usage(self.data_dir)
            b[I.GL_HK_OFS_DISK_FREE] = int(100 * du.free / du.total)
        except OSError:
            pass
        struct.pack_into("<H", b, I.GL_HK_OFS_LUX, self._lux_u16())
        return bytes(b)

    def _exec_obc(self, c: ground.Command):
        a = c.args
        if c.cmd == I.GL_OBC_PING:
            return I.ICD_ST_OK, b""
        if c.cmd == I.GL_OBC_GET_HK:
            return I.ICD_ST_OK, self._hk()
        if c.cmd == I.GL_OBC_SET_MODE:
            if len(a) != 1 or a[0] not in (I.ICD_MODE_SAFE, I.ICD_MODE_NOMINAL):
                return I.ICD_ST_BAD_PARAM, b""
            if a[0] == I.ICD_MODE_NOMINAL and self.eps.low_battery:
                return I.GL_ST_REFUSED, b""
            self.set_obc_mode(a[0], "ground command")
            return I.ICD_ST_OK, b""
        if c.cmd == I.GL_OBC_CAPTURE_IMAGE:
            # The capture runs in the background (it must not stall this loop), so the
            # ACK means "started"; the data is the number of images saved before it.
            if self.payload is None:
                return I.GL_ST_REFUSED, b""
            res = self.payload.request_capture(self.mode, self.eps.low_battery)
            if res == "started":
                return I.ICD_ST_OK, struct.pack("<H", min(0xFFFF, self.payload.images))
            if res == "busy":
                return I.ICD_ST_BUSY, b""
            if res == "error":
                return I.ICD_ST_HW_FAULT, b""
            return I.GL_ST_REFUSED, b""            # disabled, SAFE or low battery
        if c.cmd == I.GL_OBC_GET_SLAVE_TLM:
            if len(a) != 2 or a[0] not in (I.GL_TGT_ADCS, I.GL_TGT_TTC):
                return I.ICD_ST_BAD_PARAM, b""
            client = self.adcs if a[0] == I.GL_TGT_ADCS else self.ttc
            r = client.link.transact(I.ICD_CMD_GET_TELEMETRY, bytes([a[1]]))
            return r.status, r.payload
        if c.cmd == I.GL_OBC_REBOOT:
            if not self.cfg["obc"]["allow_reboot"] or len(a) != 4 or struct.unpack("<I", a)[0] != I.GL_REBOOT_KEY:
                return I.GL_ST_REFUSED, b""
            self.reboot_requested = True
            return I.ICD_ST_OK, b""
        return I.ICD_ST_UNKNOWN_CMD, b""

    def _exec_eps(self, c: ground.Command):
        a = c.args
        if c.cmd == I.GL_EPS_GET:
            e = self.last["eps"]
            b = bytearray(I.GL_EPS_LEN)
            struct.pack_into("<H", b, I.GL_EPS_OFS_VBATT, int(max(0, min(65535, e.get("vbus_mV", 0)))))
            struct.pack_into("<h", b, I.GL_EPS_OFS_IBUS, int(max(-32768, min(32767, e.get("current_mA", 0)))))
            struct.pack_into("<H", b, I.GL_EPS_OFS_PBUS, int(max(0, min(65535, e.get("power_mW", 0)))))
            b[I.GL_EPS_OFS_LOW_BATT] = int(self.eps.low_battery)
            b[I.GL_EPS_OFS_PYRO_FIRES] = min(255, self.pyro.fire_count)
            return I.ICD_ST_OK, bytes(b)
        if c.cmd == I.GL_EPS_GET_STATE:             # ICD v0.4
            return I.ICD_ST_OK, self.eps.state_bytes()
        if c.cmd == I.GL_EPS_PYRO_ARM:
            if len(a) != 4:
                return I.ICD_ST_BAD_LENGTH, b""
            if self.mode == I.ICD_MODE_SAFE:
                return I.GL_ST_REFUSED, b""
            return (I.ICD_ST_OK if self.pyro.arm(struct.unpack("<I", a)[0]) else I.GL_ST_REFUSED), b""
        if c.cmd == I.GL_EPS_PYRO_FIRE:
            if len(a) != 2:
                return I.ICD_ST_BAD_LENGTH, b""
            allowed = self.mode != I.ICD_MODE_SAFE and not self.eps.low_battery
            ok = self.pyro.fire(struct.unpack("<H", a)[0], allowed=allowed)
            return (I.ICD_ST_OK if ok else I.GL_ST_REFUSED), b""
        return I.ICD_ST_UNKNOWN_CMD, b""

    def _flush_outbox(self) -> None:
        while self.outbox:
            st = self._try("ttc queue_tx", self.ttc.queue_tx, self.outbox[0])
            if st is None or st == I.ICD_ST_QUEUE_FULL:
                break  # try again next tick
            if st != I.ICD_ST_OK:
                log.warning("TTC refused downlink packet: %s", status_name(st))
            self._write_record("ground", {"t": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                                          "dir": "down", "hex": self.outbox[0].hex(), "status": st})
            self.outbox.pop(0)
        del self.outbox[64:]  # never grow without bound

    # ------------------------------------------------------------ modes / beacon
    def _mode_logic(self) -> None:
        if self.eps.low_battery and self.mode != I.ICD_MODE_SAFE:
            self.set_obc_mode(I.ICD_MODE_SAFE, "low battery")
        elif self.mode == I.ICD_MODE_SAFE and self.safe_reason == "low battery" and not self.eps.low_battery:
            self.set_obc_mode(I.ICD_MODE_NOMINAL, "battery recovered")

        st, ctrl = self.last["adcs_status"], self.last["adcs_ctrl"]
        if (self.mode == I.ICD_MODE_NOMINAL and self.cfg["obc"]["adcs_auto_nominal"] and st is not None
                and st.mode == I.ICD_MODE_DETUMBLE and ctrl.get("detumbled")):
            log.info("ADCS detumbled (%.2f dps): commanding NOMINAL", ctrl.get("rate_dps", -1))
            self._try("adcs nominal", self.adcs.set_mode, I.ICD_MODE_NOMINAL)

    def _update_beacon(self) -> None:
        now = self.clock.monotonic()
        if now - self.last_beacon_update < self.cfg["obc"]["beacon_update_s"]:
            return
        self.last_beacon_update = now
        e, a, t = self.last["eps"], self.last["adcs_status"], self.last["ttc_status"]
        pkt = ground.build_obc_beacon(
            seq=self.beacon_seq, uptime_s=self.uptime_s(), obc_mode=self.mode,
            vbatt_mv=e.get("vbus_mV", 0), ibus_ma=e.get("current_mA", 0),
            adcs_mode=a.mode if a else 0xFF, ttc_mode=t.mode if t else 0xFF,
            adcs_rate_cdps=int(self.last["adcs_ctrl"].get("rate_dps", 0) * 100),
            comm_flags=self.comm_flags(), cpu_temp_c=self.cpu_temp_c() or 0,
            lux=self._lux_u16())
        self.beacon_seq = (self.beacon_seq + 1) & 0xFF
        self._try("ttc set_beacon", self.ttc.set_beacon, pkt)

    # ------------------------------------------------------------ io
    def _write_record(self, kind: str, rec: dict) -> None:
        day = datetime.now(timezone.utc).strftime("%Y-%m-%d")
        path = self.data_dir / ("telemetry" if kind == "telemetry" else ".") / f"{kind}_{day}.jsonl"
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(json.dumps(rec, default=str) + "\n")
        except OSError as e:
            log.error("cannot write %s: %s", path, e)

    def _sd_notify(self, msg: str) -> None:
        addr = os.environ.get("NOTIFY_SOCKET")
        if not addr or not hasattr(socket, "AF_UNIX"):
            return
        try:
            if self._notify_sock is None:
                self._notify_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            if addr.startswith("@"):
                addr = "\0" + addr[1:]
            self._notify_sock.sendto(msg.encode(), addr)
        except OSError:
            pass
