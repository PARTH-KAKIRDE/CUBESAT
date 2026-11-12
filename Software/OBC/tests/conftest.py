"""Shared fixtures. Run from Software/:  python3 -B -m pytest -q OBC/tests

The simulation tests run the real ADCS / TT&C firmware logic, compiled for the
PC (libcubesat_sim). The firmware is not in Software/ yet, so they are skipped
unless Software/build/host/libcubesat_sim.so exists or CUBESAT_SIM_LIB points
at one (build it with the firmware's build.py sim).
"""
import pytest

from OBC import config as cfgmod  # first import of the package: puts ICD/python on sys.path
from OBC.bus import SimLib, default_sim_lib

import os
from pathlib import Path

SOFTWARE = Path(__file__).resolve().parents[2]


def _sim_lib_path():
    p = Path(os.environ.get("CUBESAT_SIM_LIB", default_sim_lib()))
    return p if p.exists() else None


@pytest.fixture(scope="session")
def sim_lib():
    p = _sim_lib_path()
    if p is None:
        pytest.skip("firmware simulation library not built (set CUBESAT_SIM_LIB)")
    return SimLib(p)


@pytest.fixture
def sim(sim_lib):
    sim_lib.init(11)
    return sim_lib


TEST_KEY = bytes(range(32))


@pytest.fixture
def cfg(tmp_path):
    c = cfgmod.load(None)
    c["obc"]["data_dir"] = str(tmp_path / "data")
    key_file = tmp_path / "ground_key"
    key_file.write_text(TEST_KEY.hex())
    c["ground"]["key_file"] = str(key_file)
    return c
