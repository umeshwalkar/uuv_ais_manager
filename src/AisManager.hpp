#pragma once
#include <memory>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include "Config.hpp"
#include "Transport.hpp"
#include "MqttClient.hpp"
#include "AisDevice.hpp"
#include "Logger.hpp"

/**
 * AisManager — owns the transport pool and coordinates AisDevice instances.
 *
 * Start-up sequence:
 *   1. Build transport pool: one ITransport per TransportDef in ais.transport[].
 *   2. For each enabled device, resolve its channel transports from the pool
 *      and construct an AisDevice with ResolvedTransport pairs.
 *   3. Start all devices (each manages its own rx / gga threads).
 *   4. Run publishLoop (merge + MQTT publish, respects publish_enabled per device).
 *
 * Enabling rules enforced at AisManager level:
 *   device.enabled=false      → AisDevice not created; absent from ais/status
 *   transport.enabled=false   → ResolvedTransport.enabled=false passed to device;
 *                               device idles, still appears in ais/status
 *   device.publish_enabled    → controls MQTT vessel publish only (not status)
 *
 * GPS GGA flow:
 *   External call to setGga() → broadcast to all devices whose gga_out is active.
 */
class AisManager {
public:
    explicit AisManager(const AppConfig& cfg);
    ~AisManager();

    void run();
    void stop();

    // Push latest $GPGGA to all devices with an enabled GGA output channel
    void setGga(const std::string& gga_sentence, double ts);

private:
    AppConfig cfg_;

    // Transport pool: id → ITransport (includes disabled transports so status can be reported)
    std::map<std::string, std::unique_ptr<ITransport>> pool_;

    std::vector<std::unique_ptr<AisDevice>> devices_;
    std::unique_ptr<MqttClient>             mqtt_;

    std::atomic<bool>       running_{false};
    std::thread             publish_thread_;
    mutable std::mutex      cv_mutex_;
    std::condition_variable cv_;

    // Build pool_ from cfg_.ais.transports
    void buildTransportPool();

    // Resolve a transport id → {ptr, enabled}
    ResolvedTransport resolveTransport(const std::string& transport_id) const;

    void publishLoop();
    void onMqttMessage(const std::string& topic, const std::string& payload);
    void publishAisVessels(int device_id);

    std::unordered_map<uint32_t, VesselRecord> mergeVessels(
        const std::vector<DeviceSnapshot>& snaps) const;

    std::string formatVesselsJson(
        const std::unordered_map<uint32_t, VesselRecord>& vessels,
        const std::vector<DeviceSnapshot>& snaps,
        double now_epoch) const;
    std::string formatStatusJson(
        const std::vector<DeviceSnapshot>& snaps,
        double now_epoch) const;

    static double epochNow();
};
