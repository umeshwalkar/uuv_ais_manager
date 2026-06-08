"""
test_config.py — config-driven behaviour tests.

Verifies that runtime behaviour changes correctly when specific config flags
are set:
  - publish_interval_ms = 0 → topic disabled, no MQTT publish
  - validate_checksum = false → bad-CRC packets accepted and published
  - publish_interval_ms 1–999 → normalised to 1000 at load time
  - Single-device config → only device 1 reported in status
"""

import json
import time


from conftest import (
    TOPIC_AIS, TOPIC_STATUS, make_config, ManagerProcess,
)
from helpers import build_type1, corrupt_checksum


MMSI = 338500099


# ─── helpers ──────────────────────────────────────────────────────────────────

def _env(srv1, srv2, monitor, tmp_path, **kwargs):
    cfg = make_config(**kwargs)
    cfg_file = tmp_path / "cfg_custom.json"
    cfg_file.write_text(json.dumps(cfg, indent=2))
    mgr = ManagerProcess(str(cfg_file))
    mgr.start(startup_wait=1.5)
    srv1.wait_connected(timeout=6)
    srv2.wait_connected(timeout=6)
    return mgr


# ─── publish_interval_ms = 0 disables topic ───────────────────────────────────

class TestPublishIntervalZero:

    def test_ais_topic_disabled_when_interval_zero(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        When the 'ais' pub topic has publish_interval_ms=0, receiving a valid
        !AIVDM must NOT produce any MQTT message on uuv/sensors/ais.
        """
        mgr = _env(server_ch1, server_ch2, monitor, tmp_path,
                   ais_pub_interval_ms=0)
        try:
            server_ch1.send_line(build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90))
            assert monitor.no_message_within(TOPIC_AIS, window=2.0), \
                "AIS topic published even though publish_interval_ms=0"
        finally:
            mgr.stop()

    def test_ais_topic_disabled_multiple_packets(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        Even after many valid AIVDM packets, no publish when interval=0.
        """
        mgr = _env(server_ch1, server_ch2, monitor, tmp_path,
                   ais_pub_interval_ms=0)
        try:
            for _ in range(5):
                server_ch1.send_line(build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90))
                time.sleep(0.1)
            assert monitor.no_message_within(TOPIC_AIS, window=1.5), \
                "AIS topic published despite publish_interval_ms=0"
        finally:
            mgr.stop()


# ─── validate_checksum = false ────────────────────────────────────────────────

class TestChecksumValidationDisabled:

    def test_bad_checksum_accepted_when_validation_off(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        With validate_checksum=false, a packet with a corrupted NMEA checksum
        must still be processed and published to MQTT.
        """
        mgr = _env(server_ch1, server_ch2, monitor, tmp_path,
                   validate_checksum=False)
        try:
            good    = build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90)
            bad_crc = corrupt_checksum(good)

            server_ch1.send_line(bad_crc)
            msg = monitor.wait_for_message(
                TOPIC_AIS, timeout=5.0,
                predicate=lambda m: any(v.get("mmsi") == MMSI
                                        for v in m.get("vessels", []))
            )
            assert msg is not None, \
                "Bad-CRC packet not published when validate_checksum=false"
        finally:
            mgr.stop()

    def test_good_checksum_still_published_when_validation_off(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        Disabling checksum validation must not break normal (good-CRC) packets.
        """
        mgr = _env(server_ch1, server_ch2, monitor, tmp_path,
                   validate_checksum=False)
        try:
            server_ch1.send_line(build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90))
            msg = monitor.wait_for_message(
                TOPIC_AIS, timeout=5.0,
                predicate=lambda m: any(v.get("mmsi") == MMSI
                                        for v in m.get("vessels", []))
            )
            assert msg is not None, \
                "Good-CRC packet not published with validate_checksum=false"
        finally:
            mgr.stop()


# ─── interval normalisation (1–999 → 1000) ───────────────────────────────────

class TestIntervalNormalisation:

    def test_sub_second_interval_normalised_to_1000(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        Config publish_interval_ms=500 must be normalised to 1000 at load time.
        The AIS topic should still function (not disabled), confirming the value
        was not clamped to 0.
        """
        mgr = _env(server_ch1, server_ch2, monitor, tmp_path,
                   ais_pub_interval_ms=500)
        try:
            server_ch1.send_line(build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90))
            msg = monitor.wait_for_message(
                TOPIC_AIS, timeout=5.0,
                predicate=lambda m: any(v.get("mmsi") == MMSI
                                        for v in m.get("vessels", []))
            )
            assert msg is not None, \
                "No publish after sub-second interval (500ms→1000ms normalisation may be broken)"
        finally:
            mgr.stop()


# ─── single-device config ─────────────────────────────────────────────────────

class TestSingleDeviceConfig:

    def test_single_device_publishes(self, server_ch1, monitor, tmp_path, require_mqtt):
        """
        A config with only one device must work correctly — one connection
        and normal AIS publish.
        """
        cfg = make_config(single_device=True)
        cfg_file = tmp_path / "cfg_single.json"
        cfg_file.write_text(json.dumps(cfg, indent=2))

        mgr = ManagerProcess(str(cfg_file))
        mgr.start(startup_wait=1.5)
        server_ch1.wait_connected(timeout=6)
        try:
            server_ch1.send_line(build_type1(MMSI, 18.92, 72.84, 5.0, 90.0, 90))
            msg = monitor.wait_for_message(
                TOPIC_AIS, timeout=5.0,
                predicate=lambda m: any(v.get("mmsi") == MMSI
                                        for v in m.get("vessels", []))
            )
            assert msg is not None, "Single-device config: no MQTT publish"
        finally:
            mgr.stop()

    def test_single_device_status_has_one_entry(
            self, server_ch1, monitor, tmp_path, require_mqtt):
        """
        With a single-device config the status must contain exactly one device.
        """
        cfg = make_config(single_device=True, status_interval_ms=2000)
        cfg_file = tmp_path / "cfg_single_st.json"
        cfg_file.write_text(json.dumps(cfg, indent=2))

        mgr = ManagerProcess(str(cfg_file))
        mgr.start(startup_wait=1.5)
        server_ch1.wait_connected(timeout=6)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None, "No status from single-device manager"
            assert len(msg["devices"]) == 1, \
                f"Expected 1 device in status, got {len(msg['devices'])}"
        finally:
            mgr.stop()
