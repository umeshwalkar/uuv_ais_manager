# AIS Manager — Development Log

**Project:** `D:\Study\Docker\ais_manager`
**Session dates:** 2026-06-04 / 2026-06-05 / 2026-06-06
**References used:** `D:\Study\Docker\gnss_manager`, `D:\Study\Docker\svp_manager`, `D:\Study\Docker\ins_manager`

---

## Session Summary

This session built the complete `ais_manager` C++17 service from scratch, following the architecture patterns established in `svp_manager` and `gnss_manager`, with the logger pattern from `ins_manager`.

---

## Iteration History

### 1 — Initial build (svp_manager pattern, single device)

Built the initial single-device AIS manager following `svp_manager` architecture:

**Files created:**
- `src/Config.hpp / .cpp` — `TransportConfig`, `AisDeviceConfig`, `GpsOutputConfig`, `AisConfig`, JSON+INI loaders
- `src/AisParser.hpp` — header-only 6-bit AIS decoder; types 1/2/3, 5, 18, 21, 24; multi-part fragment reassembly
- `src/AisManager.hpp / .cpp` — `receiveLoop` + `publishLoop` + `gpsOutputLoop`, per-MMSI vessel table
- `src/main.cpp` — signal-safe entry point
- `CMakeLists.txt` — links `libmosquitto`, `nlohmann_json`
- `Dockerfile` — multi-stage Ubuntu 24.04
- `config/ais_config.json` — TCP client, port 4008
- `config/ais_config.ini` — INI alternative
- `test_ais.py` — TCP server simulator, 4 vessels, types 1/18/5/24
- `README.md`

**Key design decisions:**
- `shared_with` key in output_channels references the rx transport by name
- Vessel table is per-MMSI map merging position + static data
- GPS output channel sends `$GPGGA` back to device using same TCP connection
- `uuv/ais` published with vessel array; `ais/status` published with health stats

---

### 2 — Two AIS sensors (gnss_manager multi-receiver pattern)

Extended to support AIS1 and AIS2 on separate transport media, following `gnss_manager`'s multi-receiver pattern.

**New files:**
- `src/AisDevice.hpp / .cpp` — per-device class (like `GnssReceiver`): owns transport, rxLoop, vessel table, `snapshot()`

**Key changes:**
- `AisConfig.devices` → `vector<AisDeviceConfig>` (replaces single `ais_device`)
- Each `AisDeviceConfig` has `id`, `name`, per-device `init_commands`
- `AisManager` holds `vector<unique_ptr<AisDevice>>`
- `publishLoop` iterates all devices, merges vessel tables
- Published vessel JSON includes `device_id` and `device_name` per vessel entry
- `ais/status` has `"devices"` array — per-device stats
- `test_ais.py` runs two TCP server threads (`[AIS1]` / `[AIS2]`)

---

### 3 — Per-device GGA output channels

Each device gets its own `output_channels.gga` section (moved from single top-level config).

**Key changes:**
- `GgaOutputConfig` → `GgaChannelConfig` — moved into `AisDeviceConfig`
- `AisDevice` owns its own `ggaOutputLoop` thread (started if `gga_out.enabled && transport.enabled`)
- `AisDevice::setGga()` — receives latest GGA string from `AisManager`
- `AisManager::setGga()` — broadcasts to all devices with `gga_out.enabled`
- Removed single `gps_out_thread_` from `AisManager`
- `ais/status` per-device entry gains `gga_output_enabled` and `gga_sent_count`

---

### 4 — Named transport pool + full channel enable/disable logic

Adopted the architecture from `suggested_ais_config.json`:

**Config structure (final):**
```
ais.transport[]           named pool: id, enabled, type, host/port/serial
ais.devices[]
  ├─ enabled              device master switch
  ├─ publish_enabled      MQTT vessel publish switch (transport still runs)
  ├─ publish_interval_ms  per-device rate
  ├─ validate_checksum    per-device
  ├─ input_channels.aivdm  { enabled, debug, data_timeout_sec, transport.shared_with }
  └─ output_channels.gga   { enabled, debug, send_interval_ms, data_timeout_sec, transport.shared_with }
```

**Enabling/disabling logic (enforced in code):**

| Condition | Effect |
|-----------|--------|
| `transport.enabled=false` | Never opened; device in `ais/status` with `health:"transport_disabled"` |
| `aivdm.enabled=false` | Transport connects, lines discarded; `health:"channel_disabled"` |
| `gga.enabled=false` | GGA thread not started |
| `device.publish_enabled=false` | Transport/parse runs; NO `uuv/ais` publish; IS in `ais/status` |
| `device.enabled=false` | All operations skipped; in `ais/status` as `health:"device_disabled"` |

**New files:**
- `src/AisDevice.hpp` updated with `ResolvedTransport` struct
- `AisManager` owns `map<string, unique_ptr<ITransport>>` transport pool
- `AisManager::buildTransportPool()` + `resolveTransport(id)` wires devices to pool
- `publishLoop` respects `publish_enabled` and individual `publish_interval_ms`
- `ais/status` carries: `rx_transport_enabled`, `tx_transport_enabled`, `aivdm_ch_enabled`, `gga_ch_enabled`

---

### 5 — Logger integration (ins_manager pattern)

Added tick-stamped, level-gated logging throughout all modules.

**Logger copied from:** `D:\Study\Docker\ins_manager\src\Logger.hpp / .cpp`

**Output format:**
```
[0000012345ms] [ERR] [module        ] message
[0000012345ms] [WRN] [module        ] message
[0000012345ms] [INF] [module        ] message   (debug.enabled=true only)
[0000012345ms] [DBG] [module        ] message   (debug.enabled=true only)
```

**Module tags:**

| File | MOD tag |
|------|---------|
| `main.cpp` | `"Main"` |
| `AisManager.cpp` | `"AisManager"` |
| `AisDevice.cpp` | device name e.g. `"ais1"` (stored in `mod_`) |
| `MqttClient.cpp` | `"MqttClient"` |

**Log coverage:**

| Level | Triggers |
|-------|---------|
| ERR | Transport connect failed, CRC error, MQTT publish failed, GGA send failed, init cmd failed |
| WRN | Transport disabled (idling), connection lost, no data for N seconds (rate-limited), GGA data stale, MQTT unexpected disconnect, data stale in publish loop, channel enabled but transport disabled |
| INF | Connected/disconnected, init commands, loop start/stop, periodic status summary, MQTT connected, startup summary |
| DBG | Raw RX NMEA sentence (when `aivdm.debug=true`), decoded vessel fields, GGA TX payload (when `gga.debug=true`), MQTT TX per-publish bytes, non-AIS lines skipped |

**New config keys:**
- `debug.enabled` — master INFO/DEBUG switch
- `debug.level` — `debug | info | warn | error`
- `input_channels.aivdm.debug` — per-channel sentence payload logging
- `output_channels.gga.debug` — per-channel GGA TX logging
- `data_timeout_sec` warning: rate-limited, fires once per timeout period

**INI equivalents:**
- `[debug]` section
- `debug = true/false` in `[ais.device1.input.aivdm]` and `[ais.device1.output.gga]`

---

### 6 — Channel transport key renamed: `shared_with` → `id` (2026-06-06)

User updated `ais_config.json`: inside `input_channels.aivdm.transport` and `output_channels.gga.transport`, the key that references the transport pool entry was renamed from `"shared_with"` to `"id"`.

**Files changed:**

| File | Change |
|------|--------|
| `src/Config.hpp` | `ChannelTransportRef.shared_with` → `.id`; updated comments |
| `src/Config.cpp` | `parseRef()` reads `"id"` JSON key; INI loader reads `"id"` key |
| `src/AisDevice.cpp` | All 8 occurrences of `.transport.shared_with` → `.transport.id` |
| `src/AisManager.cpp` | All 4 occurrences of `.transport.shared_with` → `.transport.id` |
| `config/ais_config.ini` | `shared_with = ch1/ch2` → `id = ch1/ch2` in all channel sections |
| `README.md` | Section heading, prose, code blocks, INI example comments all updated |

**Key disambiguation added to README:**

| Location | Type | Example | Meaning |
|----------|------|---------|---------|
| `ais.transport[].id` | string | `"ch1"` | Pool entry label |
| `ais.devices[].id` | integer | `1` | Numeric device identifier |
| `*.transport.id` | string | `"ch1"` | Reference to a pool entry |

`suggested_ais_config.json` and `DEVELOPMENT_LOG.md` were left unchanged (reference/history files).

---

## Final File Layout

```
ais_manager/
├── src/
│   ├── Config.hpp / .cpp      — config structs + JSON/INI loaders
│   ├── Logger.hpp / .cpp      — tick-stamped logger (from ins_manager)
│   ├── AisParser.hpp          — header-only 6-bit AIS decoder
│   ├── AisDevice.hpp / .cpp   — per-device rx/gga threads + vessel table
│   ├── AisManager.hpp / .cpp  — transport pool, publishLoop
│   ├── Transport.hpp / .cpp   — ITransport + TCP/UDP/serial implementations
│   ├── MqttClient.hpp / .cpp  — libmosquitto wrapper
│   └── main.cpp               — entry point
├── config/
│   ├── ais_config.json        — primary config (two TCP devices)
│   └── ais_config.ini         — INI alternative
├── references/
│   ├── Comar-R220U-Datasheet.pdf
│   └── packet structure.txt
├── CMakeLists.txt
├── Dockerfile
├── test_ais.py                — dual-server AIS simulator (8 vessels)
├── README.md
└── DEVELOPMENT_LOG.md         — this file
```

---

## Key Design Principles Applied

1. **Pattern consistency** — followed `svp_manager` → `gnss_manager` → `ins_manager` patterns exactly; new code is recognisably from the same family.
2. **Transport pool** — single definition, referenced by `id` key in channels; same physical TCP connection used bidirectionally.
3. **Granular enable/disable** — transport, channel, device, and publish can each be independently disabled without stopping the others.
4. **Per-device isolation** — each `AisDevice` owns its transport lifecycle, threads, vessel table, and logger tag; `AisManager` only coordinates.
5. **Logger** — ERR/WRN always visible; INF/DBG gated by `debug.enabled`; per-channel `debug` flag for payload tracing without flooding unrelated output.
6. **Config dual format** — JSON (primary, human-readable with comments) and INI (flat key-value for simpler deployments) both fully supported.

---

## AIS Device Reference

**Comar R220U:**
- Output: continuous `!AIVDM` at 38400 baud RS-232
- No init commands required
- Default port: TCP 4008 when used over LAN gateway

**Supported message types decoded:**
| Type | Description |
|------|-------------|
| 1/2/3 | Position Report Class A — MMSI, lat, lon, SOG, COG, heading, nav status |
| 5 | Static & Voyage Data — vessel name, IMO, callsign, ship type, ETA, destination |
| 18 | Standard Class B Position — MMSI, lat, lon, SOG, COG, heading |
| 21 | Aid-to-Navigation — name, lat, lon |
| 24 | Class B Static — vessel name (Part A), callsign, ship type, dimensions (Part B) |
