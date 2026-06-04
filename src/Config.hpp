#pragma once
#include <string>
#include <vector>
#include <stdexcept>

// ── Named transport definition (pool entry) ───────────────────────────────────
// Each entry has a unique string id.  Channels reference these by id via
// shared_with.  If enabled=false the transport is never opened; any channel
// referencing it is implicitly disabled, but the device still reports status.

struct TransportDef {
    std::string id;
    bool        enabled            = true;
    std::string type               = "tcp_client"; // tcp_client|tcp_server|udp_server|serial
    std::string bind_host          = "0.0.0.0";
    int         bind_port          = 0;
    std::string host;
    int         port               = 0;
    std::string serial_port;
    int         serial_baud        = 9600;
    int         serial_data_bits   = 8;
    int         serial_stop_bits   = 1;
    std::string serial_parity      = "N";
    int         connect_timeout_sec  = 5;
    int         reconnect_delay_sec  = 3;
    int         read_timeout_ms      = 1000;
    int         buffer_size_bytes    = 1024;
};

// ── Channel transport reference ───────────────────────────────────────────────
// A thin reference used inside input_channels / output_channels to point at
// one entry in the transport pool by its id.

struct ChannelTransportRef {
    std::string shared_with;     // must match a TransportDef.id
};

// ── MQTT ──────────────────────────────────────────────────────────────────────

struct MqttTopics {
    std::string ais      = "uuv/ais";
    std::string status   = "ais/status";
    std::string gnss_gga = "uuv/gnss/gga";  // subscribe: incoming GPS feed
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

// ── Channel configs ───────────────────────────────────────────────────────────

// input_channels.aivdm — receives !AIVDM / !AIVDO sentences from the device
struct AivdmChannelConfig {
    bool             enabled          = true;
    double           data_timeout_sec = 5.0;
    ChannelTransportRef transport;
};

// output_channels.gga — sends $GPGGA to the device (own-vessel position)
struct GgaChannelConfig {
    bool             enabled          = false;
    int              send_interval_ms = 1000;
    double           data_timeout_sec = 2.0;
    ChannelTransportRef transport;
};

// ── AIS device config ─────────────────────────────────────────────────────────
//
// enabled=false          → skip ALL operations for this device (not in status)
// enabled=true           → device is managed:
//   transport disabled   → channels disabled, device IS in ais/status (connected=false)
//   publish_enabled=true → vessels published to uuv/ais MQTT topic
//   publish_enabled=false→ transport/parsing still runs; NO uuv/ais publish;
//                          device IS still in ais/status

struct AisDeviceConfig {
    int         id                    = 1;
    std::string name                  = "ais1";
    bool        enabled               = true;
    double      sync_timeout_sec      = 5.0;
    bool        send_init_on_reconnect = false;
    std::vector<std::string> init_commands;

    bool        publish_enabled       = true;   // controls uuv/ais MQTT publish only
    bool        publish_raw_ais       = false;
    int         publish_interval_ms   = 1000;
    bool        validate_checksum     = true;

    AivdmChannelConfig  aivdm_in;   // input_channels.aivdm
    GgaChannelConfig    gga_out;    // output_channels.gga
};

// ── AIS application config ────────────────────────────────────────────────────

struct AisConfig {
    int status_interval_sec = 10;
    std::vector<TransportDef>    transports;   // named transport pool
    std::vector<AisDeviceConfig> devices;
};

// ── Top-level ─────────────────────────────────────────────────────────────────

struct AppConfig {
    MqttConfig mqtt;
    AisConfig  ais;

    static AppConfig fromFile(const std::string& path);
    static AppConfig fromJsonFile(const std::string& path);
    static AppConfig fromIniFile(const std::string& path);
};
