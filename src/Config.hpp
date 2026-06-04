#pragma once
#include <string>
#include <vector>
#include <stdexcept>

// ── Transport ─────────────────────────────────────────────────────────────────

struct TransportConfig {
    std::string type               = "tcp_client"; // udp_server|tcp_client|tcp_server|serial
    std::string bind_host          = "0.0.0.0";
    int         bind_port          = 0;
    std::string host;
    int         port               = 0;
    std::string serial_port;
    int         serial_baud        = 9600;
    int         serial_data_bits   = 8;
    int         serial_stop_bits   = 1;
    std::string serial_parity      = "N";          // N=None E=Even O=Odd
    int         connect_timeout_sec  = 5;
    int         reconnect_delay_sec  = 3;
    int         read_timeout_ms      = 1000;
    int         buffer_size_bytes    = 1024;
    std::string shared_with;                       // reuse named device's transport
};

// ── MQTT ──────────────────────────────────────────────────────────────────────

struct MqttTopics {
    std::string ais      = "uuv/ais";
    std::string status   = "ais/status";
    std::string gnss_gga = "uuv/gnss/gga";        // subscribe: incoming GPS feed
};

struct MqttConfig {
    bool        enabled   = true;
    std::string broker    = "localhost";
    int         port      = 1883;
    std::string client_id = "ais_manager";
    int         keepalive = 60;
    int         qos       = 1;
    bool        retain    = false;
    MqttTopics  topics;
};

// ── GGA output channel ────────────────────────────────────────────────────────
// Each AIS device can have its own GGA output channel.  When enabled, the
// device receives the current GPS GGA position (pushed by AisManager from the
// gnss_gga MQTT topic) and periodically writes a $GPGGA sentence back to the
// physical transponder so it can broadcast its own-vessel position (AIVDO).
//
// transport.shared_with: set to the parent device's own name (e.g. "ais1") to
// reuse the same bidirectional TCP/serial connection for the output.
// Leave empty to use the device's own transport by default.

struct GgaOutputConfig {
    bool        enabled          = false;
    int         send_interval_ms = 1000;
    double      data_timeout_sec = 2.0;
    TransportConfig transport;                     // shared_with → same device transport
};

// ── AIS device ────────────────────────────────────────────────────────────────
// Each entry represents one physical AIS sensor (AIS1, AIS2 …).

struct AisDeviceConfig {
    int         id                    = 1;
    std::string name                  = "ais1";
    bool        enabled               = true;
    double      data_timeout_sec      = 5.0;
    double      sync_timeout_sec      = 5.0;
    bool        send_init_on_reconnect = false;
    std::vector<std::string> init_commands;
    TransportConfig  transport;
    GgaOutputConfig  gga_output;                   // per-device GPS output channel
};

// ── AIS application ───────────────────────────────────────────────────────────

struct AisConfig {
    int         publish_interval_ms    = 1000;
    double      data_timeout_sec       = 5.0;
    int         status_interval_sec    = 10;
    bool        publish_raw_ais        = false;
    bool        validate_crc           = true;
    bool        require_position_valid = false;

    std::vector<AisDeviceConfig> devices;          // AIS1, AIS2, …
};

// ── Top-level ─────────────────────────────────────────────────────────────────

struct AppConfig {
    MqttConfig mqtt;
    AisConfig  ais;

    static AppConfig fromFile(const std::string& path);
    static AppConfig fromJsonFile(const std::string& path);
    static AppConfig fromIniFile(const std::string& path);
};
