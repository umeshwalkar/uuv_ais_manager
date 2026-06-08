"""
conftest.py — shared fixtures and infrastructure for ais_manager integration tests.

Every test drives the real ais_manager binary:
  1. AisServer(s) listen on test ports and simulate AIS transponders.
  2. ManagerProcess starts the binary with a generated config pointing at those ports.
  3. MqttMonitor subscribes to MQTT topics and collects payloads for assertions.

Port assignments (avoid clashing with the interactive simulator on 4008/4009):
  ch1 → 14008   ch2 → 14009
"""

import json
import os
import socket
import subprocess
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import pytest

# ── path constants ─────────────────────────────────────────────────────────────

REPO_ROOT   = Path(__file__).parent.parent
BINARY      = REPO_ROOT / "build" / "ais_manager"
BASE_CONFIG = REPO_ROOT / "config" / "ais_config.json"

TEST_PORT_CH1 = 14008
TEST_PORT_CH2 = 14009
MQTT_HOST     = "localhost"
MQTT_PORT     = 1883

TOPIC_AIS    = "uuv/sensors/ais"
TOPIC_STATUS = "ais/status"
TOPIC_GGA    = "uuv/gnss/gga"


# ── AisServer ──────────────────────────────────────────────────────────────────

class AisServer:
    """
    TCP server that impersonates one AIS transponder.
    The manager (tcp_client) connects to it.

    send_line(str)        → push an NMEA line to the manager
    received_lines()      → list of raw lines received FROM the manager (GGA etc.)
    wait_connected(t)     → blocks until a client connects or timeout
    wait_for_line(prefix) → wait until a received line starts with prefix
    """

    def __init__(self, host: str = "127.0.0.1", port: int = TEST_PORT_CH1,
                 label: str = "AIS"):
        self.host  = host
        self.port  = port
        self.label = label
        self._srv: socket.socket | None = None
        self._conn: socket.socket | None = None
        self._running = False
        self._received: list[str] = []
        self._lock = threading.Lock()
        self.connected = threading.Event()

    # ── lifecycle ───────────────────────────────────────────────────────────────

    def start(self):
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind((self.host, self.port))
        self._srv.listen(1)
        self._srv.settimeout(1.0)
        self._running = True
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def stop(self):
        self._running = False
        for s in (self._conn, self._srv):
            if s:
                try:
                    s.close()
                except OSError:
                    pass

    # ── internal ────────────────────────────────────────────────────────────────

    def _accept_loop(self):
        while self._running:
            try:
                conn, _ = self._srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            self._conn = conn
            self.connected.set()
            self._read_loop(conn)
            self.connected.clear()
            self._conn = None
            if self._running:
                # ready to accept the next reconnect
                self.connected.clear()

    def _read_loop(self, conn: socket.socket):
        conn.settimeout(0.1)
        buf = b""
        while self._running:
            try:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    decoded = line.decode("ascii", errors="replace").strip()
                    if decoded:
                        with self._lock:
                            self._received.append(decoded)
            except socket.timeout:
                continue
            except OSError:
                break

    # ── public interface ────────────────────────────────────────────────────────

    def send_line(self, line: str):
        conn = self._conn
        if conn:
            try:
                conn.sendall((line + "\r\n").encode("ascii"))
            except OSError:
                pass

    def wait_connected(self, timeout: float = 6.0) -> bool:
        return self.connected.wait(timeout)

    def received_lines(self) -> list[str]:
        with self._lock:
            return list(self._received)

    def clear_received(self):
        with self._lock:
            self._received.clear()

    def wait_for_line(self, prefix: str, timeout: float = 4.0) -> str | None:
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            with self._lock:
                lines = self._received
                for line in lines[seen:]:
                    if line.startswith(prefix):
                        return line
                seen = len(lines)
            time.sleep(0.05)
        return None


# ── MqttMonitor ───────────────────────────────────────────────────────────────

class MqttMonitor:
    """
    Subscribes to a set of MQTT topics and collects incoming JSON payloads.

    wait_for_message(topic, timeout, predicate) → dict or None
    no_message_within(topic, window)            → True if nothing arrived
    all_messages(topic)                         → list[dict]
    clear(topic)                                → discard collected messages
    """

    def __init__(self, host: str, port: int, topics: list[str]):
        self.host   = host
        self.port   = port
        self.topics = topics
        self._client: mqtt.Client | None = None
        self._messages: dict[str, list[str]] = {}
        self._lock = threading.Lock()
        self._ready = threading.Event()

    def start(self):
        self._client = mqtt.Client(
            client_id=f"test_monitor_{os.getpid()}_{id(self)}"
        )
        self._client.on_connect = self._on_connect
        self._client.on_message = self._on_message
        self._client.connect(self.host, self.port, keepalive=10)
        self._client.loop_start()
        if not self._ready.wait(5.0):
            raise RuntimeError(f"MqttMonitor: could not connect to {self.host}:{self.port}")

    def stop(self):
        if self._client:
            self._client.loop_stop()
            self._client.disconnect()

    def _on_connect(self, client, userdata, flags, rc):
        if rc == 0:
            for t in self.topics:
                client.subscribe(t)
            self._ready.set()

    def _on_message(self, client, userdata, msg):
        raw = msg.payload.decode("utf-8", errors="replace")
        with self._lock:
            self._messages.setdefault(msg.topic, []).append(raw)

    def wait_for_message(self, topic: str, timeout: float = 5.0,
                          predicate=None) -> dict | None:
        deadline = time.monotonic() + timeout
        seen = 0
        while time.monotonic() < deadline:
            with self._lock:
                msgs = self._messages.get(topic, [])
                for raw in msgs[seen:]:
                    try:
                        obj = json.loads(raw)
                        if predicate is None or predicate(obj):
                            return obj
                    except json.JSONDecodeError:
                        pass
                seen = len(msgs)
            time.sleep(0.05)
        return None

    def no_message_within(self, topic: str, window: float = 1.5) -> bool:
        """Return True if no new message arrives on topic within window seconds."""
        with self._lock:
            before = len(self._messages.get(topic, []))
        time.sleep(window)
        with self._lock:
            after = len(self._messages.get(topic, []))
        return after == before

    def all_messages(self, topic: str) -> list[dict]:
        with self._lock:
            result = []
            for raw in self._messages.get(topic, []):
                try:
                    result.append(json.loads(raw))
                except json.JSONDecodeError:
                    pass
            return result

    def clear(self, topic: str | None = None):
        with self._lock:
            if topic:
                self._messages.pop(topic, None)
            else:
                self._messages.clear()

    def publish(self, topic: str, payload: str):
        """Publish a message (used to inject GGA or other test inputs)."""
        if self._client:
            self._client.publish(topic, payload)


# ── ManagerProcess ────────────────────────────────────────────────────────────

class ManagerProcess:
    """Launches and manages the ais_manager binary as a subprocess."""

    def __init__(self, config_path: str):
        self.config_path = config_path
        self._proc: subprocess.Popen | None = None
        self._log: list[str] = []
        self._log_thread: threading.Thread | None = None

    def start(self, startup_wait: float = 2.0) -> "ManagerProcess":
        if not BINARY.exists():
            pytest.skip(f"Binary not found: {BINARY} — run 'cmake --build build' first")
        self._proc = subprocess.Popen(
            [str(BINARY), self.config_path],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )
        self._log_thread = threading.Thread(target=self._capture_output, daemon=True)
        self._log_thread.start()
        time.sleep(startup_wait)
        if self._proc.poll() is not None:
            raise RuntimeError(
                f"Manager exited early (rc={self._proc.returncode}):\n"
                + "\n".join(self._log[-30:])
            )
        return self

    def stop(self):
        if self._proc and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()

    def _capture_output(self):
        for line in self._proc.stdout:
            self._log.append(line.rstrip())

    def log_lines(self) -> list[str]:
        return list(self._log)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.stop()


# ── config factory ────────────────────────────────────────────────────────────

def make_config(
    *,
    ch1_port: int = TEST_PORT_CH1,
    ch2_port: int = TEST_PORT_CH2,
    broker: str = MQTT_HOST,
    gga_enabled: bool = True,
    ais_pub_interval_ms: int = 1000,
    status_interval_ms: int = 3000,
    validate_checksum: bool = True,
    single_device: bool = False,
) -> dict:
    """
    Build a test config from the production template, overriding transport
    ports/host and any behavioural flags needed by a specific test.
    """
    with open(BASE_CONFIG) as f:
        cfg = json.load(f)

    cfg["mqtt"]["broker"] = broker

    for pub in cfg["mqtt"]["topics"]["pub"]:
        if pub["name"] == "status":
            pub["publish_interval_ms"] = status_interval_ms
        if pub["name"] == "ais":
            pub["publish_interval_ms"] = ais_pub_interval_ms

    for tr in cfg["ais"]["transport"]:
        tr["host"] = "127.0.0.1"
        tr["reconnect_delay_sec"] = 1

    cfg["ais"]["transport"][0]["port"] = ch1_port
    cfg["ais"]["transport"][1]["port"] = ch2_port

    for dev in cfg["ais"]["devices"]:
        dev["validate_checksum"] = validate_checksum
        dev["output_channels"]["gga"]["enabled"] = gga_enabled

    if single_device:
        cfg["ais"]["devices"]   = cfg["ais"]["devices"][:1]
        cfg["ais"]["transport"] = cfg["ais"]["transport"][:1]

    return cfg


# ── session-scoped MQTT availability check ────────────────────────────────────

@pytest.fixture(scope="session")
def require_mqtt():
    """Skip the entire test session if no MQTT broker is available."""
    try:
        c = mqtt.Client(client_id="probe")
        c.connect(MQTT_HOST, MQTT_PORT, keepalive=5)
        c.disconnect()
    except Exception as exc:
        pytest.skip(f"MQTT broker not available on {MQTT_HOST}:{MQTT_PORT}: {exc}")


# ── per-test fixtures ─────────────────────────────────────────────────────────

@pytest.fixture
def server_ch1():
    srv = AisServer("127.0.0.1", TEST_PORT_CH1, "AIS1")
    srv.start()
    yield srv
    srv.stop()


@pytest.fixture
def server_ch2():
    srv = AisServer("127.0.0.1", TEST_PORT_CH2, "AIS2")
    srv.start()
    yield srv
    srv.stop()


@pytest.fixture
def monitor(require_mqtt):
    mon = MqttMonitor(MQTT_HOST, MQTT_PORT,
                      [TOPIC_AIS, TOPIC_STATUS, TOPIC_GGA])
    mon.start()
    yield mon
    mon.stop()


@pytest.fixture
def default_env(server_ch1, server_ch2, monitor, tmp_path):
    """
    Start both AIS servers, manager with default test config, MQTT monitor.
    Waits until both transport connections are established before yielding.
    Returns (server_ch1, server_ch2, monitor, manager).
    """
    cfg_file = tmp_path / "cfg.json"
    cfg_file.write_text(json.dumps(make_config(), indent=2))

    mgr = ManagerProcess(str(cfg_file))
    mgr.start(startup_wait=1.5)

    assert server_ch1.wait_connected(timeout=6), "Manager did not connect to ch1"
    assert server_ch2.wait_connected(timeout=6), "Manager did not connect to ch2"

    yield server_ch1, server_ch2, monitor, mgr

    mgr.stop()


@pytest.fixture
def single_env(server_ch1, monitor, tmp_path):
    """
    Single-device variant: only ch1, simpler setup for focused tests.
    Returns (server_ch1, monitor, manager).
    """
    cfg_file = tmp_path / "cfg.json"
    cfg_file.write_text(json.dumps(make_config(single_device=True), indent=2))

    mgr = ManagerProcess(str(cfg_file))
    mgr.start(startup_wait=1.5)

    assert server_ch1.wait_connected(timeout=6), "Manager did not connect to ch1"

    yield server_ch1, monitor, mgr

    mgr.stop()
