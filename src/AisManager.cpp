#include "AisManager.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ───────────────────────────────────────────────────────────────────

double AisManager::epochNow() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ── construction ──────────────────────────────────────────────────────────────

AisManager::AisManager(const AppConfig& cfg) : cfg_(cfg) {
    for (const auto& dcfg : cfg_.ais.devices) {
        if (dcfg.enabled)
            devices_.push_back(std::make_unique<AisDevice>(dcfg, cfg_.ais.validate_crc));
    }
    if (cfg_.mqtt.enabled)
        mqtt_ = std::make_unique<MqttClient>(cfg_.mqtt);
}

AisManager::~AisManager() { stop(); }

// ── public interface ──────────────────────────────────────────────────────────

void AisManager::run() {
    if (running_) return;
    running_ = true;

    std::cout << "[AisManager] Starting with " << devices_.size() << " device(s)\n";
    for (const auto& dev : devices_) {
        const auto& dc = cfg_.ais.devices[dev->id() - 1];
        const auto& tr = dc.transport;
        std::cout << "  [" << dev->name() << "] id=" << dev->id()
                  << " transport=" << tr.type;
        if (tr.type == "tcp_client")      std::cout << " " << tr.host << ":" << tr.port;
        else if (tr.type == "serial")     std::cout << " " << tr.serial_port << "@" << tr.serial_baud;
        else                              std::cout << " " << tr.bind_host << ":" << tr.bind_port;
        std::cout << "  gga_out=" << (dc.gga_output.enabled ? "enabled" : "disabled") << "\n";
    }
    std::cout << "  Publish interval: " << cfg_.ais.publish_interval_ms << " ms\n"
              << "  AIS topic:        " << cfg_.mqtt.topics.ais    << "\n"
              << "  Status topic:     " << cfg_.mqtt.topics.status << "\n";

    if (mqtt_) {
        if (!mqtt_->connect())
            std::cerr << "[AisManager] WARNING: MQTT connect failed — will retry on publish\n";
    }

    for (auto& dev : devices_) dev->start();

    publish_thread_ = std::thread(&AisManager::publishLoop, this);
    if (publish_thread_.joinable()) publish_thread_.join();

    std::cout << "[AisManager] Stopped\n";
}

void AisManager::stop() {
    running_ = false;
    cv_.notify_all();
    for (auto& dev : devices_) dev->stop();
}

// ── GPS GGA broadcast ─────────────────────────────────────────────────────────

void AisManager::setGga(const std::string& gga_sentence, double ts) {
    for (auto& dev : devices_) {
        if (cfg_.ais.devices[dev->id() - 1].gga_output.enabled)
            dev->setGga(gga_sentence, ts);
    }
}

// ── vessel merge ──────────────────────────────────────────────────────────────

std::unordered_map<uint32_t, VesselRecord>
AisManager::mergeVessels(const std::vector<DeviceSnapshot>& snaps) const {
    std::unordered_map<uint32_t, VesselRecord> merged;
    for (const auto& snap : snaps) {
        for (const auto& [mmsi, rec] : snap.vessels) {
            auto it = merged.find(mmsi);
            if (it == merged.end() || rec.data.recv_ts > it->second.data.recv_ts)
                merged[mmsi] = rec;
        }
    }
    return merged;
}

// ── publish loop ──────────────────────────────────────────────────────────────

void AisManager::publishLoop() {
    std::cout << "[AisManager::publishLoop] Started\n";

    const auto pub_interval    = std::chrono::milliseconds(cfg_.ais.publish_interval_ms);
    const auto status_interval = std::chrono::seconds(cfg_.ais.status_interval_sec);
    auto last_pub    = std::chrono::steady_clock::now();
    auto last_status = std::chrono::steady_clock::now();

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, pub_interval, [this] { return !running_.load(); });
        }
        if (!running_) break;

        auto   now    = std::chrono::steady_clock::now();
        double now_ep = epochNow();

        std::vector<DeviceSnapshot> snaps;
        for (const auto& dev : devices_) snaps.push_back(dev->snapshot());

        if (now - last_pub >= pub_interval) {
            auto vessels = mergeVessels(snaps);
            if (!vessels.empty() && mqtt_)
                mqtt_->publish(cfg_.mqtt.topics.ais, formatVesselsJson(vessels, now_ep));
            last_pub = now;
        }

        if (now - last_status >= status_interval) {
            if (mqtt_)
                mqtt_->publish(cfg_.mqtt.topics.status, formatStatusJson(snaps, now_ep));
            last_status = now;
        }
    }

    std::cout << "[AisManager::publishLoop] Stopped\n";
}

// ── JSON formatters ───────────────────────────────────────────────────────────

std::string AisManager::formatVesselsJson(
    const std::unordered_map<uint32_t, VesselRecord>& vessels,
    double now_epoch) const
{
    const double purge_after = cfg_.ais.data_timeout_sec * 60.0;

    json j;
    j["ts"] = now_epoch;
    json arr = json::array();

    for (auto& [mmsi, rec] : vessels) {
        double age = now_epoch - rec.data.recv_ts;
        if (age > purge_after) continue;

        json v;
        v["mmsi"]        = rec.data.mmsi;
        v["device_id"]   = rec.source_device_id;
        v["device_name"] = rec.source_device_name;
        v["msg_type"]    = rec.data.msg_type;
        v["recv_ts"]     = rec.data.recv_ts;
        v["age_sec"]     = age;

        if (!std::isnan(rec.data.lat)) { v["lat"] = rec.data.lat; v["lon"] = rec.data.lon; }
        if (rec.data.sog > 0.0)        v["sog"]         = rec.data.sog;
        if (rec.data.cog > 0.0)        v["cog"]         = rec.data.cog;
        if (rec.data.heading != 511)   v["heading"]     = rec.data.heading;
        if (rec.data.nav_status != 15) v["nav_status"]  = rec.data.nav_status;
        if (!rec.data.vessel_name.empty()) v["vessel_name"] = rec.data.vessel_name;
        if (!rec.data.call_sign.empty())   v["call_sign"]   = rec.data.call_sign;
        if (rec.data.ship_type)            v["ship_type"]   = rec.data.ship_type;
        if (rec.data.imo)                  v["imo"]         = rec.data.imo;
        if (!rec.data.destination.empty()) v["destination"] = rec.data.destination;
        if (cfg_.ais.publish_raw_ais)      v["raw"]         = rec.data.raw;
        arr.push_back(v);
    }
    j["vessels"]      = arr;
    j["vessel_count"] = arr.size();
    return j.dump();
}

std::string AisManager::formatStatusJson(
    const std::vector<DeviceSnapshot>& snaps,
    double now_epoch) const
{
    json j;
    j["ts"] = now_epoch;

    json devs = json::array();
    bool any_connected = false, any_data = false;

    for (const auto& snap : snaps) {
        double age   = snap.is_valid ? (now_epoch - snap.last_recv_ts) : -1.0;
        bool   fresh = snap.is_valid && age >= 0.0 && age < cfg_.ais.data_timeout_sec;

        std::string health;
        if (!snap.connected)  health = "disconnected";
        else if (!snap.is_valid)  health = "no_data";
        else if (!fresh)          health = "stale";
        else                      health = "ok";

        json d;
        d["id"]                = snap.id;
        d["name"]              = snap.name;
        d["connected"]         = snap.connected;
        d["data_valid"]        = snap.is_valid;
        d["last_data_ts"]      = snap.last_recv_ts;
        d["data_age_sec"]      = age;
        d["packets_received"]  = snap.packets_received;
        d["parse_errors"]      = snap.parse_errors;
        d["crc_errors"]        = snap.crc_errors;
        d["vessel_count"]      = (int)snap.vessels.size();
        d["gga_output_enabled"] = snap.gga_output_enabled;
        d["gga_sent_count"]    = snap.gga_sent_count;
        d["health"]            = health;
        devs.push_back(d);

        if (snap.connected) any_connected = true;
        if (snap.is_valid)  any_data      = true;
    }

    j["devices"] = devs;

    std::string overall;
    if (!any_connected)  overall = "disconnected";
    else if (!any_data)  overall = "no_data";
    else {
        bool all_ok = true;
        for (auto& d : devs)
            if (d["health"] != "ok") { all_ok = false; break; }
        overall = all_ok ? "ok" : "degraded";
    }
    j["health"] = overall;
    return j.dump();
}
