# How to Test ais_manager

Integration tests drive the real `ais_manager` binary end-to-end:

```
AIS device (TCP server) → ais_manager binary → MQTT broker → test assertions
```

Each test starts one or two TCP servers that simulate AIS transponders, launches
the manager with a generated config, subscribes to MQTT, injects NMEA sentences
or GGA messages, and asserts the resulting MQTT output.

---

## Prerequisites

### 1. Build the binary

```bash
cd /workspaces/ais_manager
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Expected output: `build/ais_manager`

### 2. Install Python dependencies

```bash
pip3 install paho-mqtt pytest
```

Or use the requirements file:

```bash
pip3 install -r tests/requirements_test.txt
```

> **Devcontainer users:** rebuild the container after the recent Dockerfile update
> (`python3`, `pip3`, `paho-mqtt`, and `pytest` are now included).

### 3. Start an MQTT broker

```bash
mosquitto -v &
```

The tests connect to `localhost:1883`. All tests are automatically skipped if no
broker is reachable (via the `require_mqtt` session fixture in `conftest.py`).

---

## Running the Tests

```bash
cd /workspaces/ais_manager/tests

# Run the full suite
pytest -v

# Run a single file
pytest test_receive.py -v

# Run a specific test class
pytest test_receive.py::TestBasicPublish -v

# Run a single test
pytest test_receive.py::TestBasicPublish::test_type1_triggers_immediate_publish -v

# Show live log output
pytest -v --log-cli-level=INFO

# Stop on first failure
pytest -v -x
```

### Port assignments

| Channel | Port  | Purpose |
|---------|-------|---------|
| ch1 | **14008** | Simulates ais1 transponder |
| ch2 | **14009** | Simulates ais2 transponder |

These are distinct from the interactive simulator (`test_ais.py`) which uses
4008/4009, so both can run simultaneously without conflict.

---

## Test Suite Summary

### `test_receive.py` — 20 tests

Core AIS data path: NMEA sentence → manager → `uuv/sensors/ais`.

| Class | What it checks |
|---|---|
| `TestBasicPublish` | Type 1 and type 18 trigger an MQTT publish; publish is event-driven (< 2 s latency, not timer-based) |
| `TestNonAivdmFiltering` | `!AIVDO`, `$GPGGA`, garbage text, and empty lines on the AIS input channel are silently discarded with no MQTT publish |
| `TestChecksumValidation` | Bad NMEA checksum is dropped; next valid packet still publishes |
| `TestMultipartAivdm` | Type 5 two-fragment sequence publishes exactly once (on second fragment); type 24 A+B publishes correctly |
| `TestDeviceTagging` | ch1 data → `device_id=1`; ch2 data → `device_id=2`; simultaneous sends are correctly isolated |
| `TestPayloadStructure` | JSON has `ts`, `device_id`, `device_name`, `vessel_count`, `vessels`; each vessel has `mmsi`, `device_id`, `msg_type`, `recv_ts`; `vessel_count == len(vessels)` |

### `test_gga.py` — 7 tests

GGA MQTT subscription → AIS device TCP forwarding.

| Test | What it checks |
|---|---|
| `test_gga_forwarded_to_ch1/ch2` | Publishing to `uuv/gnss/gga` reaches the device's TCP connection |
| `test_gga_forwarded_to_both_devices` | A single GGA publish arrives on both ch1 and ch2 |
| `test_gga_content_preserved` | Time field in the forwarded sentence matches the MQTT payload |
| `test_multiple_gga_publishes_all_forwarded` | Three sequential publishes each reach the device |
| `test_gga_not_forwarded_when_channel_disabled` | `output_channels.gga.enabled=false` blocks forwarding |
| `test_gga_publish_does_not_suppress_ais_publish` | GGA forwarding and AIS publish are independent threads |

### `test_status.py` — 10 tests

`ais/status` heartbeat topic.

| Class | What it checks |
|---|---|
| `TestStatusPublishTiming` | Arrives within 2× the configured interval; repeats; disabled when `publish_interval_ms=0` |
| `TestStatusStructure` | JSON has `ts`, `health`, `devices`; devices array has 2 entries; each entry has `id`, `name`, `connected`, `health`; `ts` is a positive number |
| `TestDeviceHealth` | Both devices show `connected=true` when servers are up; at least one shows disconnected when no server is listening |

### `test_transport.py` — 8 tests

TCP transport lifecycle and multi-device isolation.

| Class | What it checks |
|---|---|
| `TestDualDeviceConnect` | Both ch1 and ch2 connect on startup as independent TCP sockets |
| `TestReconnect` | Manager reconnects to ch1 after connection drop; publish resumes after reconnect; ch2 is unaffected by ch1 drop |
| `TestDeviceIsolation` | ch1 vessels never tagged with ch2's device_id; simultaneous packets from both channels tagged correctly |
| `TestInitCommands` | `init_commands` in device config are sent to the device after connect |

### `test_config.py` — 7 tests

Config-driven runtime behaviour.

| Class | What it checks |
|---|---|
| `TestPublishIntervalZero` | `ais` topic `publish_interval_ms=0` disables publish for all packets including multiple |
| `TestChecksumValidationDisabled` | `validate_checksum=false` accepts bad-CRC packets; good-CRC packets still work |
| `TestIntervalNormalisation` | `publish_interval_ms=500` is normalised to 1000 at load time (topic stays enabled, not zeroed) |
| `TestSingleDeviceConfig` | Single-device config publishes correctly; status shows exactly 1 device |

---

## Test Infrastructure (`conftest.py`)

### `AisServer`

TCP server that simulates one AIS transponder. The manager connects to it.

```python
srv = AisServer("127.0.0.1", 14008, "AIS1")
srv.start()
srv.wait_connected(timeout=6)      # wait for manager to connect
srv.send_line("!AIVDM,...")        # push NMEA to manager
srv.wait_for_line("$GPGGA")        # wait for line from manager
srv.received_lines()               # all lines received from manager
srv.stop()
```

### `MqttMonitor`

Subscribes to a list of topics and collects JSON payloads.

```python
mon = MqttMonitor("localhost", 1883, ["uuv/sensors/ais", "ais/status"])
mon.start()
msg = mon.wait_for_message("uuv/sensors/ais", timeout=5.0,
                            predicate=lambda m: m["vessel_count"] > 0)
ok  = mon.no_message_within("uuv/sensors/ais", window=1.5)
mon.publish("uuv/gnss/gga", "$GPGGA,...")   # inject input
mon.stop()
```

### `make_config(**kwargs)`

Generates a test config from the production `ais_config.json` template with
overrides applied:

| Parameter | Default | Effect |
|---|---|---|
| `ch1_port` | 14008 | AIS1 transport port |
| `ch2_port` | 14009 | AIS2 transport port |
| `broker` | `localhost` | MQTT broker host |
| `gga_enabled` | `True` | `output_channels.gga.enabled` for all devices |
| `ais_pub_interval_ms` | 1000 | `publish_interval_ms` for the `ais` MQTT topic |
| `status_interval_ms` | 3000 | `publish_interval_ms` for the `status` MQTT topic |
| `validate_checksum` | `True` | `validate_checksum` for all devices |
| `single_device` | `False` | Truncate to one device + one transport |

### Fixtures

| Fixture | Scope | Yields |
|---|---|---|
| `require_mqtt` | session | skips if broker unreachable |
| `server_ch1` | function | `AisServer` on port 14008 |
| `server_ch2` | function | `AisServer` on port 14009 |
| `monitor` | function | `MqttMonitor` on all three topics |
| `default_env` | function | `(srv1, srv2, monitor, manager)` — both channels connected |
| `single_env` | function | `(srv1, monitor, manager)` — ch1 only |

### `helpers.py`

Re-exports NMEA encoders from `test_ais.py` and adds test-specific utilities:

```python
from helpers import (
    build_type1,          # !AIVDM Class A position
    build_type18,         # !AIVDM Class B position
    build_type5,          # !AIVDM static/voyage (2 fragments) → list[str]
    build_type24,         # !AIVDM Class B static (2 sentences) → list[str]
    corrupt_checksum,     # flip last CRC nibble
    make_gpgga,           # build a valid $GPGGA sentence
    wait_until,           # poll condition with timeout
)
```

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| All tests skipped | MQTT broker not running — start `mosquitto -v &` |
| Tests skipped with "Binary not found" | Run `cmake --build build` first |
| `AssertionError: Manager did not connect to ch1` | Port 14008 already in use; kill other process or change `TEST_PORT_CH1` in `conftest.py` |
| `wait_for_message` returns `None` | MQTT publish not happening — run manager manually with the generated config and watch `mosquitto_sub -t 'uuv/#' -v` |
| GGA forwarding tests fail | Check `output_channels.gga.enabled=true` in config and that the GGA output loop is running |
