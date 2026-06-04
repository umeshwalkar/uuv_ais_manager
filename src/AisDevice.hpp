#pragma once
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include "Config.hpp"
#include "Transport.hpp"
#include "AisParser.hpp"

// ── Per-vessel record held by each AisDevice ──────────────────────────────────

struct VesselRecord {
    AisData     data;
    int         source_device_id   = 0;
    std::string source_device_name;
    std::chrono::system_clock::time_point last_seen;
};

// ── Thread-safe snapshot returned by AisDevice::snapshot() ───────────────────

struct DeviceSnapshot {
    int         id                = 0;
    std::string name;
    bool        connected         = false;
    bool        is_valid          = false;
    double      last_recv_ts      = 0.0;
    uint64_t    packets_received  = 0;
    uint64_t    parse_errors      = 0;
    uint64_t    crc_errors        = 0;
    bool        gga_output_enabled = false;
    uint64_t    gga_sent_count    = 0;
    std::unordered_map<uint32_t, VesselRecord> vessels;
};

// ── AisDevice ─────────────────────────────────────────────────────────────────
/**
 * Manages one physical AIS sensor.  Owns the transport, runs a receive thread
 * that parses !AIVDM/!AIVDO sentences, and maintains a per-MMSI vessel table.
 *
 * If cfg.gga_output.enabled is true, a second thread periodically sends the
 * latest $GPGGA sentence back to the same transport so the transponder can
 * broadcast its own-vessel position (AIVDO).  The latest GGA is pushed in by
 * AisManager via setGga() whenever fresh GPS data arrives.
 */
class AisDevice {
public:
    AisDevice(const AisDeviceConfig& cfg, bool validate_crc);
    ~AisDevice();

    void start();
    void stop();

    DeviceSnapshot snapshot() const;

    // Called by AisManager to push latest GPS GGA sentence to this device
    void setGga(const std::string& gga_sentence, double ts);

    int                id()      const { return cfg_.id;      }
    const std::string& name()    const { return cfg_.name;    }
    bool               enabled() const { return cfg_.enabled; }

private:
    AisDeviceConfig             cfg_;
    bool                        validate_crc_;
    std::unique_ptr<ITransport> transport_;
    AisParser                   parser_;

    // State protected by mtx_
    mutable std::mutex          mtx_;
    DeviceSnapshot              state_;

    // Shared stop signal + wake-up
    std::atomic<bool>           running_{false};
    mutable std::mutex          cv_mutex_;
    std::condition_variable     cv_;

    // Receive thread
    std::thread                 rx_thread_;

    // GGA output thread (only started when gga_output.enabled)
    std::thread                 gga_thread_;

    // Latest GGA sentence (written by setGga, read by gga output loop)
    mutable std::mutex          gga_mutex_;
    std::string                 last_gga_;
    double                      last_gga_ts_ = 0.0;

    // Receive loop internals
    void rxLoop();
    void sendInitCommands();
    void processLine(const std::string& line, double ts);
    void updateVessel(const AisData& d, double ts);

    // GGA output loop
    void ggaOutputLoop();

    // Build $GPGGA sentence from the stored last_gga_ and send it
    bool sendGgaToDevice();

    static double epochNow();
};
