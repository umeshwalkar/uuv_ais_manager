---
name: feedback-conventions
description: Coding rules, Logger usage, patterns confirmed by user, things to avoid
metadata:
  type: feedback
---

## Confirmed working patterns (do not change)

**Logger from ins_manager:**
- Copy `Logger.hpp / .cpp` verbatim from `D:\Study\Docker\ins_manager\src\` — do not rewrite
- `Logger::init()` called FIRST in `main()` before any other code
- `Logger::setEnabled(cfg.debug.enabled)` + `Logger::setLevel(parseLevel(cfg.debug.level))` applied immediately after config load
- Module name ≤ 14 chars; AisDevice uses `mod_ = cfg_.name` (device name); AisManager uses `"AisManager"`

**Why:** User explicitly requested this pattern from ins_manager. Confirmed accepted across multiple iterations.

---

**Config dual format (JSON + INI must stay in sync):**
- Every field added to `fromJsonFile()` must also be added to `fromIniFile()` with the corresponding section/key
- JSON uses `json::parse(f, nullptr, true, true)` (allow comments)
- INI uses flat section-based parser with `data[section][key]` map

**Why:** User asked for both formats from session start. Confirmed correct at every review.

---

**Per-device config (not global):**
- `publish_interval_ms`, `validate_checksum`, `publish_raw_ais`, `debug` (channels) → inside each device entry
- `status_interval_sec` → at `ais` level (single value affects all devices' combined status publish)
- `init_commands` → inside each device entry (per-device commands)

**Why:** User explicitly requested this in iteration 2 (multi-device). Reconfirmed at every iteration.

---

**ais/status always includes disabled devices:**
- `formatStatusJson()` loops `cfg_.ais.devices` (all), not `devices_` (only running ones)
- Disabled device entry: `{ "id": n, "name": "...", "health": "device_disabled" }`

**Why:** User rule: "if devices is disabled then... MQTT ais/status will be still reported with status of that device."

---

**Rate-limit WRN for repetitive conditions:**
- Data-timeout warning: `last_data_warn_time_` steady_clock, fires once per `data_timeout_sec` period
- Stale data warning in publishLoop: fires once per `data_timeout_sec` period per device

**Why:** Without rate-limiting, a stale condition would flood the console at 1 Hz (readLine timeout rate).

---

## Things to avoid (learned from corrections)

**Never use std::cout / std::cerr:**
- Replace all `std::cout <<` and `std::cerr <<` with `LOG_INF` / `LOG_WRN` / `LOG_ERR` / `LOG_DBG`
- This was done globally in iteration 5 (Logger integration)

**Never use `shared_with` key:**
- The channel transport reference key was renamed to `id` by user (2026-06-06)
- `ChannelTransportRef.id` is the C++ field; `"id"` is the JSON/INI key
- `suggested_ais_config.json` still has `shared_with` — that's a reference artifact, do not use it as a template

**Never mix transport ownership:**
- AisDevice holds `ResolvedTransport {ITransport* ptr, bool enabled}` — raw pointers only
- AisManager's `pool_` holds `unique_ptr<ITransport>` — the only owner
- Deleting or moving pool entries while devices are running causes dangling pointers

**Never start gga_thread if channel or transport is disabled:**
```cpp
// Correct:
if (cfg_.gga_out.enabled && tx_.enabled && tx_.ptr)
    gga_thread_ = std::thread(&AisDevice::ggaOutputLoop, this);
// Wrong: starting thread then checking inside — wastes a thread object
```

**Never add per-device fields at ais[] level:**
- `ais.publish_interval_ms` (global) was removed in favour of per-device `devices[].publish_interval_ms`
- Same applies to any future per-device settings

---

## Code style rules confirmed

- `#define MOD "ModuleName"` at file top for LOG macros
- `printf`-style format strings in LOG calls (not streams)
- `const char* mod = mod_.c_str()` at start of long functions to avoid repeated `.c_str()` calls
- `std::lock_guard<std::mutex>` for snapshot reads — no long-lived locks
- `cv_.wait_for(lk, interval, predicate)` for all sleeps (interruptible by `stop()`)
- Reconnect loop: `LOG_ERR` on failure, `cv_.wait_for(reconnect_delay_sec)`, continue

---

## Reference projects to check before writing new code

1. `D:\Study\Docker\svp_manager` — transport lifecycle, init commands, single-device pattern
2. `D:\Study\Docker\gnss_manager` — multi-receiver, snapshot pattern, quality selection
3. `D:\Study\Docker\ins_manager` — Logger, DebugConfig, transport pool, per-channel debug flag
