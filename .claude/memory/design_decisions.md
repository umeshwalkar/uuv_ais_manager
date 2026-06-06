---
name: design-decisions
description: Key design choices, architectural trade-offs, and things to avoid
metadata:
  type: project
---

## Why transport pool is owned by AisManager, not AisDevice

**Why:** Multiple channels within the same device (aivdm RX + gga TX) share one physical connection. If AisDevice owned the transport, sharing would require reference counting or external coordination.

**How to apply:** AisManager owns `pool_`, passes raw `ResolvedTransport` to each device. AisDevice calls `rx_.ptr->open()` / `close()` / `readLine()` / `send()` directly. AisManager must outlive all AisDevices.

## Why `ChannelTransportRef.id` (not `shared_with`)

**Why:** The user renamed the key in `ais_config.json` from `"shared_with"` to `"id"` (2026-06-06) to make the config self-describing. The JSON key `"id"` inside a channel's `transport` object unambiguously references a pool entry's label.

**Disambiguation:** `device.id` is an `int` (1, 2, …); `channel.transport.id` is a `string` ("ch1", "ch2", …). Never confuse them.

## Why device.enabled=false devices still appear in ais/status

**Why:** An operator disabling a device should still see it in the status dashboard to confirm the disable took effect. Health shows `device_disabled`.

**How to apply:** `AisDevice` is NOT created for disabled devices. `formatStatusJson()` in `AisManager` loops over `cfg_.ais.devices` (all configured), not just `devices_` (running ones), and adds a minimal status entry for disabled devices.

## Why publish_enabled=false keeps transport running

**Why:** Needed for scenarios where the device should still receive/send (e.g. GGA output active) but MQTT publish is temporarily suppressed (e.g. during calibration, or to reduce network load).

**How to apply:** `publishLoop` skips `snap.publish_enabled=false` devices for `uuv/ais` publish but includes them in `ais/status` always.

## Why data-timeout warning is rate-limited

**Why:** rxLoop runs at ~1 kHz (1 s read timeout). Without rate-limiting, a single stale period would generate thousands of log lines.

**How to apply:** In rxLoop, track `last_data_warn_time_`. Fire `LOG_WRN` only when `now - last_data_warn_time_ > data_timeout_sec`. Reset `last_data_warn_time_` after firing.

## Why Logger MOD for AisDevice is the device name

**Why:** With two devices, distinguishing `[AisDevice     ]` from `[AisDevice     ]` is useless. `[ais1          ]` vs `[ais2          ]` is immediately scannable.

**How to apply:** `AisDevice` stores `std::string mod_` = `cfg_.name`. Pass `mod_.c_str()` to all LOG macros. Module column is 14 chars — device names up to 14 chars are fine.

## Why AisParser.hpp is header-only

**Why:** Follows `SvpParser.hpp` (svp_manager) pattern. Keeps the decoder self-contained and easily testable without a build step.

**How to apply:** All AIS bit-decoding logic stays in `AisParser.hpp`. Do not split into .cpp. The `AisParser` class holds multi-part reassembly state, so one instance per AisDevice.

## Things to avoid

- **Never `std::cout` / `std::cerr`** — all output goes through `LOG_*` macros
- **Never add global config keys for per-device settings** — `publish_interval_ms`, `validate_checksum`, `debug` etc. live inside each device config, not at `ais` level
- **Never share ITransport across devices** — the pool allows sharing within one device (rx+tx); cross-device sharing would cause race conditions in `readLine()`
- **Never call `Logger::init()` more than once** — called exactly once in `main()` before anything else
- **Never skip INI loader when adding a JSON config key** — both loaders must stay in sync
