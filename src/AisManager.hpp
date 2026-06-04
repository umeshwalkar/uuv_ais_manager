#pragma once
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include "Config.hpp"
#include "MqttClient.hpp"
#include "AisDevice.hpp"

/**
 * AisManager — coordinates one or more AisDevice instances (AIS1, AIS2 …).
 *
 * Each AisDevice runs its own receive thread and, if enabled, its own GGA
 * output thread.  AisManager's publish thread merges vessel tables across all
 * devices and publishes to MQTT with device_id/device_name tags per vessel.
 *
 * GPS data flow for GGA output:
 *   MQTT gnss_gga topic → AisManager::setGga() → AisDevice::setGga() (all)
 *   → each device's ggaOutputLoop() sends $GPGGA to the transponder.
 */
class AisManager {
public:
    explicit AisManager(const AppConfig& cfg);
    ~AisManager();

    void run();    // blocks until stop()
    void stop();   // safe to call from signal handler

    // Push latest GPS GGA sentence to all devices that have GGA output enabled.
    // Call this whenever fresh GNSS data arrives (MQTT subscription callback etc.)
    void setGga(const std::string& gga_sentence, double ts);

private:
    AppConfig                               cfg_;
    std::vector<std::unique_ptr<AisDevice>> devices_;
    std::unique_ptr<MqttClient>             mqtt_;

    std::atomic<bool>           running_{false};
    std::thread                 publish_thread_;

    mutable std::mutex          cv_mutex_;
    std::condition_variable     cv_;

    void publishLoop();

    std::unordered_map<uint32_t, VesselRecord> mergeVessels(
        const std::vector<DeviceSnapshot>& snaps) const;

    std::string formatVesselsJson(
        const std::unordered_map<uint32_t, VesselRecord>& vessels,
        double now_epoch) const;
    std::string formatStatusJson(
        const std::vector<DeviceSnapshot>& snaps,
        double now_epoch) const;

    static double epochNow();
};
