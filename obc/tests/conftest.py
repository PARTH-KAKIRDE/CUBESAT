import subprocess
import sys
from pathlib import Path

import pytest

OBC = Path(__file__).resolve().parents[1]
CODE = OBC.parent
sys.path.insert(0, str(OBC))

from cubesat_obc import config as cfgmod  # noqa: E402
from cubesat_obc.bus import SimLib, default_sim_lib  # noqa: E402


def _sim_sources_newer(lib: Path) -> bool:
    if not lib.exists():
        return True
    t = lib.stat().st_mtime
    srcs = list((CODE / "firmware").rglob("*.[ch]")) + list((CODE / "icd").glob("*.[ch]"))
    return any(s.stat().st_mtime > t for s in srcs if "vendor" not in s.parts and "stm32" not in s.parts)


@pytest.fixture(scope="session")
def sim_lib():
    lib = default_sim_lib()
    if _sim_sources_newer(lib):
        subprocess.run([sys.executable, str(CODE / "firmware" / "build.py"), "sim"], check=True)
    return SimLib(lib)


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
