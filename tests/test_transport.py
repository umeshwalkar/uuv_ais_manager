"""
test_transport.py — TCP transport reconnect and multi-device isolation tests.

These tests verify the manager's transport layer behaviour:
  - Both devices establish independent TCP connections on startup
  - After a connection drop the manager reconnects automatically
  - Data on ch1 and ch2 does not cross-contaminate device tagging
  - init_commands (if configured) are sent on connect
"""

import json
import time
import threading


from conftest import (
    TOPIC_AIS, TOPIC_STATUS, make_config, ManagerProcess,
)
from helpers import build_type1, build_type18


MMSI_CH1 = 338111001
MMSI_CH2 = 477222002


# ─── startup: both devices connect ───────────────────────────────────────────

class TestDualDeviceConnect:

    def test_both_channels_connect_on_startup(self, default_env):
        """
        The default_env fixture already waits for both connections.
        This test just confirms both servers see a client.
        """
        srv1, srv2, mon, mgr = default_env
        assert srv1.connected.is_set(), "ch1 did not connect on startup"
        assert srv2.connected.is_set(), "ch2 did not connect on startup"

    def test_ch1_and_ch2_are_independent_connections(self, default_env):
        """
        Two distinct TCP connections must be made — one per device.
        Verify by checking each server's socket is a separate client.
        """
        srv1, srv2, mon, mgr = default_env
        # Both servers have accepted a connection
        assert srv1._conn is not None
        assert srv2._conn is not None
        # They must be different socket objects
        assert srv1._conn is not srv2._conn


# ─── reconnect after connection drop ─────────────────────────────────────────

class TestReconnect:

    def test_manager_reconnects_after_ch1_drop(self, default_env):
        """
        After ch1 closes its TCP connection the manager must reconnect
        within reconnect_delay_sec (1 s in test config) and resume publishing.
        """
        srv1, srv2, mon, mgr = default_env

        # Forcibly close the server-side connection
        if srv1._conn:
            srv1._conn.close()

        # Manager should reconnect and ch1 server should see a new connection
        reconnected = srv1.wait_connected(timeout=6.0)
        assert reconnected, "Manager did not reconnect to ch1 after connection drop"

    def test_publish_resumes_after_reconnect(self, default_env):
        """
        After reconnect, AIS data sent over ch1 must still produce MQTT publishes.
        """
        srv1, srv2, mon, mgr = default_env

        # Drop and wait for reconnect
        if srv1._conn:
            srv1._conn.close()
        reconnected = srv1.wait_connected(timeout=6.0)
        assert reconnected, "Manager did not reconnect to ch1"

        # Allow short settle time for the new connection to be ready
        time.sleep(0.3)
        mon.clear(TOPIC_AIS)

        srv1.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(
            TOPIC_AIS, timeout=5.0,
            predicate=lambda m: any(v.get("mmsi") == MMSI_CH1
                                    for v in m.get("vessels", []))
        )
        assert msg is not None, "No MQTT publish after reconnect to ch1"

    def test_ch2_unaffected_by_ch1_drop(self, default_env):
        """
        A ch1 disconnect must not interrupt ch2 data flow.
        """
        srv1, srv2, mon, mgr = default_env
        mon.clear(TOPIC_AIS)

        # Drop ch1
        if srv1._conn:
            srv1._conn.close()

        # ch2 should still work
        srv2.send_line(build_type1(MMSI_CH2, 18.90, 72.86, 4.0, 45.0, 45))
        msg = mon.wait_for_message(
            TOPIC_AIS, timeout=5.0,
            predicate=lambda m: any(v.get("mmsi") == MMSI_CH2
                                    for v in m.get("vessels", []))
        )
        assert msg is not None, "ch2 publish disrupted by ch1 connection drop"


# ─── cross-device data isolation ─────────────────────────────────────────────

class TestDeviceIsolation:

    def test_ch1_packet_not_tagged_as_ch2(self, default_env):
        """
        A vessel received on ch1 must never appear with ch2's device_id
        in the same or subsequent MQTT message.
        """
        srv1, srv2, mon, mgr = default_env
        mon.clear(TOPIC_AIS)

        srv1.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(
            TOPIC_AIS, timeout=5.0,
            predicate=lambda m: any(v.get("mmsi") == MMSI_CH1
                                    for v in m.get("vessels", []))
        )
        assert msg is not None
        v = next(v for v in msg["vessels"] if v["mmsi"] == MMSI_CH1)
        assert v["device_id"] == 1, \
            f"ch1 vessel incorrectly tagged with device_id={v['device_id']}"

    def test_simultaneous_packets_correctly_tagged(self, default_env):
        """
        When both channels send a packet at roughly the same time, each vessel
        must be tagged with its correct device.
        """
        srv1, srv2, mon, mgr = default_env
        mon.clear(TOPIC_AIS)

        # Fire both nearly simultaneously from separate threads
        def send1():
            srv1.send_line(build_type18(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))

        def send2():
            srv2.send_line(build_type18(MMSI_CH2, 18.90, 72.86, 4.0, 45.0, 45))

        t1 = threading.Thread(target=send1)
        t2 = threading.Thread(target=send2)
        t1.start(); t2.start()
        t1.join(); t2.join()

        msg1 = mon.wait_for_message(
            TOPIC_AIS, timeout=5.0,
            predicate=lambda m: any(v.get("mmsi") == MMSI_CH1
                                    for v in m.get("vessels", []))
        )
        msg2 = mon.wait_for_message(
            TOPIC_AIS, timeout=5.0,
            predicate=lambda m: any(v.get("mmsi") == MMSI_CH2
                                    for v in m.get("vessels", []))
        )
        assert msg1 is not None
        assert msg2 is not None

        v1 = next(v for v in msg1["vessels"] if v["mmsi"] == MMSI_CH1)
        v2 = next(v for v in msg2["vessels"] if v["mmsi"] == MMSI_CH2)

        assert v1["device_id"] == 1
        assert v2["device_id"] == 2


# ─── init commands ────────────────────────────────────────────────────────────

class TestInitCommands:

    def test_init_commands_sent_on_connect(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        If the device config has init_commands, they must be sent right after
        connection — before any AIVDM data arrives from the manager.
        """
        cfg = make_config()
        # Inject init commands into device 1 config
        init_cmds = ["AIVDM_START", "AIVDM_RATE 2"]
        cfg["ais"]["devices"][0]["init_commands"] = init_cmds
        cfg_file = tmp_path / "cfg_init.json"
        cfg_file.write_text(json.dumps(cfg, indent=2))

        mgr = ManagerProcess(str(cfg_file))
        mgr.start(startup_wait=1.5)
        try:
            server_ch1.wait_connected(timeout=6)

            # Allow time for init commands to be sent
            time.sleep(0.5)

            received = server_ch1.received_lines()
            for cmd in init_cmds:
                assert any(cmd in line for line in received), \
                    f"Init command {cmd!r} not found in data sent by manager: {received}"
        finally:
            mgr.stop()
