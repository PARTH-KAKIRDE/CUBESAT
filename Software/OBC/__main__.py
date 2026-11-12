"""OBC command line. Run from Software/:

  python3 -m OBC run [--config OBC/config/obc.toml] [--sim] [--ticks N]
  python3 -m OBC scan                       which expected devices answer
  python3 -m OBC ping adcs|ttc [-n 100]
  python3 -m OBC status adcs|ttc
  python3 -m OBC tlm adcs|ttc [page]
  python3 -m OBC mode adcs|ttc safe|detumble|nominal
Add --sim to any command to talk to the PC simulation instead of the Pi bus.
"""
from __future__ import annotations

import argparse
import json
import logging
import signal
import sys
import time

import icd_defs as I
from icd_link import BusError, LinkError, SlaveError, SlaveLink, status_name

from . import config as cfgmod
from .bus import LinuxI2CBus, SimBus, SimLib
from .slaves import ADCS_FAULT_NAMES, TTC_FAULT_NAMES, AdcsClient, TtcClient, fault_names

MODES = {"safe": I.ICD_MODE_SAFE, "detumble": I.ICD_MODE_DETUMBLE, "nominal": I.ICD_MODE_NOMINAL}


def clients(args, cfg):
    if args.sim:
        from .factory import sim_sensors

        sim = SimLib()
        sim.init(1)
        sim.run_ms(3000)  # let the simulated slaves take a few samples
        bus = SimBus(sim, sim_sensors(cfg))
    else:
        bus = LinuxI2CBus(cfg["link"]["bus"])
    L = cfg["link"]
    return {"adcs": AdcsClient(SlaveLink(bus, L["adcs_addr"], "ADCS")),
            "ttc": TtcClient(SlaveLink(bus, L["ttc_addr"], "TTC"))}, bus


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m OBC", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", help="TOML config (default: built-in defaults)")
    ap.add_argument("--sim", action="store_true", help="use the PC firmware simulation")
    ap.add_argument("-v", "--verbose", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--ticks", type=int, help="stop after N ticks (default: forever)")
    sub.add_parser("scan")
    p = sub.add_parser("ping"); p.add_argument("target", choices=["adcs", "ttc"]); p.add_argument("-n", type=int, default=1)
    s = sub.add_parser("status"); s.add_argument("target", choices=["adcs", "ttc"])
    t = sub.add_parser("tlm"); t.add_argument("target", choices=["adcs", "ttc"]); t.add_argument("page", type=int, nargs="?")
    m = sub.add_parser("mode"); m.add_argument("target", choices=["adcs", "ttc"]); m.add_argument("mode", choices=list(MODES))
    args = ap.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    cfg = cfgmod.load(args.config)

    if args.cmd == "run":
        from .factory import build_real, build_sim

        if args.sim:
            import tempfile

            cfg["obc"]["data_dir"] = tempfile.mkdtemp(prefix="cubesat_sim_")
            app, _ = build_sim(cfg)
            print(f"simulation, data in {cfg['obc']['data_dir']}")
        else:
            app = build_real(cfg)
        # systemd stops the service with SIGTERM: unwind through run()'s finally,
        # so a running capture is stopped and the payload log is flushed.
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        app.run(ticks=args.ticks)
        return 0

    cl, bus = clients(args, cfg)
    try:
        if args.cmd == "scan":
            sensor_bus = bus if args.sim else LinuxI2CBus(cfg["sensors"]["bus"])
            devices = [("ADCS", bus, cfg["link"]["adcs_addr"]), ("TT&C", bus, cfg["link"]["ttc_addr"]),
                       ("INA226", sensor_bus, cfg["sensors"]["ina226_addr"]),
                       ("TSL2591", sensor_bus, I.ICD_ADDR_TSL2591)]
            # Fitted later: listed once their address is uncommented in the ICD.
            if hasattr(I, "ICD_ADDR_INA226_BATT"):
                devices.append(("INA226#2", sensor_bus, I.ICD_ADDR_INA226_BATT))
            if hasattr(I, "ICD_ADDR_VEML6075"):
                devices.append(("VEML6075", sensor_bus, I.ICD_ADDR_VEML6075))
            probe_reg = {"INA226": 0xFE, "INA226#2": 0xFE, "TSL2591": 0xB2, "VEML6075": 0x0C}
            for name, b, addr in devices:
                try:
                    if name in ("ADCS", "TT&C"):
                        c = cl["adcs" if name == "ADCS" else "ttc"]
                        v = c.version()
                        print(f"  {name:8s} 0x{addr:02X}  OK  fw {v['fw']} ICD {v['icd']}")
                    else:
                        b.write_read(addr, bytes([probe_reg[name]]), 1)
                        print(f"  {name:8s} 0x{addr:02X}  answers")
                except (BusError, LinkError, SlaveError) as e:
                    print(f"  {name:8s} 0x{addr:02X}  -- no answer ({e})")
            return 0
        c = cl[args.target]
        if args.cmd == "ping":
            t0, bad = time.monotonic(), 0
            for _ in range(args.n):
                try:
                    c.ping()
                except (LinkError, SlaveError) as e:
                    bad += 1
                    print("  error:", e)
            dt = time.monotonic() - t0
            st = c.link.stats
            print(f"{args.n} pings, {bad} failed, {st.busy_retries} BUSY re-reads, "
                  f"{st.read_crc_errors} read CRC errors, {1000 * dt / args.n:.1f} ms each")
            return 1 if bad else 0
        if args.cmd == "status":
            st = c.status()
            d = st.as_dict()
            d["fault_names"] = fault_names(st.faults, ADCS_FAULT_NAMES if args.target == "adcs" else TTC_FAULT_NAMES)
            d["version"] = c.version()
            print(json.dumps(d, indent=2))
            return 0
        if args.cmd == "tlm":
            if args.target == "adcs":
                out = {0: c.sensors, 1: c.sun_mtq, 2: c.control}
            else:
                out = {0: c.radio, 1: c.counters}
            pages = [args.page] if args.page is not None else list(out)
            for pg in pages:
                print(f"page {pg}:", json.dumps(out[pg](), indent=2))
            return 0
        if args.cmd == "mode":
            st = c.set_mode(MODES[args.mode])
            print(status_name(st))
            return 0 if st == I.ICD_ST_OK else 1
    except (LinkError, SlaveError) as e:
        print("error:", e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
