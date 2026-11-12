"""CubeSat OBC flight software (Raspberry Pi 4B). Run from Software/:  python3 -m OBC run

The ICD folder next to this one is the single source of truth: this package
imports its generated constants (ICD/python/icd_defs.py) and its I2C master
(ICD/python/icd_link.py) instead of keeping copies.
"""
import sys
from pathlib import Path

__version__ = "0.4.0"   # follows the ICD version it is built against

_ICD_PY = Path(__file__).resolve().parent.parent / "ICD" / "python"
if str(_ICD_PY) not in sys.path:
    sys.path.insert(0, str(_ICD_PY))
