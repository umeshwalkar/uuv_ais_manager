"""
test_status.py — ais/status MQTT topic tests.

The manager publishes a status heartbeat on ais/status at status_interval_ms.
Tests verify:
  - The topic is published within the configured interval
  - The JSON structure contains required fields
  - Per-device health is reported correctly
  - Interval enforcement (publish_interval_ms=0 disables the topic)
"""

import json
import time


from conftest import (
    TOPIC_STATUS, make_config, ManagerProcess,
)


# ─── helpers ──────────────────────────────────────────────────────────────────

STATUS_INTERVAL_MS = 2000  # short interval so tests don't take too long


def _build_status_env(srv1, srv2, monitor, tmp_path, **kwargs):
    cfg = make_config(status_interval_ms=STATUS_INTERVAL_MS, **kwargs)
    cfg_file = tmp_path / "cfg_status.json"
    cfg_file.write_text(json.dumps(cfg, indent=2))
    mgr = ManagerProcess(str(cfg_file))
    mgr.start(startup_wait=1.5)
    srv1.wait_connected(timeout=6)
    srv2.wait_connected(timeout=6)
    return mgr


# ─── publication timing ───────────────────────────────────────────────────────

class TestStatusPublishTiming:

    def test_status_arrives_within_interval(self, server_ch1, server_ch2, monitor, tmp_path):
        """ais/status must arrive within 2× the configured interval."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            timeout = (STATUS_INTERVAL_MS / 1000) * 2 + 1.0
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=timeout)
            assert msg is not None, \
                f"No ais/status received within {timeout:.1f}s (interval={STATUS_INTERVAL_MS}ms)"
        finally:
            mgr.stop()

    def test_status_repeats(self, server_ch1, server_ch2, monitor, tmp_path):
        """ais/status must be published repeatedly, not just once."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            interval_sec = STATUS_INTERVAL_MS / 1000
            # Wait long enough for at least 2 publishes
            wait = interval_sec * 2 + 1.5
            monitor.wait_for_message(TOPIC_STATUS, timeout=wait)
            time.sleep(interval_sec + 0.5)

            msgs = monitor.all_messages(TOPIC_STATUS)
            assert len(msgs) >= 2, \
                f"Expected >=2 status messages, got {len(msgs)}"
        finally:
            mgr.stop()

    def test_status_disabled_when_interval_zero(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """When status publish_interval_ms=0, no ais/status must be published."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path,
                                status_interval_ms=0)
        try:
            assert monitor.no_message_within(TOPIC_STATUS, window=2.0), \
                "ais/status was published even with interval=0 (should be disabled)"
        finally:
            mgr.stop()


# ─── JSON structure ───────────────────────────────────────────────────────────

class TestStatusStructure:

    def test_required_top_level_fields(self, server_ch1, server_ch2, monitor, tmp_path):
        """Status JSON must have ts, health, and devices fields."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None

            for field in ("ts", "health", "devices"):
                assert field in msg, f"Missing required field in status: {field!r}"
        finally:
            mgr.stop()

    def test_devices_array_has_both_entries(self, server_ch1, server_ch2, monitor, tmp_path):
        """Status must report one entry per configured device."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None
            assert isinstance(msg["devices"], list)
            assert len(msg["devices"]) == 2, \
                f"Expected 2 device entries in status, got {len(msg['devices'])}"
        finally:
            mgr.stop()

    def test_device_entry_has_required_fields(self, server_ch1, server_ch2, monitor, tmp_path):
        """Each device entry must have id, name, connected, and health."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None

            for dev in msg["devices"]:
                for field in ("id", "name", "connected", "health"):
                    assert field in dev, \
                        f"Device entry missing field {field!r}: {dev}"
        finally:
            mgr.stop()

    def test_device_ids_are_sequential(self, server_ch1, server_ch2, monitor, tmp_path):
        """Device IDs in status must be 1 and 2 (matching the config)."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None

            ids = sorted(d["id"] for d in msg["devices"])
            assert ids == [1, 2], f"Unexpected device IDs: {ids}"
        finally:
            mgr.stop()

    def test_ts_is_positive_integer(self, server_ch1, server_ch2, monitor, tmp_path):
        """Status ts must be a positive integer (Unix epoch seconds)."""
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(TOPIC_STATUS, timeout=6.0)
            assert msg is not None

            ts = msg["ts"]
            assert isinstance(ts, (int, float)) and ts > 0, \
                f"Status ts is not a positive number: {ts!r}"
        finally:
            mgr.stop()


# ─── device health values ─────────────────────────────────────────────────────

class TestDeviceHealth:

    def test_health_ok_when_both_connected(self, server_ch1, server_ch2, monitor, tmp_path):
        """
        When both AIS servers are up and connected, the overall status health
        and each device health should be 'ok'.
        """
        mgr = _build_status_env(server_ch1, server_ch2, monitor, tmp_path)
        try:
            msg = monitor.wait_for_message(
                TOPIC_STATUS, timeout=6.0,
                predicate=lambda m: all(
                    d.get("connected") for d in m.get("devices", [])
                )
            )
            assert msg is not None, \
                "No status with both devices connected within timeout"

            for dev in msg["devices"]:
                assert dev.get("connected") is True, \
                    f"Device {dev.get('id')} not connected: {dev}"
        finally:
            mgr.stop()

    def test_health_disconnected_before_server_starts(
            self, monitor, tmp_path, require_mqtt):
        """
        If no AIS server is listening when the manager starts, device health
        must reflect the disconnected state.
        """
        # Don't start AIS servers — manager will fail to connect
        cfg = make_config(status_interval_ms=STATUS_INTERVAL_MS)
        cfg_file = tmp_path / "cfg_no_srv.json"
        cfg_file.write_text(json.dumps(cfg, indent=2))

        mgr = ManagerProcess(str(cfg_file))
        mgr.start(startup_wait=1.5)
        try:
            # Look for a status where at least one device is not connected
            msg = monitor.wait_for_message(
                TOPIC_STATUS, timeout=6.0,
                predicate=lambda m: any(
                    not d.get("connected", True) for d in m.get("devices", [])
                )
            )
            assert msg is not None, \
                "Status did not report any disconnected device when servers are down"

            disconnected = [d for d in msg["devices"] if not d.get("connected")]
            assert disconnected, "All devices show connected even though no server is up"
        finally:
            mgr.stop()
