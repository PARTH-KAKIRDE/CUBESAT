"""Shared fixtures. Run from Software/:  python3 -B -m pytest -q OBC/tests

The simulation tests run the real ADCS / TT&C firmware logic, compiled for the
PC (libcubesat_sim). They are skipped unless Software/build/host/libcubesat_sim.so
exists or CUBESAT_SIM_LIB points at one. Build it from Software/ with:
    python3 firmware/build.py sim
"""
import pytest

from OBC import config as cfgmod  # first import of the package: puts ICD/python on sys.path
from OBC.comms.bus import SimLib, default_sim_lib

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
# TT&C emergency key of the host build (firmware/host/ttc_key_test.c).
TTC_TEST_KEY = bytes(range(32))


def make_exif_jpeg(thumb: bytes, endian: str = "<", body: bytes = b"main image data") -> bytes:
    """A minimal JPEG whose EXIF IFD1 holds `thumb`, as rpicam-still --thumb writes it."""
    import struct
    bo = b"II" if endian == "<" else b"MM"
    ifd1 = 14
    tiff = bo + struct.pack(endian + "HI", 42, 8) + struct.pack(endian + "HI", 0, ifd1)
    data_at = ifd1 + 2 + 2 * 12 + 4
    tiff += struct.pack(endian + "H", 2)
    tiff += struct.pack(endian + "HHII", 0x0201, 4, 1, data_at)
    tiff += struct.pack(endian + "HHII", 0x0202, 4, 1, len(thumb))
    tiff += struct.pack(endian + "I", 0) + thumb
    app1 = b"Exif\x00\x00" + tiff
    return (b"\xff\xd8" + b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1
            + b"\xff\xda\x00\x02" + body + b"\xff\xd9")


@pytest.fixture
def cfg(tmp_path):
    c = cfgmod.load(None)
    c["obc"]["data_dir"] = str(tmp_path / "data")
    key_file = tmp_path / "ground_key"
    key_file.write_text(TEST_KEY.hex())
    c["ground"]["key_file"] = str(key_file)
    return c
