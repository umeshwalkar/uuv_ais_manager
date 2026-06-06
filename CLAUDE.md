# AIS Manager — Project Instructions for Claude

## What this project is

C++17 service that connects to one or more Comar R220U (or compatible) marine AIS transponders over TCP, UDP, or RS-232 serial; decodes NMEA 0183 `!AIVDM`/`!AIVDO` sentences using a 6-bit AIS payload decoder; and publishes per-device vessel JSON to an MQTT broker.

---

## Reference projects (same codebase family)

When adding features or fixing bugs, always check these sibling projects first for patterns to follow:

| Project | Pattern to reuse |
|---------|-----------------|
| `D:\Study\Docker\svp_manager` | Single-device manager, transport lifecycle, init commands |
| `D:\Study\Docker\gnss_manager` | Multi-receiver pattern, per-device threads, quality selection |
| `D:\Study\Docker\ins_manager` | Logger module, DebugConfig, per-channel debug flag, transport pool |

The AIS manager is a deliberate synthesis of all three. New code must be recognisably from the same family.

---

## Architecture (quick reference)

```
Transport Pool  [ch1: tcp:4008]  [ch2: tcp:4009]  [serial1: ...]
                      │                  │
               AisDevice 1          AisDevice 2     (one per enabled device)
               rxLoop                rxLoop          ← reads !AIVDM/!AIVDO
               ggaOutputLoop         ggaOutputLoop   ← writes $GPGGA (optional)
               vessel table          vessel table
                      │                  │
               AisManager::publishLoop             ← publishes uuv/ais + ais/status
               AisManager::setGga()               ← broadcasts GPS to all devices
```

**Thread ownership:** Each `AisDevice` owns exactly two threads (`rxLoop`, `ggaOutputLoop`). `AisManager` owns one (`publishLoop`). `AisManager` owns all `ITransport` objects; `AisDevice` holds raw pointers.

---

## Config structure (must stay in sync)

```json
{
  "debug":   { "enabled": true, "level": "debug" },
  "mqtt":    { "broker": "...", "port": 1883, "topics": { "ais": "uuv/ais", "status": "ais/status", "gnss_gga": "uuv/gnss/gga" } },
  "ais": {
    "status_interval_sec": 10,
    "transport": [
      { "id": "ch1", "enabled": true, "type": "tcp_client", "host": "...", "port": 4008, ... },
      { "id": "ch2", "enabled": true, "type": "tcp_client", "host": "...", "port": 4009, ... }
    ],
    "devices": [
      {
        "id": 1, "name": "ais1", "enabled": true,
        "publish_enabled": true, "publish_interval_ms": 1000, "validate_checksum": true,
        "input_channels":  { "aivdm": { "enabled": true, "debug": true, "data_timeout_sec": 5.0,
                                         "transport": { "id": "ch1" } } },
        "output_channels": { "gga":   { "enabled": false, "debug": true, "send_interval_ms": 1000,
                                         "data_timeout_sec": 2.0, "transport": { "id": "ch1" } } }
      }
    ]
  }
}
```

**Key: `transport.id` inside a channel references the pool entry label (string). `device.id` is a separate numeric field.**

INI equivalent sections: `[ais.transport.ch1]`, `[ais.device1]`, `[ais.device1.input.aivdm]`, `[ais.device1.output.gga]`.

---

## Enable/disable rules (enforce in code, not just config)

| Flag | Effect on AisDevice |
|------|---------------------|
| `device.enabled = false` | AisDevice not created; shown as `device_disabled` in status |
| `transport.enabled = false` | `ResolvedTransport.enabled=false`; rxLoop idles; `transport_disabled` in status |
| `aivdm.enabled = false` | Transport connects (may be needed for GGA TX), lines discarded; `channel_disabled` |
| `gga.enabled = false` | `ggaOutputLoop` thread not started at all |
| `publish_enabled = false` | Receive/send runs normally; NO `uuv/ais` publish; IS in `ais/status` |

---

## Logger usage (mandatory for all new code)

```cpp
#include "Logger.hpp"
#define MOD "MyModule"          // or use device name: mod_.c_str()

LOG_ERR(MOD, "Connect FAILED: %s", err.c_str());   // always printed
LOG_WRN(MOD, "Data stale %.1fs (> %.1fs)", age, timeout);  // always printed
LOG_INF(MOD, "Connected to '%s'", transport_id.c_str());    // debug.enabled only
LOG_DBG(MOD, "RX raw (%zu bytes): %s", line.size(), line.c_str()); // channel.debug only
```

- `ERR` + `WRN` — always shown regardless of config
- `INF` — shown when `debug.enabled = true`
- `DBG` — shown when `debug.enabled = true`; for payload content, also check `cfg_.aivdm_in.debug` or `cfg_.gga_out.debug` before calling
- Rate-limit `WRN` for repetitive conditions (e.g. data-timeout warning: once per `data_timeout_sec`)
- `AisDevice` uses device name as MOD: stored in `mod_` (`= cfg_.name`), pass as `mod_.c_str()`

**Never use `std::cout` or `std::cerr` directly in any source file.**

---

## Key source files

| File | Role |
|------|------|
| `src/Config.hpp/.cpp` | All config structs: `DebugConfig`, `TransportDef`, `ChannelTransportRef`, `AivdmChannelConfig`, `GgaChannelConfig`, `AisDeviceConfig`, `AisConfig`, `AppConfig`. JSON and INI loaders. |
| `src/Logger.hpp/.cpp` | Tick-stamped logger. Copied from `ins_manager`. Do not modify. |
| `src/AisParser.hpp` | Header-only 6-bit AIS decoder. Handles types 1/2/3/5/18/21/24, multi-part reassembly. |
| `src/AisDevice.hpp/.cpp` | Per-device: `rxLoop` + `ggaOutputLoop` threads, vessel table, `setGga()`, data-timeout warning. |
| `src/AisManager.hpp/.cpp` | Transport pool (`pool_`), device wiring, `publishLoop`, `setGga()` broadcast. |
| `src/Transport.hpp/.cpp` | `ITransport` + TCP client/server, UDP server, serial. DO NOT modify — shared pattern. |
| `src/MqttClient.hpp/.cpp` | libmosquitto wrapper. Uses Logger. |
| `src/main.cpp` | Entry point: `Logger::init()` first, then load config, apply debug settings, run. |

---

## Build

```bash
# Prerequisites (Ubuntu 22.04/24.04)
sudo apt-get install -y build-essential cmake libmosquitto-dev nlohmann-json3-dev

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
# binary: build/ais_manager
```

CMakeLists.txt SOURCES list: `main.cpp Config.cpp Logger.cpp Transport.cpp MqttClient.cpp AisDevice.cpp AisManager.cpp`

---

## Testing

```bash
# Terminal 1: dual AIS simulator (two TCP servers, 8 vessels)
python test_ais.py                        # ports 4008 + 4009, mix format
python test_ais.py --format 1 --interval 0.5   # fast Class-A only

# Terminal 2: manager
./build/ais_manager config/ais_config.json

# Terminal 3: monitor MQTT
mosquitto_sub -h localhost -t "uuv/ais" -t "ais/status" -v
mosquitto_sub -h localhost -t "uuv/ais" | python -m json.tool
```

---

## MQTT topics

| Topic | Direction | Content |
|-------|-----------|---------|
| `uuv/ais` | Publish | Per-device vessel array with `device_id`/`device_name` per vessel |
| `ais/status` | Publish | All devices array: `health`, `connected`, `packets_received`, `crc_errors`, `gga_sent_count`, transport/channel enabled flags |
| `uuv/gnss/gga` | Subscribe | GPS GGA fed to `setGga()` for GGA output channels |

---

## Conventions

- **No `std::cout`/`std::cerr`** — use `LOG_*` macros everywhere
- **No global `init_commands`** — they live per device in `AisDeviceConfig`
- **No ownership of ITransport in AisDevice** — pool owned by AisManager; AisDevice holds raw `ResolvedTransport {ptr, enabled}`
- **Channel transport key is `"id"`** (not `"shared_with"`) — matches the pool entry's string label
- **Both JSON and INI must stay in sync** — any new config field added to JSON loader must also be added to INI loader and vice versa
- **`ais/status` must always include all configured devices** — even `device.enabled=false` ones (shown as `device_disabled`)
- **Rate-limit warnings** — use a `last_warn_time` steady_clock checkpoint; don't warn every loop tick

---

## Session history

See `DEVELOPMENT_LOG.md` for full iteration history and `CLAUDE.md` (this file) for live project instructions.
Local workspace memory: `.claude/memory/`
Global project memory: `C:\Users\Umesh\.claude\projects\d--Study-Docker-ais-manager\memory\`
