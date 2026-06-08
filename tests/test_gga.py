"""
test_gga.py — GGA MQTT subscription → AIS device TCP forwarding.

When the manager receives a message on uuv/gnss/gga it must forward the
GGA sentence over every output_channel that has gga.enabled=true.

Config-driven variants:
  - gga_enabled=True  (default): device receives the sentence
  - gga_enabled=False: device does NOT receive the sentence
"""

import json
import time


from conftest import TOPIC_GGA, TOPIC_AIS, make_config, ManagerProcess
from helpers import make_gpgga, build_type1


# ─── helpers ──────────────────────────────────────────────────────────────────

def _start_env_gga_off(srv1, srv2, monitor, tmp_path):
    """Stand up a manager with gga output disabled on both devices."""
    cfg = make_config(gga_enabled=False)
    cfg_file = tmp_path / "cfg_gga_off.json"
    cfg_file.write_text(json.dumps(cfg, indent=2))
    mgr = ManagerProcess(str(cfg_file))
    mgr.start(startup_wait=1.5)
    assert srv1.wait_connected(timeout=6), "ch1 did not connect"
    assert srv2.wait_connected(timeout=6), "ch2 did not connect"
    return mgr


# ─── GGA forwarding enabled (default) ────────────────────────────────────────

class TestGgaForwardingEnabled:

    def test_gga_forwarded_to_ch1(self, default_env):
        """
        Publishing a $GPGGA to uuv/gnss/gga must reach the ch1 AIS device's
        TCP connection (manager's GGA output channel).
        """
        srv1, srv2, mon, mgr = default_env
        srv1.clear_received()

        gga = make_gpgga()
        mon.publish(TOPIC_GGA, gga)

        line = srv1.wait_for_line("$GPGGA", timeout=4.0)
        assert line is not None, "ch1 did not receive GGA after MQTT publish"
        assert "$GPGGA" in line

    def test_gga_forwarded_to_ch2(self, default_env):
        """Same as above but for the ch2 AIS device."""
        srv1, srv2, mon, mgr = default_env
        srv2.clear_received()

        gga = make_gpgga(lat_deg=18.85, lon_deg=72.92)
        mon.publish(TOPIC_GGA, gga)

        line = srv2.wait_for_line("$GPGGA", timeout=4.0)
        assert line is not None, "ch2 did not receive GGA after MQTT publish"

    def test_gga_forwarded_to_both_devices(self, default_env):
        """
        A single GGA publish must be forwarded to both ch1 and ch2.
        """
        srv1, srv2, mon, mgr = default_env
        srv1.clear_received()
        srv2.clear_received()

        gga = make_gpgga()
        mon.publish(TOPIC_GGA, gga)

        line1 = srv1.wait_for_line("$GPGGA", timeout=4.0)
        line2 = srv2.wait_for_line("$GPGGA", timeout=4.0)

        assert line1 is not None, "ch1 did not receive GGA"
        assert line2 is not None, "ch2 did not receive GGA"

    def test_gga_content_preserved(self, default_env):
        """The forwarded GGA sentence must carry the same data as the MQTT payload."""
        srv1, srv2, mon, mgr = default_env
        srv1.clear_received()

        lat, lon = 18.9123, 72.8456
        gga = make_gpgga(lat_deg=lat, lon_deg=lon, time_str="093045")
        mon.publish(TOPIC_GGA, gga)

        line = srv1.wait_for_line("$GPGGA", timeout=4.0)
        assert line is not None
        # The time field (093045) must survive intact
        assert "093045" in line, f"Time field not preserved in forwarded GGA: {line!r}"

    def test_multiple_gga_publishes_all_forwarded(self, default_env):
        """Three sequential GGA publishes must each reach the device."""
        srv1, srv2, mon, mgr = default_env
        srv1.clear_received()

        for i in range(3):
            mon.publish(TOPIC_GGA, make_gpgga(time_str=f"12{i:02d}00"))
            time.sleep(0.1)

        # Allow all to propagate
        time.sleep(0.5)
        lines = [l for l in srv1.received_lines() if l.startswith("$GPGGA")]
        assert len(lines) >= 3, \
            f"Expected at least 3 GGA lines on ch1, got {len(lines)}"


# ─── GGA forwarding disabled via config ───────────────────────────────────────

class TestGgaForwardingDisabled:

    def test_gga_not_forwarded_when_channel_disabled(
            self, server_ch1, server_ch2, monitor, tmp_path):
        """
        With output_channels.gga.enabled=false, a GGA MQTT message must NOT
        be forwarded to the AIS device's TCP connection.
        """
        mgr = _start_env_gga_off(server_ch1, server_ch2, monitor, tmp_path)
        try:
            server_ch1.clear_received()

            monitor.publish(TOPIC_GGA, make_gpgga())
            time.sleep(1.5)  # wait long enough for forwarding to have happened

            lines = [l for l in server_ch1.received_lines() if l.startswith("$GPGGA")]
            assert len(lines) == 0, \
                f"GGA unexpectedly forwarded when channel is disabled: {lines}"
        finally:
            mgr.stop()


# ─── GGA does not interfere with AIS publish ─────────────────────────────────

class TestGgaAisIndependence:

    def test_gga_publish_does_not_suppress_ais_publish(self, single_env):
        """
        GGA forwarding and AIS publish are independent.  Publishing GGA while
        AIS packets are streaming must not block or delay AIS publishes.
        """
        srv, mon, mgr = single_env
        srv.clear_received()

        # Simultaneously send AIS data and a GGA
        mon.publish(TOPIC_GGA, make_gpgga())
        srv.send_line(build_type1(338123456, 18.92, 72.84, 5.0, 90.0, 90))

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=lambda m: any(
                                       v.get("mmsi") == 338123456
                                       for v in m.get("vessels", [])))
        assert msg is not None, "AIS publish was suppressed by concurrent GGA forward"
