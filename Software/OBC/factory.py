"""Build the OBC on real hardware or on the PC simulation."""
from __future__ import annotations

import subprocess
import sys
from typing import Optional, Tuple

import icd_defs as I

from .comms.bus import LinuxI2CBus, SimBus, SimLib
from .EPS.EPS import FakeINA226, GpioOut, Pyro, PyroConfig
from .obc_app import Clock, ObcApp
from .Payload.Payload import FakeTSL2591

# Stand-in for rpicam-still in the simulation: writes a tiny JPEG to the -o path.
SIM_CAMERA_CMD = [sys.executable, "-c",
                  "import sys; a = sys.argv; open(a[a.index('-o') + 1], 'wb').write(b'\\xff\\xd8SIM\\xff\\xd9')"]


class SimClock(Clock):
    """Simulated time: sleeping advances the firmware simulation."""

    def __init__(self, sim: SimLib):
        self.sim = sim

    def monotonic(self) -> float:
        return self.sim.now_ms() / 1000.0

    def sleep(self, s: float) -> None:
        self.sim.run_ms(int(round(s * 1000)))


def make_pyro(cfg: dict, dry_run: bool) -> Pyro:
    p = cfg["pyro"]
    pc = PyroConfig(gpio=p["gpio"], enabled=p["enabled"], max_ms=p["max_ms"], arm_window_s=p["arm_window_s"])
    return Pyro(pc, GpioOut(pc.gpio, dry_run=dry_run or not pc.enabled))


def build_real(cfg: dict) -> ObcApp:
    link_bus = LinuxI2CBus(cfg["link"]["bus"])
    sbus_n = cfg["sensors"]["bus"]
    sensor_bus = link_bus if sbus_n == cfg["link"]["bus"] else LinuxI2CBus(sbus_n)

    def reboot():
        subprocess.run(["sudo", "systemctl", "reboot"], check=False)

    return ObcApp(cfg, link_bus, sensor_bus, make_pyro(cfg, dry_run=False), Clock(), reboot)


def sim_sensors(cfg: dict) -> dict:
    """Fake EPS/payload chips for the sensor bus (INA226 shunt matches the config)."""
    return {I.ICD_ADDR_INA226_EPS: FakeINA226(shunt_ohm=cfg["sensors"]["shunt_ohm"]),
            I.ICD_ADDR_TSL2591: FakeTSL2591()}


def build_sim(cfg: dict, seed: int = 1, sim: Optional[SimLib] = None) -> Tuple[ObcApp, SimLib]:
    sim = sim or SimLib()
    sim.init(seed)
    bus = SimBus(sim, sim_sensors(cfg))
    cam = cfg.setdefault("payload", {}).setdefault("camera", {})
    cam.setdefault("command", SIM_CAMERA_CMD)
    app = ObcApp(cfg, bus, bus, make_pyro(cfg, dry_run=True), SimClock(sim))
    return app, sim
