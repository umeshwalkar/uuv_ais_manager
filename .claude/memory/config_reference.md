---
name: config-reference
description: Complete config key reference — all fields, types, defaults, INI section equivalents
metadata:
  type: reference
---

## JSON top-level structure

```
AppConfig
  ├─ debug:   DebugConfig
  ├─ mqtt:    MqttConfig
  └─ ais:     AisConfig
```

## DebugConfig

| JSON key | INI section/key | Type | Default | Meaning |
|----------|----------------|------|---------|---------|
| `debug.enabled` | `[debug] enabled` | bool | false | Master INF/DBG switch. ERR+WRN always printed. |
| `debug.level` | `[debug] level` | string | "warn" | Minimum level: `debug \| info \| warn \| error` |

## MqttConfig

| JSON key | INI section/key | Type | Default |
|----------|----------------|------|---------|
| `mqtt.enabled` | `[mqtt] enabled` | bool | true |
| `mqtt.broker` | `[mqtt] broker` | string | "localhost" |
| `mqtt.port` | `[mqtt] port` | int | 1883 |
| `mqtt.client_id` | `[mqtt] client_id` | string | "ais_manager" |
| `mqtt.keepalive` | `[mqtt] keepalive` | int | 60 |
| `mqtt.qos` | `[mqtt] qos` | int | 1 |
| `mqtt.retain` | `[mqtt] retain` | bool | false |
| `mqtt.topics.ais` | `[mqtt.topics] ais` | string | "uuv/ais" |
| `mqtt.topics.status` | `[mqtt.topics] status` | string | "ais/status" |
| `mqtt.topics.gnss_gga` | `[mqtt.topics] gnss_gga` | string | "uuv/gnss/gga" |

## AisConfig

| JSON key | INI section/key | Type | Default |
|----------|----------------|------|---------|
| `ais.status_interval_sec` | `[ais] status_interval_sec` | int | 10 |

## TransportDef (pool entry)

JSON array: `ais.transport[]` — INI sections: `[ais.transport.<id>]` (section suffix = id)

| JSON key | INI key | Type | Default |
|----------|---------|------|---------|
| `id` | (section suffix) | string | — |
| `enabled` | `enabled` | bool | true |
| `type` | `type` | string | "tcp_client" |
| `host` | `host` | string | "" |
| `port` | `port` | int | 0 |
| `bind_host` | `bind_host` | string | "0.0.0.0" |
| `bind_port` | `bind_port` | int | 0 |
| `serial_port` | `serial_port` | string | "" |
| `serial_baud` | `serial_baud` | int | 9600 |
| `connect_timeout_sec` | `connect_timeout_sec` | int | 5 |
| `reconnect_delay_sec` | `reconnect_delay_sec` | int | 3 |
| `read_timeout_ms` | `read_timeout_ms` | int | 1000 |
| `buffer_size_bytes` | `buffer_size_bytes` | int | 1024 |

## AisDeviceConfig

JSON array: `ais.devices[]` — INI sections: `[ais.device1]`, `[ais.device2]`, …

| JSON key | INI key | Type | Default | Note |
|----------|---------|------|---------|------|
| `id` | `id` | **int** | seq | Numeric device identifier |
| `name` | `name` | string | "ais1" | Used as Logger MOD tag |
| `enabled` | `enabled` | bool | true | false → skip all ops |
| `sync_timeout_sec` | `sync_timeout_sec` | double | 5.0 | |
| `send_init_on_reconnect` | `send_init_on_reconnect` | bool | false | |
| `init_commands` | `[ais.device1.init_commands] cmd1 = ...` | string[] | [] | |
| `publish_enabled` | `publish_enabled` | bool | true | false → no uuv/ais publish |
| `publish_raw_ais` | `publish_raw_ais` | bool | false | include raw NMEA in vessel JSON |
| `publish_interval_ms` | `publish_interval_ms` | int | 1000 | per-device rate |
| `validate_checksum` | `validate_checksum` | bool | true | |

## AivdmChannelConfig (input_channels.aivdm)

INI section: `[ais.device1.input.aivdm]`

| JSON key | INI key | Type | Default | Note |
|----------|---------|------|---------|------|
| `enabled` | `enabled` | bool | true | |
| `debug` | `debug` | bool | false | Log RX sentences + decoded fields |
| `data_timeout_sec` | `data_timeout_sec` | double | 5.0 | Stale threshold |
| `transport.id` | `id` | **string** | "" | Pool entry label reference |

## GgaChannelConfig (output_channels.gga)

INI section: `[ais.device1.output.gga]`

| JSON key | INI key | Type | Default | Note |
|----------|---------|------|---------|------|
| `enabled` | `enabled` | bool | false | |
| `debug` | `debug` | bool | false | Log TX GGA sentences |
| `send_interval_ms` | `send_interval_ms` | int | 1000 | |
| `data_timeout_sec` | `data_timeout_sec` | double | 2.0 | GPS stale threshold |
| `transport.id` | `id` | **string** | "" | Pool entry label reference |

## id disambiguation

- `ais.transport[].id` → **string**, pool entry label (e.g. `"ch1"`)
- `ais.devices[].id` → **int**, device number (e.g. `1`)
- `input_channels.aivdm.transport.id` → **string**, references a pool entry label
- `output_channels.gga.transport.id` → **string**, references a pool entry label
