"""
test_receive.py — AIVDM receive, filtering, CRC validation, and device tagging.

Tests the core AIS data path:
  AIS transponder (TCP) → ais_manager → MQTT uuv/sensors/ais

Each test uses the default_env fixture (both ch1 and ch2 connected) unless it
only needs a single device, in which case it uses single_env.
"""

import json
import time


from conftest import TOPIC_AIS, make_config
from helpers import (
    build_type1, build_type5, build_type18, build_type24,
    corrupt_checksum, make_gpgga,
)


# ─── helpers ──────────────────────────────────────────────────────────────────

MMSI_CH1 = 338000001
MMSI_CH2 = 477000002


def _ais_with_mmsi(mmsi: int):
    """Predicate: MQTT payload contains a vessel with the given MMSI."""
    def _check(msg: dict) -> bool:
        return any(v.get("mmsi") == mmsi for v in msg.get("vessels", []))
    return _check


# ─── type 1 / type 18 basic publish ──────────────────────────────────────────

class TestBasicPublish:

    def test_type1_triggers_immediate_publish(self, single_env):
        """A valid !AIVDM type 1 sentence must produce an MQTT message."""
        srv, mon, mgr = single_env
        sentence = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)

        srv.send_line(sentence)
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))

        assert msg is not None, "No MQTT publish received after sending !AIVDM type 1"

    def test_type18_triggers_publish(self, single_env):
        """A valid !AIVDM type 18 sentence must produce an MQTT message."""
        srv, mon, mgr = single_env
        sentence = build_type18(MMSI_CH1, 18.92, 72.84, 3.0, 45.0, 45)

        srv.send_line(sentence)
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))

        assert msg is not None, "No MQTT publish received after sending !AIVDM type 18"

    def test_publish_is_event_driven_not_timer(self, single_env):
        """
        Publish must happen quickly after receiving the packet, not after a
        publish_interval_ms timer fires.  We check arrival within 2 s.
        """
        srv, mon, mgr = single_env
        sentence = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 180.0, 180)

        t0 = time.monotonic()
        srv.send_line(sentence)
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        elapsed = time.monotonic() - t0

        assert msg is not None, "MQTT publish not received"
        assert elapsed < 2.0, f"Publish took {elapsed:.2f}s — expected event-driven (<2 s)"


# ─── non-AIVDM filtering ─────────────────────────────────────────────────────

class TestNonAivdmFiltering:

    def test_aivdo_line_is_ignored(self, single_env):
        """!AIVDO (own-vessel) must not trigger an MQTT publish."""
        srv, mon, mgr = single_env
        # Build a valid type 1 body but wrap it as !AIVDO
        type1_body = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)
        aivdo = type1_body.replace("!AIVDM,", "!AIVDO,", 1)
        # Recompute checksum so it's not rejected for CRC reasons
        body_part = aivdo[1:aivdo.rindex("*")]
        cs = 0
        for c in body_part:
            cs ^= ord(c)
        aivdo = f"!{body_part}*{cs:02X}"

        srv.send_line(aivdo)
        assert mon.no_message_within(TOPIC_AIS, window=1.5), \
            "MQTT publish triggered by !AIVDO (should be ignored)"

    def test_gpgga_on_aivdm_channel_is_ignored(self, single_env):
        """$GPGGA arriving on the AIS input channel must not trigger an MQTT publish."""
        srv, mon, mgr = single_env
        srv.send_line(make_gpgga())
        assert mon.no_message_within(TOPIC_AIS, window=1.5), \
            "MQTT publish triggered by $GPGGA on AIS input channel"

    def test_garbage_line_is_ignored(self, single_env):
        """Random ASCII garbage must not trigger an MQTT publish."""
        srv, mon, mgr = single_env
        srv.send_line("HELLO WORLD GARBAGE 12345")
        assert mon.no_message_within(TOPIC_AIS, window=1.5), \
            "MQTT publish triggered by garbage line"

    def test_empty_line_is_ignored(self, single_env):
        """Empty or whitespace-only lines must not trigger an MQTT publish."""
        srv, mon, mgr = single_env
        srv.send_line("   ")
        srv.send_line("")
        assert mon.no_message_within(TOPIC_AIS, window=1.5), \
            "MQTT publish triggered by empty line"

    def test_multiple_garbage_then_valid_publishes(self, single_env):
        """
        After several ignored lines the first valid !AIVDM must still publish.
        Verifies that the filter does not corrupt state.
        """
        srv, mon, mgr = single_env
        for junk in ["$GPRMC,123519,A,...", "!AIVDO,1,1,,A,xxx,0*00", "GARBAGE"]:
            srv.send_line(junk)

        sentence = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)
        srv.send_line(sentence)

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None, "Publish did not arrive after valid !AIVDM following garbage"


# ─── CRC validation ───────────────────────────────────────────────────────────

class TestChecksumValidation:

    def test_bad_checksum_is_dropped(self, single_env):
        """Packet with wrong NMEA checksum must not trigger publish when validate_checksum=true."""
        srv, mon, mgr = single_env
        good = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)
        bad  = corrupt_checksum(good)

        srv.send_line(bad)
        assert mon.no_message_within(TOPIC_AIS, window=1.5), \
            "MQTT publish triggered by !AIVDM with corrupted checksum"

    def test_valid_checksum_after_bad_publishes(self, single_env):
        """A valid packet sent after a bad-checksum one must still publish."""
        srv, mon, mgr = single_env
        good = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)
        bad  = corrupt_checksum(good)

        srv.send_line(bad)
        # Give the manager a moment to process (and discard) the bad packet
        time.sleep(0.2)
        srv.send_line(good)

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None, "No publish after valid packet following a bad-CRC packet"


# ─── multi-part AIVDM (type 5) ───────────────────────────────────────────────

class TestMultipartAivdm:

    def test_type5_publishes_only_on_second_fragment(self, single_env):
        """
        Type 5 (static/voyage) comes in two fragments.  The manager must hold
        state until both fragments arrive and then publish exactly once.
        """
        srv, mon, mgr = single_env
        mon.clear(TOPIC_AIS)

        frags = build_type5(MMSI_CH1, "TEST VESSEL", "T1CALL", 9000001)
        # Send only the first fragment — no publish should happen
        srv.send_line(frags[0])
        assert mon.no_message_within(TOPIC_AIS, window=1.0), \
            "Publish after first fragment only (should wait for second)"

        # Send second fragment — publish must now arrive
        srv.send_line(frags[1])
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None, "No publish after sending both type-5 fragments"

    def test_type5_publish_count_is_one(self, single_env):
        """Two-fragment sequence must produce exactly one MQTT message."""
        srv, mon, mgr = single_env
        mon.clear(TOPIC_AIS)

        frags = build_type5(MMSI_CH1, "TEST VESSEL 2", "T2CALL", 9000002)
        for f in frags:
            srv.send_line(f)
            time.sleep(0.05)

        # Wait for publish then give extra time for any duplicates
        mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                             predicate=_ais_with_mmsi(MMSI_CH1))
        time.sleep(0.5)

        msgs = [m for m in mon.all_messages(TOPIC_AIS)
                if any(v.get("mmsi") == MMSI_CH1 for v in m.get("vessels", []))]
        assert len(msgs) == 1, \
            f"Expected exactly 1 publish for type-5, got {len(msgs)}"

    def test_type24_two_sentences_publish(self, single_env):
        """Class B type 24 comes as two independent sentences (part A + B)."""
        srv, mon, mgr = single_env
        parts = build_type24(MMSI_CH1, "CLASS B SHIP", "CB1234")
        for p in parts:
            srv.send_line(p)
            time.sleep(0.05)

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None, "No publish for type-24 sentences"


# ─── device tagging ───────────────────────────────────────────────────────────

class TestDeviceTagging:

    def test_ch1_data_tagged_with_device_1(self, default_env):
        """Vessel received on ch1 must carry device_id matching ais1 config."""
        srv1, srv2, mon, mgr = default_env
        sentence = build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90)
        srv1.send_line(sentence)

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None, "No publish for ch1 vessel"
        vessel = next(v for v in msg["vessels"] if v["mmsi"] == MMSI_CH1)
        assert vessel["device_id"] == 1, \
            f"ch1 vessel has wrong device_id: {vessel['device_id']}"

    def test_ch2_data_tagged_with_device_2(self, default_env):
        """Vessel received on ch2 must carry device_id matching ais2 config."""
        srv1, srv2, mon, mgr = default_env
        sentence = build_type1(MMSI_CH2, 18.90, 72.86, 4.0, 180.0, 180)
        srv2.send_line(sentence)

        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH2))
        assert msg is not None, "No publish for ch2 vessel"
        vessel = next(v for v in msg["vessels"] if v["mmsi"] == MMSI_CH2)
        assert vessel["device_id"] == 2, \
            f"ch2 vessel has wrong device_id: {vessel['device_id']}"

    def test_ch1_and_ch2_publish_independently(self, default_env):
        """Data on ch1 must not appear tagged as ch2 and vice versa."""
        srv1, srv2, mon, mgr = default_env

        srv1.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        srv2.send_line(build_type1(MMSI_CH2, 18.90, 72.86, 4.0, 45.0, 45))

        msg1 = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                    predicate=_ais_with_mmsi(MMSI_CH1))
        msg2 = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                    predicate=_ais_with_mmsi(MMSI_CH2))

        assert msg1 is not None
        assert msg2 is not None

        v1 = next(v for v in msg1["vessels"] if v["mmsi"] == MMSI_CH1)
        v2 = next(v for v in msg2["vessels"] if v["mmsi"] == MMSI_CH2)

        assert v1["device_id"] != v2["device_id"], \
            "ch1 and ch2 have same device_id — tagging is broken"


# ─── MQTT payload structure ───────────────────────────────────────────────────

class TestPayloadStructure:

    def test_required_top_level_fields(self, single_env):
        """Published JSON must contain ts, device_id, device_name, vessel_count, vessels."""
        srv, mon, mgr = single_env
        srv.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0)
        assert msg is not None

        for field in ("ts", "device_id", "device_name", "vessel_count", "vessels"):
            assert field in msg, f"Missing required field: {field!r}"

    def test_vessel_has_required_fields(self, single_env):
        """Each vessel entry must have mmsi, device_id, device_name, msg_type, recv_ts."""
        srv, mon, mgr = single_env
        srv.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None and msg["vessels"]

        vessel = msg["vessels"][0]
        for field in ("mmsi", "device_id", "device_name", "msg_type", "recv_ts"):
            assert field in vessel, f"Vessel missing field: {field!r}"

    def test_vessel_mmsi_matches_sent(self, single_env):
        """The MMSI in the published vessel must match what was encoded in the packet."""
        srv, mon, mgr = single_env
        srv.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0,
                                   predicate=_ais_with_mmsi(MMSI_CH1))
        assert msg is not None

        mmsis = [v["mmsi"] for v in msg["vessels"]]
        assert MMSI_CH1 in mmsis, f"MMSI {MMSI_CH1} not in published vessels: {mmsis}"

    def test_vessel_count_matches_vessels_array(self, single_env):
        """vessel_count must equal len(vessels)."""
        srv, mon, mgr = single_env
        srv.send_line(build_type1(MMSI_CH1, 18.92, 72.84, 5.0, 90.0, 90))
        msg = mon.wait_for_message(TOPIC_AIS, timeout=5.0)
        assert msg is not None
        assert msg["vessel_count"] == len(msg["vessels"]), \
            "vessel_count does not match len(vessels)"
