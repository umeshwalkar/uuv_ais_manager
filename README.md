# AIS Manager

C++17 service that communicates with one or more marine AIS transponders (e.g. Comar R220U) over TCP, UDP, or RS-232 serial, decodes NMEA 0183 `!AIVDM` / `!AIVDO` sentences, and publishes per-device vessel data to an MQTT broker.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                            ais_manager                                   │
│                                                                          │
│  Transport Pool  ─────────────────────────────────────────────────────  │
│  [ch1: tcp:4008]   [ch2: tcp:4009]   [serial1: /dev/ttyUSB0]  …        │
│       │                  │                                               │
│  ┌────▼──────────┐  ┌────▼──────────┐                                   │
│  │   AisDevice 1  │  │   AisDevice 2  │   (one per enabled device)      │
│  │  (ais1)        │  │  (ais2)        │                                  │
│  │  rxLoop        │  │  rxLoop        │  ← connect/reconnect transport   │
│  │  AisParser     │  │  AisParser     │  ← decode !AIVDM/!AIVDO         │
│  │  vessel table  │  │  vessel table  │  ← per-MMSI map                 │
│  │  ggaOutputLoop │  │  ggaOutputLoop │  ← $GPGGA → same transport      │
│  └───────┬────────┘  └───────┬────────┘                                 │
│          │                   │                                           │
│  AisManager::publishLoop ────┴──────────────────────────────────────── │
│    per-device vessel JSON ──► MQTT  uuv/ais  (per publish_interval_ms)  │
│    status JSON            ──► MQTT  ais/status (every status_interval)  │
│                                                                          │
│  MQTT uuv/gnss/gga ──► AisManager::setGga() ──► each device.setGga()   │
└──────────────────────────────────────────────────────────────────────────┘
```

### Thread model

Each enabled `AisDevice` spawns:

| Thread | Condition to start | Role |
|--------|--------------------|------|
| `rxLoop` | always (if device enabled) | Connects transport, reads `!AIVDM`/`!AIVDO`, validates checksum, parses, updates vessel table. Idles if transport disabled. |
| `ggaOutputLoop` | `output_channels.gga.enabled && transport.enabled` | Sends `$GPGGA` periodically on the same transport |

`AisManager` runs one additional thread:

| Thread | Role |
|--------|------|
| `publishLoop` | Ticks at 100 ms; publishes each device's vessel JSON at its own `publish_interval_ms`; publishes `ais/status` every `status_interval_sec` |

### Transport pool and `shared_with`

All physical connections are defined once in `ais.transport[]` and identified by a string `id` (`"ch1"`, `"ch2"`, …).  Channels reference them by id via `shared_with`.

```
ais.transport.ch1  (tcp:4008, enabled=true)
        │
        ├── ais1.input_channels.aivdm.transport.shared_with = "ch1"  → RX
        └── ais1.output_channels.gga.transport.shared_with  = "ch1"  → TX (same TCP conn)
```

A single bidirectional TCP or serial connection therefore serves both directions simultaneously.

---

## Enabling / Disabling Logic

| Flag | Effect |
|------|--------|
| `transport.enabled = false` | Transport never opened. All channels using it are implicitly disabled. Device IS still reported in `ais/status` with `health: "transport_disabled"`. |
| `input_channels.aivdm.enabled = false` | Transport connects (may be needed for GGA output), but received lines are counted and discarded. `health: "channel_disabled"`. |
| `output_channels.gga.enabled = false` | GGA output thread not started. |
| `device.publish_enabled = false` | RX and GGA threads run normally. Vessels NOT published to `uuv/ais`. Device IS in `ais/status`. |
| `device.enabled = false` | **All** operations skipped (no connect, no parse, no publish). Device appears in `ais/status` with `health: "device_disabled"`. |

---

## Source Files

| File | Purpose |
|------|---------|
| [src/Config.hpp](src/Config.hpp) / [.cpp](src/Config.cpp) | `TransportDef` pool, `AivdmChannelConfig`, `GgaChannelConfig`, `AisDeviceConfig`, JSON/INI loaders |
| [src/AisParser.hpp](src/AisParser.hpp) | Header-only 6-bit AIS decoder; types 1/2/3, 5, 18, 21, 24; multi-part reassembly |
| [src/AisDevice.hpp](src/AisDevice.hpp) / [.cpp](src/AisDevice.cpp) | Per-device: owns `rxLoop` + `ggaOutputLoop` threads, vessel table, `setGga()` |
| [src/AisManager.hpp](src/AisManager.hpp) / [.cpp](src/AisManager.cpp) | Owns transport pool, creates/wires devices, runs `publishLoop` |
| [src/Transport.hpp](src/Transport.hpp) / [.cpp](src/Transport.cpp) | `ITransport` interface + TCP client/server, UDP server, serial RS-232 |
| [src/MqttClient.hpp](src/MqttClient.hpp) / [.cpp](src/MqttClient.cpp) | libmosquitto wrapper |
| [src/main.cpp](src/main.cpp) | Entry point, signal handling |

---

## AIS Packet Structure

```
!AIVDM,1,1,,A,13nlh60P00PD9bVMlCO8h=b0<0Ne,0*37
  │    │ │  │ │                              │  │
  │    │ │  │ └─ 6-bit encoded payload       │  └─ NMEA checksum (XOR of bytes between ! and *)
  │    │ │  └─── VHF channel (A or B)        └──── fill bits (0-5)
  │    │ └─────── fragment sequence number (1–9)
  │    └──────── fragment count (1 = single sentence)
  └────────────── AIVDM = other vessel  |  AIVDO = own vessel
```

### 6-bit ASCII Encoding

```
value = ASCII(char) - 48
if value > 40: value -= 8
→ 6-bit value, MSB first, appended to bit stream
```

### Decoded Message Types

| Type | Name | Key Fields |
|------|------|-----------|
| 1/2/3 | Position Report Class A | MMSI, lat, lon, SOG, COG, heading, nav status, ROT, timestamp |
| 5 | Static & Voyage Data | MMSI, IMO, vessel name, call sign, ship type, dimensions, ETA, destination |
| 18 | Standard Class B Position | MMSI, lat, lon, SOG, COG, heading |
| 21 | Aid-to-Navigation | MMSI, AtoN type, name, lat, lon |
| 24 | Class B Static Data | MMSI, vessel name (part A), call sign, ship type, dimensions (part B) |

---

## Configuration

Both JSON and INI formats are supported — pass the file path as the first argument to `ais_manager`.

### `ais_config.json` — annotated

```jsonc
{
  "mqtt": {
    "enabled": true,
    "broker":  "host.docker.internal",
    "port":    1883,
    "client_id": "ais_manager",
    "topics": {
      "ais":      "uuv/ais",       // published: per-device vessel tables
      "status":   "ais/status",    // published: health + per-device stats
      "gnss_gga": "uuv/gnss/gga"  // subscribed: GPS feed for GGA output
    }
  },
  "ais": {
    "status_interval_sec": 10,     // ais/status publish rate

    // ── Transport pool ────────────────────────────────────────────────────
    // Named entries; each channel references one by id via shared_with.
    // enabled=false → transport never opened; channels using it are disabled;
    //                 device still reports in ais/status.
    "transport": [
      {
        "id": "ch1", "enabled": true,
        "type": "tcp_client",
        "host": "host.docker.internal", "port": 4008,
        "connect_timeout_sec": 5, "reconnect_delay_sec": 3,
        "read_timeout_ms": 1000, "buffer_size_bytes": 1024
      },
      {
        "id": "ch2", "enabled": true,
        "type": "tcp_client",
        "host": "host.docker.internal", "port": 4009,
        "connect_timeout_sec": 5, "reconnect_delay_sec": 3,
        "read_timeout_ms": 1000, "buffer_size_bytes": 1024
      }
    ],

    // ── Devices ───────────────────────────────────────────────────────────
    "devices": [
      {
        "id": 1, "name": "ais1",
        "enabled": true,              // false → skip ALL; appears in status as device_disabled
        "sync_timeout_sec": 5.0,
        "send_init_on_reconnect": false,
        "init_commands": [],          // sent to device on first connect
        "publish_enabled": true,      // false → transport runs, MQTT vessel publish skipped
        "publish_raw_ais": false,     // include raw NMEA sentence in vessel JSON
        "publish_interval_ms": 1000,  // per-device uuv/ais publish rate
        "validate_checksum": true,    // drop sentences with bad checksum
        "input_channels": {
          "aivdm": {
            "enabled": true,
            "data_timeout_sec": 5.0,  // vessel marked stale after this
            "transport": { "shared_with": "ch1" }
          }
        },
        "output_channels": {
          "gga": {
            "enabled": false,          // true → send $GPGGA to transponder periodically
            "send_interval_ms": 1000,
            "data_timeout_sec": 2.0,   // skip if GPS data older than this
            "transport": { "shared_with": "ch1" }  // same connection as aivdm
          }
        }
      },
      {
        "id": 2, "name": "ais2",
        "enabled": true,
        "publish_enabled": true,
        "publish_interval_ms": 1000,
        "validate_checksum": true,
        "input_channels": {
          "aivdm": { "enabled": true, "data_timeout_sec": 5.0,
                     "transport": { "shared_with": "ch2" } }
        },
        "output_channels": {
          "gga": { "enabled": false, "send_interval_ms": 1000, "data_timeout_sec": 2.0,
                   "transport": { "shared_with": "ch2" } }
        }
      }
    ]
  }
}
```

### `ais_config.ini` — annotated

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
status_interval_sec = 10

; Transport pool — [ais.transport.<id>]
[ais.transport.ch1]
enabled = true
type    = tcp_client
host    = host.docker.internal
port    = 4008

[ais.transport.ch2]
enabled = true
type    = tcp_client
host    = host.docker.internal
port    = 4009

; AIS Sensor 1
[ais.device1]
id                  = 1
name                = ais1
enabled             = true
publish_enabled     = true
publish_interval_ms = 1000
validate_checksum   = true

[ais.device1.input.aivdm]
enabled          = true
data_timeout_sec = 5.0
shared_with      = ch1

[ais.device1.output.gga]
enabled          = false
send_interval_ms = 1000
data_timeout_sec = 2.0
shared_with      = ch1

; AIS Sensor 2
[ais.device2]
id                  = 2
name                = ais2
enabled             = true
publish_enabled     = true
publish_interval_ms = 1000
validate_checksum   = true

[ais.device2.input.aivdm]
enabled          = true
data_timeout_sec = 5.0
shared_with      = ch2

[ais.device2.output.gga]
enabled          = false
send_interval_ms = 1000
data_timeout_sec = 2.0
shared_with      = ch2
```

### Transport Types

| Type | Required Keys | Direction |
|------|--------------|-----------|
| `tcp_client` | `host`, `port` | Bidirectional (connects to device) |
| `tcp_server` | `bind_host`, `bind_port` | Bidirectional (device connects to manager) |
| `udp_server` | `bind_host`, `bind_port` | Receive only (add `host`+`port` for TX) |
| `serial` | `serial_port`, `serial_baud` | Bidirectional |

### Init commands

Commands in `init_commands` are sent to the device once on first connect (or on every reconnect if `send_init_on_reconnect = true`), with a 150 ms gap between each:

```json
"init_commands": ["$PMSK,0,0,0,0,0,0,0", "$PMSK,1,1,9600,0,0,0,0"]
```

INI equivalent:
```ini
[ais.device1.init_commands]
cmd1 = $PMSK,0,0,0,0,0,0,0
cmd2 = $PMSK,1,1,9600,0,0,0,0
```

---

## MQTT Topics

### `uuv/ais` — Vessel Data

Published at `publish_interval_ms` per device. Only published when `device.publish_enabled = true`.  
Each message is tagged with the source device so consumers know which sensor received each vessel.

```json
{
  "ts": 1717430400.123,
  "device_id": 1,
  "device_name": "ais1",
  "vessel_count": 2,
  "vessels": [
    {
      "mmsi": 338123456,
      "device_id": 1,
      "device_name": "ais1",
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
      "ship_type": 70,
      "imo": 9123456
    },
    {
      "mmsi": 235678901,
      "device_id": 1,
      "device_name": "ais1",
      "msg_type": 18,
      "recv_ts": 1717430399.921,
      "age_sec": 0.202,
      "lat": 18.85002,
      "lon": 72.92001,
      "sog": 8.2,
      "cog": 45.1,
      "heading": 44
    }
  ]
}
```

Optional fields present only when available: `lat`/`lon`, `sog`, `cog`, `heading`, `nav_status`, `vessel_name`, `call_sign`, `ship_type`, `imo`, `destination`, `raw` (if `publish_raw_ais = true`).

### `ais/status` — Health Statistics

Published every `status_interval_sec` seconds. Includes all configured devices (even disabled ones).

```json
{
  "ts": 1717430410.000,
  "health": "ok",
  "devices": [
    {
      "id": 1,
      "name": "ais1",
      "publish_enabled": true,
      "rx_transport_enabled": true,
      "tx_transport_enabled": false,
      "aivdm_ch_enabled": true,
      "gga_ch_enabled": false,
      "connected": true,
      "data_valid": true,
      "last_data_ts": 1717430409.870,
      "data_age_sec": 0.13,
      "packets_received": 1425,
      "crc_errors": 0,
      "vessel_count": 4,
      "gga_sent_count": 0,
      "health": "ok"
    },
    {
      "id": 2,
      "name": "ais2",
      "connected": true,
      "data_valid": true,
      "packets_received": 1389,
      "vessel_count": 4,
      "health": "ok"
    }
  ]
}
```

#### `health` values per device

| Value | Meaning |
|-------|---------|
| `ok` | Connected, data fresh, all channels enabled |
| `stale` | Connected but no data within `data_timeout_sec` |
| `no_data` | Connected, channel enabled, no packet received yet |
| `disconnected` | Transport enabled but TCP/serial not connected |
| `channel_disabled` | Transport connected but `input_channels.aivdm.enabled = false` |
| `transport_disabled` | `transport.enabled = false` in pool |
| `device_disabled` | `device.enabled = false` |

#### Overall `health`

| Value | Meaning |
|-------|---------|
| `ok` | All active devices healthy |
| `degraded` | At least one device not `ok` |
| `no_data` | Connected but no data from any device |
| `disconnected` | No device connected |

---

## Building

### Prerequisites (Ubuntu 22.04 / 24.04)

```bash
sudo apt-get install -y build-essential cmake libmosquitto-dev nlohmann-json3-dev
```

### Compile

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# binary: build/ais_manager
```

---

## Docker

### Build image

```bash
docker build -t ais_manager:latest .
```

### Run (default config)

```bash
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  ais_manager:latest
```

### Run with custom config

```bash
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  -v $(pwd)/config/ais_config.json:/etc/ais_manager/ais_config.json:ro \
  ais_manager:latest
```

### Serial device passthrough

```bash
docker run --rm \
  --device /dev/ttyUSB0 \
  --device /dev/ttyUSB1 \
  -v $(pwd)/config/ais_config.json:/etc/ais_manager/ais_config.json:ro \
  ais_manager:latest
```

### docker-compose

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

### 1. Start the dual simulator

`test_ais.py` runs **two independent TCP server instances** — one per AIS device — and streams realistic `!AIVDM` sentences with 8 simulated vessels split between them.

```bash
# Default: two servers on 4008 and 4009, mix of all message types, 1 s interval
python test_ais.py

# Faster Class-A only
python test_ais.py --format 1 --interval 0.5

# Explicit ports
python test_ais.py --port1 4008 --port2 4009

# Single format per server
python test_ais.py --format 18
```

The simulator:
- Drains any init commands sent by the manager on connect
- Streams `!AIVDM` types 1 (Class A), 18 (Class B), 5 (static, every 10 ticks), 24 (Class B static, every 15 ticks)
- Labels output `[AIS1]` / `[AIS2]` so you can see which device each packet came from
- Displays any `$GPGGA` sentences received from the GGA output channel

```
[AIS1] !AIVDM,1,1,,A,13m:H300FkdP...0*3F
[AIS2] !AIVDM,1,1,,A,B3m:H300Fkd...0*0E
[AIS1]   -> GPS from manager: $GPGGA,123519,1853.200,N,07250.400,E,...
```

### 2. Start ais_manager (native)

```bash
./build/ais_manager config/ais_config.json
```

Expected startup output:
```
[AisManager] Transport pool (2 entries):
  [ch1] type=tcp_client host.docker.internal:4008
  [ch2] type=tcp_client host.docker.internal:4009
[AisManager] Starting — 2 active device(s)
  [ais1] id=1 publish=yes aivdm=ch1 gga=off
  [ais2] id=2 publish=yes aivdm=ch2 gga=off
[ais1] RX loop started (transport=ch1)
[ais2] RX loop started (transport=ch2)
[ais1] Connected
[ais2] Connected
[ais1] MMSI=338123456 type=1 lat=18.92001 lon=72.84002 sog=12.5kn
[ais2] MMSI=477001234 type=1 lat=18.98002 lon=72.87001 sog=11.0kn
```

### 3. Monitor MQTT

```bash
# All AIS topics
mosquitto_sub -h localhost -t "uuv/ais" -t "ais/status" -v

# Pretty-print vessel JSON from ais1
mosquitto_sub -h localhost -t "uuv/ais" | python -m json.tool

# Status only
mosquitto_sub -h localhost -t "ais/status" | python -m json.tool
```

### 4. Docker end-to-end

```bash
# Terminal 1 — dual simulator
python test_ais.py

# Terminal 2 — manager
docker run --rm \
  --add-host host.docker.internal:host-gateway \
  ais_manager:latest

# Terminal 3 — watch all MQTT
mosquitto_sub -h localhost -t "#" -v
```

### 5. Test disable scenarios

```bash
# Disable ch2 transport (ais2 stops connecting but still reports in status)
# Edit ais_config.json: transport[1].enabled = false, restart manager

# Disable ais2 entirely (absent from vessel publish, shown as device_disabled in status)
# Edit ais_config.json: devices[1].enabled = false, restart manager

# Receive on ais1 but suppress MQTT vessel publish
# Edit: devices[0].publish_enabled = false
```

---

## Comar R220U Device Notes

The Comar R220U outputs standard `!AIVDM` sentences continuously at 38400 baud over RS-232. No init commands are required.

### Serial config (single device)

```json
"transport": [
  { "id": "com1", "enabled": true, "type": "serial",
    "serial_port": "/dev/ttyUSB0", "serial_baud": 38400,
    "read_timeout_ms": 1000 }
],
"devices": [
  { "id": 1, "name": "ais1", "enabled": true,
    "input_channels": { "aivdm": { "enabled": true, "transport": { "shared_with": "com1" } } },
    "output_channels": { "gga": { "enabled": true, "send_interval_ms": 1000,
                                   "transport": { "shared_with": "com1" } } }
  }
]
```

### Dual-device serial config

```json
"transport": [
  { "id": "com1", "enabled": true, "type": "serial", "serial_port": "/dev/ttyUSB0", "serial_baud": 38400 },
  { "id": "com2", "enabled": true, "type": "serial", "serial_port": "/dev/ttyUSB1", "serial_baud": 38400 }
],
"devices": [
  { "id": 1, "name": "ais1", "input_channels": { "aivdm": { "transport": { "shared_with": "com1" } } }, ... },
  { "id": 2, "name": "ais2", "input_channels": { "aivdm": { "transport": { "shared_with": "com2" } } }, ... }
]
```

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
Decoded: IMO=9123456, Vessel=OCEAN PIONEER, Callsign=W3ABC, Type=70 (Cargo)

### Type 18 — Class B Position Report
```
!AIVDM,1,1,,B,B3m:H300FkdP?HaP07dJi`00H0<D,0*0E
```
Decoded: MMSI=338123456, lat=18.920, lon=72.840, SOG=12.5, COG=220.0

### Type 24 — Class B Static (Part A + Part B)
```
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5hFt5hFt0...0,0*2A  ← Part A: vessel name
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5hFt5hFt0...1,0*2B  ← Part B: callsign, dimensions
```

---

## Navigation Status Codes (Type 1/2/3)

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
| 31–35 | Towing |
| 60–69 | Passenger |
| 70–79 | Cargo |
| 80–89 | Tanker |
| 90–99 | Other |
