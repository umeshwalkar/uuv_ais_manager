# AIS Manager

C++17 service that communicates with one or more marine AIS transponders (e.g. Comar R220U) over TCP, UDP, or RS-232 serial, decodes NMEA 0183 `!AIVDM` sentences, and publishes per-device vessel data to an MQTT broker **immediately on each received packet** (event-driven, not on a timer).

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                              ais_manager                                     │
│                                                                              │
│  Transport Pool  ─────────────────────────────────────────────────────────  │
│  [ch1: tcp:4008]   [ch2: tcp:4009]   [serial1: /dev/ttyUSB0]  …            │
│       │                  │                                                   │
│  ┌────▼──────────┐  ┌────▼──────────┐                                       │
│  │  AisDevice 1  │  │  AisDevice 2  │   (one per enabled device)            │
│  │  rxLoop       │  │  rxLoop       │  ← connect/reconnect transport        │
│  │  AisParser    │  │  AisParser    │  ← decode !AIVDM only                 │
│  │  vessel table │  │  vessel table │  ← per-MMSI map                      │
│  │  ggaOutput    │  │  ggaOutput    │  ← $GPGGA → transport (periodic)      │
│  └──────┬────────┘  └──────┬────────┘                                       │
│         │ AivdmCallback    │ AivdmCallback                                  │
│         ▼                  ▼                                                 │
│  AisManager::publishAisVessels()  ──► MQTT  uuv/sensors/ais  (on each RX)  │
│                                                                              │
│  AisManager::publishLoop  ──► MQTT  ais/status  (every status interval)    │
│                                                                              │
│  MQTT uuv/gnss/gga  ──► AisManager::setGga()  ──► each device.setGga()    │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Thread model

Each enabled `AisDevice` spawns:

| Thread | Condition to start | Role |
|--------|--------------------|------|
| `rxLoop` | always (if device enabled) | Connects transport, reads lines, accepts only `!AIVDM,` sentences (all others logged at debug and discarded), validates checksum, parses, updates vessel table, fires `AivdmCallback` on each complete message |
| `ggaOutputLoop` | `output_channels.gga.enabled && transport.enabled` | Sends `$GPGGA` periodically on the configured transport |

`AisManager` runs one additional thread:

| Thread | Role |
|--------|------|
| `publishLoop` | Ticks at 100 ms; publishes `ais/status` on its configured interval; AIS vessel data is published via `AivdmCallback` instead |

### AivdmCallback — event-driven vessel publish

When `AisDevice::rxLoop` assembles a complete `!AIVDM` message (including the final fragment of multi-part sentences), it fires an `AivdmCallback` to `AisManager::publishAisVessels()`. That method immediately builds and publishes vessel JSON to `uuv/sensors/ais`.

- Multi-part messages (e.g. type 5) do **not** publish on each fragment — only on the final assembled message.
- `publish_interval_ms == 0` on the `ais` pub topic disables all vessel publishing.
- Lines that are not `!AIVDM,` (including `!AIVDO`, GPS sentences, etc.) are silently dropped, or logged at `DBG` if `input_channels.aivdm.debug` is enabled.

### Transport pool and channel `id` reference

All physical connections are defined once in `ais.transport[]`, each with a **unique string `id`** (`"ch1"`, `"ch2"`, …). Channel `transport` objects reference pool entries by this same `id` key.

```
ais.transport[].id = "ch1"   ← pool entry (string label, unique across the pool)
                │
                ├── ais.devices[0].input_channels.aivdm.transport.id = "ch1"  → RX
                └── ais.devices[0].output_channels.gga.transport.id  = "ch1"  → TX
                        (same bidirectional TCP/serial connection for both directions)
```

> **`id` disambiguation** — the key name `id` appears in three places with different types:
>
> | Location | Type | Example | Meaning |
> |----------|------|---------|---------|
> | `ais.transport[].id` | string | `"ch1"` | Unique label for the transport pool entry |
> | `ais.devices[].id` | integer | `1` | Numeric device identifier (appears in `ais/status` and vessel JSON) |
> | `input_channels.aivdm.transport.id` | string | `"ch1"` | Reference to a pool entry by its label |
> | `output_channels.gga.transport.id` | string | `"ch1"` | Reference to a pool entry by its label |

---

## Enabling / Disabling Logic

| Flag | Effect |
|------|--------|
| `transport.enabled = false` | Transport never opened. All channels using it are implicitly disabled. Device IS still reported in `ais/status` with `health: "transport_disabled"`. |
| `input_channels.aivdm.enabled = false` | Transport connects (may be needed for GGA output), but received lines are counted and discarded. `health: "channel_disabled"`. |
| `output_channels.gga.enabled = false` | GGA output thread not started. |
| `device.publish_enabled = false` | RX and GGA threads run normally. Vessels NOT published to `uuv/sensors/ais`. Device IS in `ais/status`. |
| `device.enabled = false` | **All** operations skipped (no connect, no parse, no publish). Device appears in `ais/status` with `health: "device_disabled"`. |
| `mqtt.topics.pub[name].publish_interval_ms = 0` | Publishing **disabled** for that topic. |
| `mqtt.topics.pub[name].publish_interval_ms 1–999` | Clamped to 1000 ms at load time. |

---

## Source Files

| File | Purpose |
|------|---------|
| [src/Config.hpp](src/Config.hpp) / [.cpp](src/Config.cpp) | `DebugConfig`, `TransportDef` pool, `MqttPubTopic`, `MqttSubTopic`, `AivdmChannelConfig`, `GgaChannelConfig`, `AisDeviceConfig`; JSON/INI loaders; `normalizePubIntervals()`; global debug override |
| [src/Logger.hpp](src/Logger.hpp) / [.cpp](src/Logger.cpp) | Tick-stamped, level-gated console logger; `LOG_ERR/WRN/INF/DBG` macros |
| [src/AisParser.hpp](src/AisParser.hpp) | Header-only 6-bit AIS decoder; types 1/2/3, 5, 18, 21, 24; multi-part reassembly |
| [src/AisDevice.hpp](src/AisDevice.hpp) / [.cpp](src/AisDevice.cpp) | Per-device: `rxLoop` (AIVDM-only filter), `ggaOutputLoop`, vessel table, `AivdmCallback`, `setGga()` |
| [src/AisManager.hpp](src/AisManager.hpp) / [.cpp](src/AisManager.cpp) | Owns transport pool, creates/wires devices, registers `AivdmCallback`, `publishAisVessels()`, `publishLoop`, MQTT subscribe for GGA |
| [src/Transport.hpp](src/Transport.hpp) / [.cpp](src/Transport.cpp) | `ITransport` interface (`readLine`, `send`) + TCP client/server, UDP server implementations |
| [src/MqttClient.hpp](src/MqttClient.hpp) / [.cpp](src/MqttClient.cpp) | libmosquitto wrapper; publish, subscribe, `MessageCallback` |
| [src/main.cpp](src/main.cpp) | Entry point: `Logger::init()`, debug config apply, signal handling |

---

## Logging

### Output format

Every log line carries a **system tick** (milliseconds since `main()` started), a level tag, and a fixed-width module identifier:

```
[<tick_ms>] [<LVL>] [<module>       ] <message>

[0000000012] [INF] [Main          ] Config: config/ais_config.json
[0000000015] [INF] [Main          ] Debug: ON  level=debug  (ERR+WRN always shown)
[0000000018] [INF] [AisManager    ] Transport pool — 2 entries
[0000000019] [INF] [AisManager    ]   [ch1] type=tcp_client host.docker.internal:4008
[0000000022] [INF] [ais1          ] Created  id=1  rx_transport=ch1[enabled]  gga_out=enabled
[0000000025] [INF] [AisMqttClient ] Connected to host.docker.internal:1883
[0000000027] [INF] [AisMqttClient ] Subscribed to 'uuv/gnss/gga'  qos=1
[0000001340] [DBG] [ais1          ] RX !AIVDM (47 bytes): !AIVDM,1,1,,A,13nlh6...
[0000001341] [INF] [ais1          ] MMSI=338123456  type= 1  pos=ok  sog=ok
[0000001341] [DBG] [ais1          ]   MMSI=338123456 lat=18.92001 lon=72.84002 sog=12.5kn cog=220.3 hdg=219 nav=0
[0000001341] [DBG] [AisManager    ] MQTT TX [uuv/sensors/ais] dev='ais1' vessels=1  312 bytes
[0000005000] [INF] [AisManager    ] Status  dev='ais1'  connected=yes  vessels=4  pkts=300  crc_err=0  health=ok
[0000005000] [DBG] [AisMqttClient ] TX [ais/status]  312 bytes  qos=1
[0000005000] [DBG] [AisManager    ] MQTT RX [uuv/gnss/gga]  82 bytes
```

### Log levels

| Level | Tag | Always printed? | Typical use |
|-------|-----|----------------|-------------|
| `ERROR` | `[ERR]` | Yes | Hard faults: transport connect failure, CRC error, MQTT publish failure, send failure |
| `WARN`  | `[WRN]` | Yes | Recoverable issues: connection lost, no data for N seconds, stale GPS data, MQTT reconnect, disabled transport/channel |
| `INFO`  | `[INF]` | When `debug.enabled=true` | Lifecycle: connected/disconnected, init commands, loop start/stop, periodic status summary |
| `DEBUG` | `[DBG]` | When `debug.enabled=true` | Payloads: raw NMEA sentences, decoded vessel fields, MQTT TX bytes, GGA TX content |

`ERR` and `WRN` are **always printed**, regardless of `debug.enabled` or `debug.level`.

### Global debug override

When `debug.enabled = true` in the top-level config, all per-topic `debug` flags and all per-channel (`aivdm`, `gga`) `debug` flags are **forced to `true`** at load time, regardless of their individual values in the config. This means a single `"debug": { "enabled": true }` is sufficient to enable all payload logging.

| `debug.enabled` | per-topic/channel `debug` | What prints |
|-----------------|--------------------------|-------------|
| `false` | any | ERR + WRN only |
| `true` | `false` (overridden to `true`) | ERR + WRN + INF + DBG |
| `true` | `true` | ERR + WRN + INF + DBG |

---

## AIS Packet Structure

```
!AIVDM,1,1,,A,13nlh60P00PD9bVMlCO8h=b0<0Ne,0*37
  │    │ │  │ │                              │  │
  │    │ │  │ └─ 6-bit encoded payload       │  └─ NMEA checksum (XOR of bytes between ! and *)
  │    │ │  └─── VHF channel (A or B)        └──── fill bits (0-5)
  │    │ └─────── fragment sequence number (1–9)
  │    └──────── fragment count (1 = single sentence)
  └────────────── AIVDM = other vessel  |  AIVDO = own vessel (ignored)
```

Only `!AIVDM,` sentences are processed. `!AIVDO` and all other lines received on the aivdm transport are discarded (logged at DBG if `input_channels.aivdm.debug` is enabled).

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
  // ── Debug / Logging ──────────────────────────────────────────────────────
  // enabled=true → forces all per-topic and per-channel debug flags to true;
  //                INF + DBG messages printed (ERR + WRN always printed)
  // level        → minimum level: debug | info | warn | error
  "debug": {
    "enabled": true,
    "level":   "debug"
  },

  "mqtt": {
    "enabled":   true,
    "broker":    "host.docker.internal",
    "port":      1883,
    "client_id": "ais_manager",
    "keepalive": 60,
    "qos":       1,
    "retain":    false,

    // ── Per-topic publish/subscribe config ────────────────────────────────
    // pub[].publish_interval_ms rules:
    //   0        → topic publishing disabled
    //   1–999    → clamped to 1000 at load time
    //   >=1000   → used as-is
    // The "ais" topic is event-driven (publishes on each !AIVDM receive);
    //   publish_interval_ms is only used for the 0=OFF gate.
    "topics": {
      "pub": [
        {
          "name": "status",  "topic": "ais/status",
          "debug": true,     "publish_interval_ms": 5000
        },
        {
          "name": "ais",     "topic": "uuv/sensors/ais",
          "debug": true,     "publish_interval_ms": 1000
        },
        {
          "name": "diagnostics", "topic": "ais/diagnostics",
          "debug": true,         "publish_interval_ms": 5000
        },
        {
          "name": "errors",  "topic": "ais/errors",
          "debug": true,     "publish_interval_ms": 1000
        }
      ],
      "sub": [
        {
          "name": "gnss_gga", "topic": "uuv/gnss/gga",
          "debug": true
        }
      ]
    }
  },

  "ais": {
    "status_interval_sec": 5,   // fallback if status pub topic has no interval

    // ── Transport pool ────────────────────────────────────────────────────
    "transport": [
      {
        "id": "ch1", "enabled": true,
        "type": "tcp_client",
        "host": "host.docker.internal", "port": 4008,
        "connect_timeout_sec": 5, "reconnect_delay_sec": 3,
        "read_timeout_ms": 1000,  "buffer_size_bytes": 1024
      },
      {
        "id": "ch2", "enabled": true,
        "type": "tcp_client",
        "host": "host.docker.internal", "port": 4009,
        "connect_timeout_sec": 5, "reconnect_delay_sec": 3,
        "read_timeout_ms": 1000,  "buffer_size_bytes": 1024
      }
    ],

    // ── Devices ───────────────────────────────────────────────────────────
    "devices": [
      {
        "id": 1, "name": "ais1",
        "enabled": true,
        "sync_timeout_sec": 5.0,
        "send_init_on_reconnect": false,
        "init_commands": [],
        "publish_enabled": true,     // false → transport runs, vessel publish skipped
        "publish_raw_ais": false,    // include raw NMEA sentence in vessel JSON
        "publish_interval_ms": 1000, // legacy field; publish rate now driven by MQTT topic config
        "validate_checksum": true,
        "input_channels": {
          "aivdm": {
            "enabled": true,
            "debug": true,           // log each !AIVDM sentence + decoded fields
            "data_timeout_sec": 5.0,
            "transport": { "id": "ch1" }
          }
        },
        "output_channels": {
          "gga": {
            "enabled": true,         // true → forward received uuv/gnss/gga to transponder
            "debug": true,
            "send_interval_ms": 1000,
            "data_timeout_sec": 2.0, // skip if GPS data older than this
            "transport": { "id": "ch1" }
          }
        }
      },
      {
        "id": 2, "name": "ais2",
        "enabled": true,
        "publish_enabled": true,
        "validate_checksum": true,
        "input_channels": {
          "aivdm": { "enabled": true, "debug": true, "data_timeout_sec": 5.0,
                     "transport": { "id": "ch2" } }
        },
        "output_channels": {
          "gga": { "enabled": true, "debug": true, "send_interval_ms": 1000,
                   "data_timeout_sec": 2.0, "transport": { "id": "ch2" } }
        }
      }
    ]
  }
}
```

### `ais_config.ini` — annotated

```ini
; ── Debug / Logging ────────────────────────────────────────────────────────────
[debug]
enabled = true
level   = debug

[mqtt]
broker     = host.docker.internal
port       = 1883
client_id  = ais_manager

; Flat topic keys — mapped to pub/sub entries at load time
[mqtt.topics]
ais          = uuv/sensors/ais
status       = ais/status
diagnostics  = ais/diagnostics
errors       = ais/errors
gnss_gga     = uuv/gnss/gga

[ais]
status_interval_sec = 5

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
validate_checksum   = true

[ais.device1.input.aivdm]
enabled          = true
debug            = true
data_timeout_sec = 5.0
id               = ch1

[ais.device1.output.gga]
enabled          = true
debug            = true
send_interval_ms = 1000
data_timeout_sec = 2.0
id               = ch1

; AIS Sensor 2
[ais.device2]
id              = 2
name            = ais2
enabled         = true
publish_enabled = true

[ais.device2.input.aivdm]
enabled = true
debug   = true
id      = ch2

[ais.device2.output.gga]
enabled          = true
debug            = true
send_interval_ms = 1000
data_timeout_sec = 2.0
id               = ch2
```

### Transport Types

| Type | Required Keys | Direction |
|------|--------------|-----------|
| `tcp_client` | `host`, `port` | Bidirectional (connects to device) |
| `tcp_server` | `bind_host`, `bind_port` | Bidirectional (device connects to manager) |
| `udp_server` | `bind_host`, `bind_port` | Receive only (`send()` returns false) |
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

### Publish topics

| Name | Topic | Trigger | Payload |
|------|-------|---------|---------|
| `ais` | `uuv/sensors/ais` | Every complete `!AIVDM` received (event-driven) | Vessel JSON for the source device |
| `status` | `ais/status` | Every `publish_interval_ms` ms (timer) | Health + per-device stats |
| `diagnostics` | `ais/diagnostics` | Reserved — not yet published | — |
| `errors` | `ais/errors` | Reserved — not yet published | — |

### Subscribe topics

| Name | Topic | Action |
|------|-------|--------|
| `gnss_gga` | `uuv/gnss/gga` | Payload forwarded via `setGga()` to all devices with `output_channels.gga.enabled = true` |

### `uuv/sensors/ais` — Vessel Data

Published **immediately** when a complete `!AIVDM` message is assembled. Only published when `device.publish_enabled = true` and the `ais` topic's `publish_interval_ms != 0`.

```json
{
  "ts": 1717430400.123,
  "device_id": 1,
  "device_name": "ais1",
  "vessel_count": 4,
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
    }
  ]
}
```

Optional fields present only when available: `lat`/`lon`, `sog`, `cog`, `heading`, `nav_status`, `vessel_name`, `call_sign`, `ship_type`, `imo`, `destination`, `raw` (if `publish_raw_ais = true`).

### `ais/status` — Health Statistics

Published every `publish_interval_ms` ms (from the `status` pub-topic entry). Includes all configured devices (even disabled ones).

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
      "tx_transport_enabled": true,
      "aivdm_ch_enabled": true,
      "gga_ch_enabled": true,
      "connected": true,
      "data_valid": true,
      "last_data_ts": 1717430409.870,
      "data_age_sec": 0.13,
      "packets_received": 1425,
      "crc_errors": 0,
      "vessel_count": 4,
      "gga_sent_count": 72,
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

### `uuv/gnss/gga` — GPS Input (subscribed)

When the manager receives a message on this topic, the raw payload is forwarded as a `$GPGGA` sentence to every device that has `output_channels.gga.enabled = true`. If the GPS data is older than `data_timeout_sec`, it is not forwarded (stale guard).

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
```

The simulator:
- Drains any init commands sent by the manager on connect
- Streams `!AIVDM` types 1 (Class A), 18 (Class B), 5 (static, every 10 ticks), 24 (Class B static, every 15 ticks)
- Displays any `$GPGGA` sentences received from the GGA output channel

### 2. Start ais_manager

```bash
./build/ais_manager config/ais_config.json
```

Expected startup (debug on):
```
[0000000012] [INF] [Main          ] Config: config/ais_config.json
[0000000016] [INF] [AisManager    ] Transport pool — 2 entries
[0000000025] [INF] [ais1          ] Created  id=1  rx_transport=ch1[enabled]  gga_out=enabled
[0000000030] [INF] [AisMqttClient ] Connected to host.docker.internal:1883
[0000000031] [INF] [AisMqttClient ] Subscribed to 'uuv/gnss/gga'  qos=1
[0000001340] [DBG] [ais1          ] RX !AIVDM (47 bytes): !AIVDM,1,1,,A,13nlh6...
[0000001341] [INF] [ais1          ] MMSI=338123456  type= 1  pos=ok  sog=ok
[0000001341] [DBG] [AisManager    ] MQTT TX [uuv/sensors/ais] dev='ais1' vessels=1  312 bytes
[0000005000] [INF] [AisManager    ] Status  dev='ais1'  connected=yes  vessels=4  pkts=300  ...
```

### 3. Monitor MQTT

```bash
# All AIS topics
mosquitto_sub -h localhost -t "uuv/sensors/ais" -t "ais/status" -v

# Pretty-print vessel JSON
mosquitto_sub -h localhost -t "uuv/sensors/ais" | python -m json.tool

# Inject a test GGA sentence to verify forwarding
mosquitto_pub -h localhost -t "uuv/gnss/gga" \
  -m '$GPGGA,123519,1853.200,N,07250.400,E,1,08,0.9,545.4,M,46.9,M,,*47'
```

### 4. Docker end-to-end

```bash
# Terminal 1 — dual simulator
python test_ais.py

# Terminal 2 — manager
docker run --rm --add-host host.docker.internal:host-gateway ais_manager:latest

# Terminal 3 — watch all MQTT
mosquitto_sub -h localhost -t "#" -v
```

### 5. Test disable scenarios

```bash
# Disable GGA forwarding on ais1
# Edit: devices[0].output_channels.gga.enabled = false

# Disable vessel publish entirely for ais2
# Edit: devices[1].publish_enabled = false

# Disable the ais topic (publish_interval_ms = 0)
# Edit: mqtt.topics.pub[name=ais].publish_interval_ms = 0
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
    "input_channels": { "aivdm": { "enabled": true, "transport": { "id": "com1" } } },
    "output_channels": { "gga": { "enabled": true, "send_interval_ms": 1000,
                                   "transport": { "id": "com1" } } }
  }
]
```

### Dual-device serial config

```json
"transport": [
  { "id": "com1", "type": "serial", "serial_port": "/dev/ttyUSB0", "serial_baud": 38400 },
  { "id": "com2", "type": "serial", "serial_port": "/dev/ttyUSB1", "serial_baud": 38400 }
],
"devices": [
  { "id": 1, "name": "ais1", "input_channels": { "aivdm": { "transport": { "id": "com1" } } }, ... },
  { "id": 2, "name": "ais2", "input_channels": { "aivdm": { "transport": { "id": "com2" } } }, ... }
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
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5...0,0*2A  ← Part A: vessel name
!AIVDM,1,1,,A,H3m:H3<DhFt5hFt5...1,0*2B  ← Part B: callsign, dimensions
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
