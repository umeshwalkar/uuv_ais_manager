#!/usr/bin/env python3
"""
test_ais.py — AIS device simulator for testing ais_manager.

Runs two independent TCP server instances (AIS1 and AIS2) on configurable
ports.  When ais_manager connects to each:
  1. Drains any init commands sent by the manager.
  2. Streams !AIVDM sentences at the configured interval.
  3. Displays any $GPRMC/GGA sentences received from the GPS output channel.

Each server gets its own fleet half, so the manager sees different vessels
on each connection, proving the device_id / device_name tagging works.

Simulated message types:
  --format 1    Position Report Class A (types 1/2/3, dynamic)
  --format 18   Standard Class B Position (type 18, dynamic)
  --format 5    Static and Voyage Data (type 5, two fragments, every 10 ticks)
  --format 24   Class B Static Data (type 24 A+B, every 15 ticks)
  --format mix  Rotates through all types (default)

Usage examples:
  python test_ais.py                               # two servers, ports 4008/4009, mix
  python test_ais.py --format 1 --interval 0.5    # fast Class-A only
  python test_ais.py --port1 4008 --port2 4009    # explicit ports
"""

import argparse
import math
import random
import signal
import socket
import threading
import time
from datetime import datetime, timezone

running = True


def _sig(signum, frame):
    global running
    running = False


signal.signal(signal.SIGINT,  _sig)
signal.signal(signal.SIGTERM, _sig)


# ── AIS encoding helpers ──────────────────────────────────────────────────────

def nmea_checksum(body: str) -> str:
    cs = 0
    for c in body:
        cs ^= ord(c)
    return f"{cs:02X}"


def encode_6bit(bits: list) -> str:
    while len(bits) % 6 != 0:
        bits.append(0)
    chars = []
    for i in range(0, len(bits), 6):
        val = 0
        for b in bits[i:i+6]:
            val = (val << 1) | b
        if val >= 40:
            val += 8
        val += 48
        chars.append(chr(val))
    return "".join(chars)


def int_to_bits(value: int, length: int) -> list:
    return [(value >> i) & 1 for i in range(length - 1, -1, -1)]


def sint_to_bits(value: int, length: int) -> list:
    if value < 0:
        value = (1 << length) + value
    return int_to_bits(value & ((1 << length) - 1), length)


def str_to_ais_bits(text: str, length_chars: int) -> list:
    padded = (text.upper() + "@" * length_chars)[:length_chars]
    bits = []
    for c in padded:
        v = ord(c)
        if v >= 64:
            v -= 64
        bits.extend(int_to_bits(v, 6))
    return bits


def encode_lon(deg: float) -> list:
    return sint_to_bits(round(deg * 600000), 28)


def encode_lat(deg: float) -> list:
    return sint_to_bits(round(deg * 600000), 27)


def encode_sog(knots: float) -> list:
    return int_to_bits(min(int(knots * 10), 1022), 10)


def encode_cog(deg: float) -> list:
    return int_to_bits(min(int(deg * 10), 3599), 12)


def wrap_nmea(payload: str, fill: int, channel: str = "A",
              frag_count: int = 1, frag_seq: int = 1, seq_id: str = "") -> str:
    body = f"AIVDM,{frag_count},{frag_seq},{seq_id},{channel},{payload},{fill}"
    return f"!{body}*{nmea_checksum(body)}"


# ── message builders ──────────────────────────────────────────────────────────

def build_type1(mmsi, lat, lon, sog, cog, heading, nav_status=0):
    bits = []
    bits += int_to_bits(1, 6)
    bits += int_to_bits(0, 2)
    bits += int_to_bits(mmsi, 30)
    bits += int_to_bits(nav_status, 4)
    bits += sint_to_bits(-128, 8)
    bits += encode_sog(sog)
    bits += [0]
    bits += encode_lon(lon)
    bits += encode_lat(lat)
    bits += encode_cog(cog)
    bits += int_to_bits(heading if heading < 360 else 511, 9)
    bits += int_to_bits(datetime.now(timezone.utc).second, 6)
    bits += int_to_bits(0, 2)
    bits += int_to_bits(0, 3)
    bits += [0]
    bits += int_to_bits(0, 19)
    return wrap_nmea(encode_6bit(bits[:168]), 0)


def build_type18(mmsi, lat, lon, sog, cog, heading):
    bits = []
    bits += int_to_bits(18, 6)
    bits += int_to_bits(0, 2)
    bits += int_to_bits(mmsi, 30)
    bits += int_to_bits(0, 8)
    bits += encode_sog(sog)
    bits += [0]
    bits += encode_lon(lon)
    bits += encode_lat(lat)
    bits += encode_cog(cog)
    bits += int_to_bits(heading if heading < 360 else 511, 9)
    bits += int_to_bits(datetime.now(timezone.utc).second, 6)
    bits += int_to_bits(0, 2)
    bits += [1, 0, 0, 1, 0, 0, 0]
    bits += int_to_bits(0, 20)
    return wrap_nmea(encode_6bit(bits[:168]), 0)


def build_type5(mmsi, name, callsign, imo, ship_type=70, dest="PORT"):
    bits = []
    bits += int_to_bits(5, 6)
    bits += int_to_bits(0, 2)
    bits += int_to_bits(mmsi, 30)
    bits += int_to_bits(0, 2)
    bits += int_to_bits(imo, 30)
    bits += str_to_ais_bits(callsign, 7)
    bits += str_to_ais_bits(name, 20)
    bits += int_to_bits(ship_type, 8)
    bits += int_to_bits(50, 9)
    bits += int_to_bits(30, 9)
    bits += int_to_bits(10, 6)
    bits += int_to_bits(10, 6)
    bits += int_to_bits(1, 4)
    bits += int_to_bits(0, 4)
    bits += int_to_bits(0, 5)
    bits += int_to_bits(24, 5)
    bits += int_to_bits(60, 6)
    bits += int_to_bits(50, 8)
    bits += str_to_ais_bits(dest, 20)
    bits += [0, 0]
    while len(bits) < 426:
        bits.append(0)
    full = encode_6bit(bits[:426])
    half = len(full) // 2
    fill = (len(full) * 6 - 426) % 6
    seq  = str(random.randint(1, 9))
    return [wrap_nmea(full[:half], 0,    "A", 2, 1, seq),
            wrap_nmea(full[half:], fill, "A", 2, 2, seq)]


def build_type24(mmsi, name, callsign, ship_type=70):
    bits_a = (int_to_bits(24, 6) + int_to_bits(0, 2) + int_to_bits(mmsi, 30) +
              int_to_bits(0, 2) + str_to_ais_bits(name, 20))
    while len(bits_a) < 160:
        bits_a.append(0)

    bits_b = (int_to_bits(24, 6) + int_to_bits(0, 2) + int_to_bits(mmsi, 30) +
              int_to_bits(1, 2) + int_to_bits(ship_type, 8) +
              str_to_ais_bits("VENDOR", 7) + str_to_ais_bits(callsign, 7) +
              int_to_bits(50, 9) + int_to_bits(20, 9) +
              int_to_bits(5, 6)  + int_to_bits(5, 6) +
              int_to_bits(0, 10) + int_to_bits(0, 2))
    while len(bits_b) < 168:
        bits_b.append(0)

    return [wrap_nmea(encode_6bit(bits_a[:160]), 0),
            wrap_nmea(encode_6bit(bits_b[:168]), 0)]


# ── vessel fleet ──────────────────────────────────────────────────────────────

class Vessel:
    def __init__(self, mmsi, name, callsign, imo, lat, lon, cog, sog, ship_type=70):
        self.mmsi = mmsi; self.name = name; self.callsign = callsign
        self.imo  = imo;  self.lat  = lat;  self.lon       = lon
        self.cog  = cog;  self.sog  = sog;  self.ship_type = ship_type

    def update(self, dt):
        dist = (self.sog * 0.514444 * dt) / 111320.0
        rad  = math.radians(self.cog)
        self.lat += dist * math.cos(rad)
        self.lon += dist * math.sin(rad) / max(math.cos(math.radians(self.lat)), 1e-9)
        self.cog  = (self.cog + random.gauss(0, 0.3)) % 360.0
        self.sog  = max(0.1, self.sog + random.gauss(0, 0.05))

    def heading(self):
        return int(self.cog) % 360


# Fleet split across two devices: AIS1 sees vessels 0-3, AIS2 sees vessels 4-7
ALL_VESSELS = [
    # --- AIS1 fleet ---
    Vessel(338123456, "OCEAN PIONEER",  "W3ABC",  9123456, 18.9200, 72.8400, 220.0, 12.5, 70),
    Vessel(235678901, "BLUE HORIZON",   "MXYZ9",  9234567, 18.8500, 72.9200, 045.0,  8.2, 80),
    Vessel(419012345, "SEA FALCON",     "ATST1",  9345678, 19.0100, 72.8100, 310.0, 15.0, 70),
    Vessel(566543210, "PACIFIC STAR",   "9VPQ3",  9456789, 18.9500, 72.7800, 180.0,  6.5, 60),
    # --- AIS2 fleet ---
    Vessel(477001234, "ARABIAN TIDE",   "A6TK7",  9567890, 18.9800, 72.8700, 095.0, 11.0, 70),
    Vessel(311987654, "CORAL REEF",     "C6PZ5",  9678901, 18.8700, 72.9500, 270.0,  7.5, 90),
    Vessel(525432109, "NORTHERN STAR",  "9MNS2",  9789012, 19.0500, 72.7600, 150.0, 13.5, 80),
    Vessel(636210987, "GOLDEN GATE",    "VRGG1",  9890123, 18.9300, 72.8200, 320.0,  9.8, 60),
]


# ── helpers ───────────────────────────────────────────────────────────────────

def drain_init_commands(conn, drain_sec=1.5, label=""):
    conn.settimeout(0.1)
    deadline = time.monotonic() + drain_sec
    buf = b""
    while time.monotonic() < deadline:
        try:
            chunk = conn.recv(512)
            if not chunk:
                return False
            buf += chunk
        except socket.timeout:
            pass
        except OSError:
            return False
    if buf:
        for line in buf.decode("ascii", errors="replace").replace("\r\n", "\n").splitlines():
            line = line.strip()
            if line:
                print(f"[{label}]   <- Init cmd: {line!r}")
    else:
        print(f"[{label}]   (no init commands received)")
    return True


def try_read_gps(conn, label):
    conn.settimeout(0.0)
    try:
        data = conn.recv(256)
        if data:
            for line in data.decode("ascii", errors="replace").splitlines():
                line = line.strip()
                if line.startswith("$GPRMC") or line.startswith("$GPGGA"):
                    print(f"[{label}]   -> GPS from manager: {line}")
    except (BlockingIOError, socket.timeout, OSError):
        pass


# ── per-client session ────────────────────────────────────────────────────────

def handle_client(conn, addr, fleet, fmt, interval, label):
    print(f"[{label}] Client connected: {addr}  format={fmt}  fleet={len(fleet)} vessels")
    if not drain_init_commands(conn, label=label):
        return

    conn.settimeout(0.01)
    cycle = 0

    while running:
        packets = []
        v = fleet[cycle % len(fleet)]
        v.update(interval)

        if fmt in ("1", "mix"):
            packets.append(build_type1(v.mmsi, v.lat, v.lon, v.sog, v.cog, v.heading()))

        if fmt in ("18", "mix"):
            v2 = fleet[(cycle + 1) % len(fleet)]
            v2.update(interval)
            packets.append(build_type18(v2.mmsi, v2.lat, v2.lon, v2.sog, v2.cog, v2.heading()))

        if fmt in ("5", "mix") and cycle % 10 == 0:
            packets.extend(build_type5(v.mmsi, v.name, v.callsign, v.imo, v.ship_type))

        if fmt in ("24", "mix") and cycle % 15 == 0:
            v3 = fleet[(cycle + 2) % len(fleet)]
            packets.extend(build_type24(v3.mmsi, v3.name, v3.callsign, v3.ship_type))

        for pkt in packets:
            try:
                conn.sendall((pkt + "\r\n").encode("ascii"))
                print(f"[{label}] {pkt}")
            except (BrokenPipeError, ConnectionResetError, OSError) as exc:
                print(f"[{label}] Client {addr} disconnected: {exc}")
                return

        try_read_gps(conn, label)
        cycle += 1
        time.sleep(interval)

    print(f"[{label}] Session ended: {addr}")


# ── TCP server ────────────────────────────────────────────────────────────────

def run_server(host, port, fleet, fmt, interval, label):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(1)
        srv.settimeout(1.0)
        print(f"[{label}] Listening on {host}:{port}  format={fmt}  interval={interval}s")

        while running:
            try:
                conn, addr = srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                handle_client(conn, addr, fleet, fmt, interval, label)
            if running:
                print(f"[{label}] Waiting for next client...\n")

    print(f"[{label}] Server stopped")


# ── entry point ───────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(
        description="Dual AIS device simulator for testing ais_manager (two sensors)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Runs two independent TCP servers:
  AIS1 on --port1  (default 4008)  — vessels 0-3 of the simulated fleet
  AIS2 on --port2  (default 4009)  — vessels 4-7 of the simulated fleet

Both connect to the same ais_manager instance.
Published JSON will include device_id=1/device_name=ais1 or device_id=2/device_name=ais2.

Config match:
  ais_config.json → ais.devices[0].transport.port = 4008
                    ais.devices[1].transport.port = 4009
        """,
    )
    p.add_argument("--host",     default="0.0.0.0")
    p.add_argument("--port1",    type=int, default=4008, help="AIS1 TCP port (default: 4008)")
    p.add_argument("--port2",    type=int, default=4009, help="AIS2 TCP port (default: 4009)")
    p.add_argument("--interval", type=float, default=1.0)
    p.add_argument("--format",   default="mix",
                   choices=["1", "18", "5", "24", "mix"])
    args = p.parse_args()

    fleet1 = ALL_VESSELS[:4]
    fleet2 = ALL_VESSELS[4:]

    print(f"[test_ais] Starting dual AIS simulator")
    print(f"[test_ais] AIS1 on :{args.port1}  ({len(fleet1)} vessels)")
    print(f"[test_ais] AIS2 on :{args.port2}  ({len(fleet2)} vessels)")
    print(f"[test_ais] Format={args.format}  interval={args.interval}s")
    print(f"[test_ais] Press Ctrl+C to stop\n")

    t1 = threading.Thread(
        target=run_server,
        args=(args.host, args.port1, fleet1, args.format, args.interval, "AIS1"),
        daemon=True)
    t2 = threading.Thread(
        target=run_server,
        args=(args.host, args.port2, fleet2, args.format, args.interval, "AIS2"),
        daemon=True)

    t1.start()
    t2.start()

    try:
        while running and (t1.is_alive() or t2.is_alive()):
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass

    print("[test_ais] Shutdown complete")


if __name__ == "__main__":
    main()
