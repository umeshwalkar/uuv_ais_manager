---
name: project-architecture
description: AIS manager transport pool, device wiring, thread model, class relationships
metadata:
  type: project
---

## Class ownership hierarchy

```
main()
  └─ AisManager (owns everything)
        ├─ pool_: map<string, unique_ptr<ITransport>>   ← created from ais.transport[]
        ├─ devices_: vector<unique_ptr<AisDevice>>      ← one per enabled device
        │     ├─ rx_: ResolvedTransport {ptr, enabled}  ← raw ptr into pool_
        │     ├─ tx_: ResolvedTransport {ptr, enabled}  ← raw ptr into pool_ (may == rx_.ptr)
        │     ├─ rx_thread_   → rxLoop()
        │     └─ gga_thread_  → ggaOutputLoop()  (only if gga.enabled && tx.enabled)
        ├─ mqtt_: unique_ptr<MqttClient>
        └─ publish_thread_  → publishLoop()
```

## Transport pool build sequence (AisManager constructor)

1. `buildTransportPool()` — iterates `cfg_.ais.transports`, calls `makeTransport()` for each, stores in `pool_[id]`
2. For each enabled device, calls `resolveTransport(id)` to get `{ptr, enabled}` pair
3. If `aivdm_in.enabled=false` → force `rx.enabled=false` even if pool entry is enabled
4. If `gga_out.enabled=false` → force `tx.enabled=false`
5. Constructs `AisDevice(cfg, rx, tx)`

## AisDevice rxLoop state machine

```
transport disabled (rx_.enabled=false)
  → idle loop, status shows connected=false

transport enabled, not open
  → LOG_INF "Connecting..."
  → rx_.ptr->open()
    fail → LOG_ERR, sleep reconnect_delay_sec, retry
    ok   → LOG_INF "Connected", reset last_data_time_, send init commands

transport open, read line
  → empty line: check data-timeout warning (rate-limited)
  → non-! line: LOG_DBG skip
  → CRC fail: LOG_ERR, crc_errors++
  → aivdm.enabled=false: count packet, discard
  → parse: processLine() → updateVessel() → state_.vessels[mmsi]
```

## publishLoop tick model

- Ticks every 100 ms
- Per-device: checks `now >= next_pub[device.id]`, advances by `publish_interval_ms`
- Only publishes if `snap.publish_enabled == true`
- Status: checks `now >= last_status + status_interval_sec`
- Status always published for all devices (including `device.enabled=false` entries)

## setGga() broadcast

`AisManager::setGga(gga, ts)` → for each device where `dcfg.gga_out.enabled == true` → `dev->setGga(gga, ts)` → stored in `last_gga_` (mutex-protected) → read by `ggaOutputLoop` → sent via `tx_.ptr->send(gga + "\r\n")`

## ResolvedTransport

```cpp
struct ResolvedTransport {
    ITransport* ptr     = nullptr;   // raw ptr into AisManager::pool_
    bool        enabled = false;     // TransportDef.enabled && channel.enabled
};
```

AisDevice does NOT own the transport. Lifetime: pool_ outlives all AisDevice instances.

## ChannelTransportRef

```cpp
struct ChannelTransportRef {
    std::string id;   // must match a TransportDef.id in the pool
};
```

The `id` key in JSON/INI channel transport objects references a pool entry. **Note: `device.id` (int) and `channel.transport.id` (string) are different fields.**
