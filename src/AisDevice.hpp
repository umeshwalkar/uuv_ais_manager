#pragma once
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include <functional>
#include "Config.hpp"
#include "Transport.hpp"
#include "AisParser.hpp"
#include "Logger.hpp"

// ── Per-vessel record ─────────────────────────────────────────────────────────

struct VesselRecord {
    AisData     data;
    int         source_device_id   = 0;
    std::string source_device_name;
    std::chrono::system_clock::time_point last_seen;
};

// ── Resolved transport (pointer from AisManager's pool + enabled flag) ────────

struct ResolvedTransport {
    ITransport* ptr     = nullptr;   // owned by AisManager — do NOT delete
    bool        enabled = false;     // = TransportDef.enabled && channel.enabled
};

// ── Thread-safe snapshot ──────────────────────────────────────────────────────

struct DeviceSnapshot {
    int         id                = 0;
    std::string name;
    bool        enabled           = true;    // always true for created devices
    bool        publish_enabled   = true;
    bool        rx_transport_enabled = false;
    bool        tx_transport_enabled = false;
    bool        aivdm_ch_enabled  = false;
    bool        gga_ch_enabled    = false;
    bool        connected         = false;
    bool        is_valid          = false;
    double      last_recv_ts      = 0.0;
    uint64_t    packets_received  = 0;
    uint64_t    parse_errors      = 0;
    uint64_t    crc_errors        = 0;
    uint64_t    gga_sent_count    = 0;
    std::unordered_map<uint32_t, VesselRecord> vessels;
};

// ── AisDevice ─────────────────────────────────────────────────────────────────
/**
 * Manages one physical AIS sensor.
 *
 * Constructed by AisManager which resolves transport pool entries and passes
 * raw ResolvedTransport pointers.  AisDevice does NOT own the transport
 * objects; AisManager does.
 *
 * Threads started by start():
 *   rx_thread_  — connects rx transport, reads/parses !AIVDM/!AIVDO
 *   gga_thread_ — (if gga_out.enabled && tx resolved) sends $GPGGA periodically
 *
 * Enabling/disabling logic:
 *   rx_.enabled == false  → rx_thread_ idles, no connect attempt, connected=false
 *   aivdm_ch disabled     → rx transport connects (may be needed for tx), lines discarded
 *   tx_.enabled == false  → gga_thread_ not started at all
 *   gga_ch disabled       → gga_thread_ not started at all
 */
class AisDevice {
public:
    AisDevice(const AisDeviceConfig& cfg,
              const ResolvedTransport& rx,
              const ResolvedTransport& tx);
    ~AisDevice();

    void start();
    void stop();

    // Thread-safe snapshot for publish/status
    DeviceSnapshot snapshot() const;

    // Push latest GPS GGA sentence; used by the GGA output thread
    void setGga(const std::string& gga_sentence, double ts);

    // Called once per complete !AIVDM message; fires from the rx thread
    using AivdmCallback = std::function<void(int device_id)>;
    void setAivdmCallback(AivdmCallback cb);

    int                id()             const { return cfg_.id;             }
    const std::string& name()           const { return cfg_.name;           }
    bool               publishEnabled() const { return cfg_.publish_enabled;}

private:
    AisDeviceConfig     cfg_;
    ResolvedTransport   rx_;    // aivdm input transport
    ResolvedTransport   tx_;    // gga output transport (may == rx_)
    AisParser           parser_;

    mutable std::mutex  mtx_;
    DeviceSnapshot      state_;

    std::atomic<bool>   running_{false};
    mutable std::mutex  cv_mutex_;
    std::condition_variable cv_;

    std::thread         rx_thread_;
    std::thread         gga_thread_;

    mutable std::mutex  gga_mutex_;
    std::string         last_gga_;
    double              last_gga_ts_ = 0.0;

    AivdmCallback       aivdm_cb_;

    // Logger module tag (device name, ≤14 chars)
    std::string         mod_;

    // Rate-limited data-timeout warning
    std::chrono::steady_clock::time_point last_data_time_;
    std::chrono::steady_clock::time_point last_data_warn_time_;

    void rxLoop();
    void ggaOutputLoop();
    void sendInitCommands();
    void processLine(const std::string& line, double ts);
    void updateVessel(const AisData& d, double ts);
    bool sendGgaToDevice();

    static double epochNow();
};
