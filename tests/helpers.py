"""
helpers.py — NMEA / AIS packet builders for ais_manager integration tests.

Re-exports the encoding primitives already present in test_ais.py and adds
test-specific utilities (checksum corruption, GPGGA builder, wait helpers).
"""

import sys
import os
import time

sys.path.insert(0, os.path.dirname(__file__))

# Re-export encoding helpers from the interactive simulator so we don't
# duplicate the bit-manipulation code.
from test_ais import (          # noqa: F401  (used by test modules)
    nmea_checksum,
    encode_6bit,
    int_to_bits,
    sint_to_bits,
    str_to_ais_bits,
    encode_lon,
    encode_lat,
    encode_sog,
    encode_cog,
    wrap_nmea,
    build_type1,
    build_type18,
    build_type5,
    build_type24,
)


# ── additional test helpers ───────────────────────────────────────────────────

def corrupt_checksum(sentence: str) -> str:
    """
    Flip the last hex digit of the NMEA checksum so the CRC is wrong
    but the sentence is otherwise well-formed.

    Input:  '!AIVDM,...*1A'
    Output: '!AIVDM,...*1B'   (last nibble incremented mod 16)
    """
    if "*" not in sentence:
        return sentence + "*XX"
    prefix, cs = sentence.rsplit("*", 1)
    cs = cs.strip()
    if len(cs) < 2:
        return sentence
    last = int(cs[-1], 16)
    bad_last = (last + 1) & 0xF
    return f"{prefix}*{cs[:-1]}{bad_last:X}"


def make_gpgga(lat_deg: float = 18.92, lon_deg: float = 72.84,
               time_str: str = "123519", num_sats: int = 8,
               altitude: float = 27.4) -> str:
    """Build a syntactically valid $GPGGA sentence."""
    lat_m = abs(lat_deg)
    lat_d = int(lat_m)
    lat_min = (lat_m - lat_d) * 60
    lat_dir = "N" if lat_deg >= 0 else "S"

    lon_m = abs(lon_deg)
    lon_d = int(lon_m)
    lon_min = (lon_m - lon_d) * 60
    lon_dir = "E" if lon_deg >= 0 else "W"

    body = (
        f"GPGGA,{time_str},"
        f"{lat_d:02d}{lat_min:07.4f},{lat_dir},"
        f"{lon_d:03d}{lon_min:07.4f},{lon_dir},"
        f"1,{num_sats:02d},1.0,{altitude:.1f},M,0.0,M,,"
    )
    return f"${body}*{nmea_checksum(body)}"


def wait_until(condition, timeout: float = 5.0, interval: float = 0.05) -> bool:
    """Poll *condition()* until it returns truthy or *timeout* elapses."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(interval)
    return False
