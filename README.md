# AIS Manager

C++17 service that communicates with a marine AIS transponder (e.g. Comar R220U) over TCP, UDP, or RS-232 serial, decodes NMEA 0183 !AIVDM / !AIVDO sentences, and publishes vessel data to an MQTT broker.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                     ais_manager                         │
│                                                         │
│  receiveLoop ──► AisParser ──► vessel table (per MMSI)  │
│                                                         │
│  publishLoop ──► MQTT  uuv/ais   (vessel JSON, 1 Hz)   │
│               ──► MQTT  ais/status (health, 10 s)       │
│                                                         │
│  gpsOutputLoop ◄── MQTT  uuv/gnss/gga  (optional)      │
│               ──► transport (shared $GPRMC to device)   │
└─────────────────────────────────────────────────────────┘
         │  TCP / UDP / RS-232
         ▼
  AIS transponder (Comar R220U / simulator)
```

### Threads

| Thread | Role |
|--------|------|
| `receiveLoop` | Connects/reconnects transport, reads lines, validates CRC, parses AIS, updates vessel table |
| `publishLoop` | Publishes vessel JSON to `uuv/ais` every `publish_interval_ms` ms; status to `ais/status` every `status_interval_sec` s |
| `gpsOutputLoop` | (optional) Reads latest GGA from internal buffer, converts to $GPRMC, sends to AIS device over shared transport |

### Shared Transport (`shared_with`)

When an output channel (e.g. `gps`) sets `transport.shared_with: "ais_rx"`, the manager reuses the same physical connection for both receiving AIS data and sending GPS sentences. This works for bidirectional links (TCP client, TCP server, serial). For UDP receive-only, a separate transmit transport should be configured.

---

## Source Files

| File | Purpose |
|------|---------|
| `src/Config.hpp / .cpp` | Configuration structures and JSON/INI loaders |
| `src/AisParser.hpp` | Header-only AIS 6-bit payload decoder (types 1,2,3,5,18,21,24) |
| `src/AisManager.hpp / .cpp` | Main manager — receive, parse, publish, GPS output |
| `src/Transport.hpp / .cpp` | ITransport interface + TCP client/server, UDP server, serial implementations |
| `src/MqttClient.hpp / .cpp` | libmosquitto wrapper |
| `src/main.cpp` | Entry point, signal handling |

---

## AIS Packet Structure

AIS uses NMEA 0183 sentences starting with `!`:

```
!AIVDM,1,1,,A,13nlh60P00PD9bVMlCO8h=b0<0Ne,0*37
  │    │ │  │ │                              │  │
  │    │ │  │ └─ 6-bit encoded payload       │  └─ NMEA checksum (XOR)
  │    │ │  └─── VHF channel (A or B)        └──── fill bits
  │    │ └─────── fragment sequence number
  │    └─────────  fragment count (1 = single sentence)
  └──────────────  sentence formatter (AIVDM=other vessel, AIVDO=own vessel)
```

### 6-bit ASCII Encoding

Each payload character encodes 6 bits:
```
value = ASCII(char) - 48
if value > 40: value -= 8
→ 6-bit value appended MSB-first to bit stream
```

### Decoded Message Types

| Type | Name | Key Fields |
|------|------|-----------|
| 1/2/3 | Position Report Class A | MMSI, lat, lon, SOG, COG, heading, nav status, ROT |
| 5 | Static & Voyage Data | MMSI, IMO, vessel name, call sign, ship type, destination, ETA |
| 18 | Class B Position Report | MMSI, lat, lon, SOG, COG, heading |
| 21 | Aid-to-Navigation | MMSI, AtoN type, name, lat, lon |
| 24 | Class B Static Data | MMSI, vessel name (A), call sign, ship type, dimensions (B) |

---

## Configuration

Both JSON and INI formats are supported.

### `ais_config.json`

```json
{
  "mqtt": {
    "broker": "host.docker.internal",
    "port": 1883,
    "client_id": "ais_manager",
    "topics": {
      "ais":      "uuv/ais",       // published: vessel table
      "status":   "ais/status",    // published: health stats
      "gnss_gga": "uuv/gnss/gga"  // subscribed: GPS for GPS output
    }
  },
  "ais": {
    "publish_interval_ms":    1000,   // vessel publish rate
    "data_timeout_sec":       5.0,    // mark stale if no data this long
    "status_interval_sec":    10,     // status publish rate
    "publish_raw_ais":        false,  // include raw sentence in JSON
    "validate_crc":           true,   // drop sentences with bad checksum
    "require_position_valid": false,  // only publish vessels with lat/lon
    "send_init_on_reconnect": false,  // re-send init commands on every reconnect
    "init_commands": [],              // sent once on first connect
    "ais_device_rx": {
      "name": "ais_rx",             // internal name (used by shared_with)
      "transport": {
        "type": "tcp_client",       // tcp_client | tcp_server | udp_server | serial
        "host": "host.docker.internal",
        "port": 4008,
        "reconnect_delay_sec": 3,
        "read_timeout_ms": 1000
      }
    },
    "output_channels": {
      "gps": {
        "enabled": false,
        "send_interval_ms": 1000,
        "transport": {
          "shared_with": "ais_rx"   // reuse RX connection for TX
        }
      }
    }
  }
}
```

### `ais_config.ini` (alternative)

```ini
[mqtt]
broker     = host.docker.internal
port       = 1883
client_id  = ais_manager

[mqtt.topics]
ais      = uuv/ais
status   = ais/status
gnss_gga = uuv/gnss/gga

[ais]
publish_interval_ms = 1000
data_timeout_sec    = 5.0

[ais.device]
type = tcp_client
host = host.docker.internal
port = 4008

[ais.gps_output]
enabled     = false
shared_with = ais_rx
```

### Transport Types

| Type | Config Keys Required | Direction |
|------|---------------------|-----------|
| `tcp_client` | `host`, `port` | bidirectional |
| `tcp_server` | `bind_host`, `bind_port` | bidirectional (waits for device to connect) |
| `udp_server` | `bind_host`, `bind_port` | receive only (TX needs `host`+`port`) |
| `serial` | `serial_port`, `serial_baud` | bidirectional |

---

## MQTT Topics

### `uuv/ais` — Vessel Table

Published every `publish_interval_ms` ms. Contains all vessels seen within `data_timeout_sec × 60` seconds:

```json
{
  "ts": 1717430400.123,
  "vessel_count": 3,
  "vessels": [
    {
      "mmsi": 338123456,
      "msg_type": 1,
      "recv_ts": 1717430399.987,
      "age_sec": 0.136,
      "lat": 18.92001,
      "lon": 72.84002,
      "sog": 12.5,
      "cog": 220.3,
      "heading": 219,
      "nav_status": 0,
      "vessel_name": "OCEAN PIONEER",
      "call_sign": "W3ABC",
      "ship_type": 70
    }
  ]
}
```

### `ais/status` — Health Statistics

Published every `status_interval_sec` seconds:

```json
{
  "ts": 1717430410.000,
  "connected": true,
  "data_valid": true,
  "last_data_ts": 1717430409.870,
  "data_age_sec": 0.13,
  "packets_received": 1425,
  "parse_errors": 0,
  "crc_errors": 0,
  "vessel_count": 4,
  "health": "ok"
}
```

`health` values: `ok` | `stale` | `no_data` | `disconnected`

---

## Building

### Prerequisites (Ubuntu 22.04 / 24.04)

```bash
sudo apt-get install -y build-essential cmake libmosquitto-dev nlohmann-json3-dev
```

### Build

```bash
cd ais_manager
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Binary: `build/ais_manager`

---

## Docker

### Build image

```bash
docker build -t ais_manager:latest .
```

### Run with default config

```bash
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  ais_manager:latest
```

### Run with custom config (volume mount)

```bash
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  -v $(pwd)/config/ais_config.json:/etc/ais_manager/ais_config.json:ro \
  ais_manager:latest
```

### docker-compose example

```yaml
version: "3.8"
services:
  mosquitto:
    image: eclipse-mosquitto:2
    ports: ["1883:1883"]
    volumes:
      - ./mosquitto.conf:/mosquitto/config/mosquitto.conf

  ais_manager:
    image: ais_manager:latest
    depends_on: [mosquitto]
    extra_hosts:
      - "host.docker.internal:host-gateway"
    volumes:
      - ./config/ais_config.json:/etc/ais_manager/ais_config.json:ro
    restart: unless-stopped
```

---

## Testing

### 1. Start the simulator

```bash
# Default: mix of all message types, port 4008, 1 s interval
python test_ais.py

# Type 1 only (Class A position), faster rate
python test_ais.py --format 1 --interval 0.5

# Class B vessels
python test_ais.py --format 18 --port 4009
```

The simulator:
- Listens on TCP (ais_manager connects to it)
- Drains init commands sent by the manager
- Streams realistic !AIVDM sentences with 4 simulated vessels
- Displays any $GPRMC sentences received from the GPS output channel

### 2. Start ais_manager (native)

```bash
./build/ais_manager config/ais_config.json
```

Expected console output:
```
AIS Manager starting
  MQTT:             host.docker.internal:1883
  Transport:        tcp_client
  Publish interval: 1000 ms
[AisManager] Connected
[AisManager] MMSI=338123456 type=1 lat=18.92001 lon=72.84002 sog=12.5kn
[AisManager] MMSI=235678901 type=18 lat=18.85002 lon=72.92001 sog=8.2kn
```

### 3. Monitor MQTT output

```bash
# Subscribe to all AIS topics
mosquitto_sub -h localhost -t "uuv/ais" -t "ais/status" -v

# Pretty-print vessel table
mosquitto_sub -h localhost -t "uuv/ais" | python -m json.tool
```

### 4. Docker end-to-end test

```bash
# Terminal 1: simulator
python test_ais.py --port 4008

# Terminal 2: manager in Docker
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  ais_manager:latest

# Terminal 3: watch MQTT
mosquitto_sub -h localhost -t "#" -v
```

---

## Comar R220U Device Notes

The Comar R220U outputs standard !AIVDM sentences at 38400 baud over RS-232:

```ini
[ais.device]
type         = serial
serial_port  = /dev/ttyUSB0
serial_baud  = 38400
```

For Docker, pass the serial device:
```bash
docker run --rm --device /dev/ttyUSB0 ais_manager:latest
```

The device outputs AIS sentences continuously without requiring init commands. The `init_commands` array can be left empty.

---

## Sample AIS Packets

### Type 1 — Class A Position Report
```
!AIVDM,1,1,,A,13nlh60P00PD9bVMlCO8h=b0<0Ne,0*37
```
Decoded: MMSI=227006760, lat=48.388, lon=-4.463, SOG=0.0, COG=211.9, heading=157

### Type 5 — Static & Voyage Data (2 fragments)
```
!AIVDM,2,1,3,A,55?P`d02>H9aE<H4eEP00000000000000000000t2P5540Ht0000000000000,0*34
!AIVDM,2,2,3,A,00000000000,2*22
```
Decoded: IMO=9123456, Name=OCEAN PIONEER, Callsign=W3ABC, Type=70 (Cargo)

### Type 18 — Class B Position Report
```
!AIVDM,1,1,,B,B3m:H300FkdP?HaP07dJi`00H0<D,0*0E
```
Decoded: MMSI=338123456, lat=18.920, lon=72.840, SOG=12.5, COG=220.0

### Type 24A — Class B Static (Name)
```
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5hFt5hFt0<tqhFt5hFt0,0*2A
```

### Type 24B — Class B Static (Callsign/Dimensions)
```
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5hFt5hFt0<tqhFt5hFt0,0*2A
```

---

## Navigation Status Codes

| Code | Meaning |
|------|---------|
| 0 | Under way using engine |
| 1 | At anchor |
| 2 | Not under command |
| 3 | Restricted manoeuvrability |
| 5 | Moored |
| 7 | Engaged in fishing |
| 8 | Under way sailing |
| 15 | Undefined |

## Ship Type Codes (selected)

| Code | Type |
|------|------|
| 30 | Fishing |
| 60-69 | Passenger |
| 70-79 | Cargo |
| 80-89 | Tanker |
| 90-99 | Other |
