#include "AisManager.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ───────────────────────────────────────────────────────────────────

double AisManager::epochNow() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

static TransportConfig toTransportConfig(const TransportDef& d) {
    TransportConfig c;
    c.type = d.type; c.bind_host = d.bind_host; c.bind_port = d.bind_port;
    c.host = d.host; c.port = d.port;
    c.serial_port = d.serial_port; c.serial_baud = d.serial_baud;
    c.serial_data_bits = d.serial_data_bits; c.serial_stop_bits = d.serial_stop_bits;
    c.serial_parity = d.serial_parity;
    c.connect_timeout_sec = d.connect_timeout_sec;
    c.reconnect_delay_sec = d.reconnect_delay_sec;
    c.read_timeout_ms = d.read_timeout_ms;
    c.buffer_size_bytes = d.buffer_size_bytes;
    return c;
}

// Build one vessel JSON object
static json vesselJson(const VesselRecord& rec, double age, int dev_id,
                       const std::string& dev_name, bool publish_raw) {
    json v;
    v["mmsi"]        = rec.data.mmsi;
    v["device_id"]   = dev_id;
    v["device_name"] = dev_name;
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
    if (publish_raw)                   v["raw"]         = rec.data.raw;
    return v;
}

// ── construction ──────────────────────────────────────────────────────────────

AisManager::AisManager(const AppConfig& cfg) : cfg_(cfg) {
    buildTransportPool();

    for (const auto& dcfg : cfg_.ais.devices) {
        if (!dcfg.enabled) {
            std::cout << "[AisManager] Device '" << dcfg.name
                      << "' disabled — skipping\n";
            continue;
        }

        ResolvedTransport rx = resolveTransport(dcfg.aivdm_in.transport.shared_with);
        if (!dcfg.aivdm_in.enabled) rx.enabled = false;

        ResolvedTransport tx = resolveTransport(dcfg.gga_out.transport.shared_with);
        if (!dcfg.gga_out.enabled) tx.enabled = false;

        devices_.push_back(std::make_unique<AisDevice>(dcfg, rx, tx));
    }

    if (cfg_.mqtt.enabled)
        mqtt_ = std::make_unique<MqttClient>(cfg_.mqtt);
}

AisManager::~AisManager() { stop(); }

void AisManager::buildTransportPool() {
    std::cout << "[AisManager] Transport pool ("
              << cfg_.ais.transports.size() << " entries):\n";
    for (const auto& td : cfg_.ais.transports) {
        pool_[td.id] = makeTransport(toTransportConfig(td));
        std::cout << "  [" << td.id << "] type=" << td.type;
        if (td.type == "tcp_client")     std::cout << " " << td.host << ":" << td.port;
        else if (td.type == "serial")    std::cout << " " << td.serial_port << "@" << td.serial_baud;
        else                             std::cout << " " << td.bind_host << ":" << td.bind_port;
        std::cout << (td.enabled ? "" : "  [DISABLED]") << "\n";
    }
}

ResolvedTransport AisManager::resolveTransport(const std::string& id) const {
    if (id.empty()) return {nullptr, false};
    auto it = pool_.find(id);
    if (it == pool_.end()) {
        std::cerr << "[AisManager] WARNING: transport '" << id << "' not in pool\n";
        return {nullptr, false};
    }
    bool enabled = false;
    for (const auto& td : cfg_.ais.transports)
        if (td.id == id) { enabled = td.enabled; break; }
    return {it->second.get(), enabled};
}

// ── public interface ──────────────────────────────────────────────────────────

void AisManager::run() {
    if (running_) return;
    running_ = true;

    std::cout << "[AisManager] Starting — " << devices_.size()
              << " active device(s)\n";
    for (const auto& dcfg : cfg_.ais.devices) {
        std::string st = dcfg.enabled ? "enabled" : "DISABLED";
        std::cout << "  [" << dcfg.name << "] id=" << dcfg.id << " " << st;
        if (dcfg.enabled)
            std::cout << " publish=" << (dcfg.publish_enabled ? "yes" : "no")
                      << " aivdm=" << (dcfg.aivdm_in.enabled
                                       ? dcfg.aivdm_in.transport.shared_with : "off")
                      << " gga="   << (dcfg.gga_out.enabled
                                       ? dcfg.gga_out.transport.shared_with : "off");
        std::cout << "\n";
    }

    if (mqtt_ && !mqtt_->connect())
        std::cerr << "[AisManager] WARNING: MQTT connect failed\n";

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

void AisManager::setGga(const std::string& gga_sentence, double ts) {
    for (auto& dev : devices_) {
        for (const auto& dcfg : cfg_.ais.devices)
            if (dcfg.id == dev->id() && dcfg.enabled && dcfg.gga_out.enabled)
                dev->setGga(gga_sentence, ts);
    }
}

// ── publish loop ──────────────────────────────────────────────────────────────

void AisManager::publishLoop() {
    std::cout << "[AisManager::publishLoop] Started\n";

    // Per-device next-publish timestamps (each has its own publish_interval_ms)
    std::map<int, std::chrono::steady_clock::time_point> next_pub;
    auto now_steady = std::chrono::steady_clock::now();
    for (const auto& d : cfg_.ais.devices)
        if (d.enabled) next_pub[d.id] = now_steady;

    auto last_status = now_steady;
    const auto status_ivl = std::chrono::seconds(cfg_.ais.status_interval_sec);

    // Tick at 100 ms so we don't miss any device's interval
    const auto tick = std::chrono::milliseconds(100);

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, tick, [this] { return !running_.load(); });
        }
        if (!running_) break;

        now_steady        = std::chrono::steady_clock::now();
        double now_epoch  = epochNow();

        // Collect snapshots from all running devices
        std::vector<DeviceSnapshot> snaps;
        for (const auto& dev : devices_) snaps.push_back(dev->snapshot());

        // Per-device vessel publish
        for (const auto& snap : snaps) {
            if (!snap.publish_enabled) continue;   // transport runs, MQTT skipped

            auto it = next_pub.find(snap.id);
            if (it == next_pub.end() || now_steady < it->second) continue;

            // Advance this device's next publish time
            int pub_ms = 1000;
            for (const auto& d : cfg_.ais.devices)
                if (d.id == snap.id) { pub_ms = d.publish_interval_ms; break; }
            it->second = now_steady + std::chrono::milliseconds(pub_ms);

            if (!mqtt_) continue;

            // Purge threshold for this device
            double purge_after = 5.0 * 60.0;  // default 5min
            bool   publish_raw = false;
            for (const auto& d : cfg_.ais.devices)
                if (d.id == snap.id) {
                    purge_after = d.aivdm_in.data_timeout_sec * 60.0;
                    publish_raw = d.publish_raw_ais;
                    break;
                }

            json j;
            j["ts"]          = now_epoch;
            j["device_id"]   = snap.id;
            j["device_name"] = snap.name;

            json arr = json::array();
            for (const auto& [mmsi, rec] : snap.vessels) {
                double age = now_epoch - rec.data.recv_ts;
                if (age > purge_after) continue;
                arr.push_back(vesselJson(rec, age, snap.id, snap.name, publish_raw));
            }
            j["vessels"]      = arr;
            j["vessel_count"] = arr.size();
            mqtt_->publish(cfg_.mqtt.topics.ais, j.dump());
        }

        // Status publish (all devices — including disabled ones)
        if (now_steady - last_status >= status_ivl) {
            if (mqtt_)
                mqtt_->publish(cfg_.mqtt.topics.status, formatStatusJson(snaps, now_epoch));
            last_status = now_steady;
        }
    }

    std::cout << "[AisManager::publishLoop] Stopped\n";
}

// ── JSON formatters ───────────────────────────────────────────────────────────

std::unordered_map<uint32_t, VesselRecord>
AisManager::mergeVessels(const std::vector<DeviceSnapshot>& snaps) const {
    std::unordered_map<uint32_t, VesselRecord> merged;
    for (const auto& snap : snaps) {
        if (!snap.publish_enabled) continue;
        for (const auto& [mmsi, rec] : snap.vessels) {
            auto it = merged.find(mmsi);
            if (it == merged.end() || rec.data.recv_ts > it->second.data.recv_ts)
                merged[mmsi] = rec;
        }
    }
    return merged;
}

std::string AisManager::formatVesselsJson(
    const std::unordered_map<uint32_t, VesselRecord>& vessels,
    const std::vector<DeviceSnapshot>& /*snaps*/,
    double now_epoch) const
{
    json j; j["ts"] = now_epoch; j["vessels"] = json::array();
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

    // Active devices (have a running AisDevice)
    for (const auto& snap : snaps) {
        double age   = snap.is_valid ? (now_epoch - snap.last_recv_ts) : -1.0;
        bool   fresh = false;
        for (const auto& d : cfg_.ais.devices)
            if (d.id == snap.id) {
                fresh = snap.is_valid && age >= 0.0
                        && age < d.aivdm_in.data_timeout_sec;
                break;
            }

        std::string health;
        if (!snap.rx_transport_enabled)  health = "transport_disabled";
        else if (!snap.connected)        health = "disconnected";
        else if (!snap.aivdm_ch_enabled) health = "channel_disabled";
        else if (!snap.is_valid)         health = "no_data";
        else if (!fresh)                 health = "stale";
        else                             health = "ok";

        json d;
        d["id"]                   = snap.id;
        d["name"]                 = snap.name;
        d["publish_enabled"]      = snap.publish_enabled;
        d["rx_transport_enabled"] = snap.rx_transport_enabled;
        d["tx_transport_enabled"] = snap.tx_transport_enabled;
        d["aivdm_ch_enabled"]     = snap.aivdm_ch_enabled;
        d["gga_ch_enabled"]       = snap.gga_ch_enabled;
        d["connected"]            = snap.connected;
        d["data_valid"]           = snap.is_valid;
        d["last_data_ts"]         = snap.last_recv_ts;
        d["data_age_sec"]         = age;
        d["packets_received"]     = snap.packets_received;
        d["crc_errors"]           = snap.crc_errors;
        d["vessel_count"]         = (int)snap.vessels.size();
        d["gga_sent_count"]       = snap.gga_sent_count;
        d["health"]               = health;
        devs.push_back(d);

        if (snap.connected) any_connected = true;
        if (snap.is_valid)  any_data      = true;
    }

    // Disabled devices still appear in status
    for (const auto& dcfg : cfg_.ais.devices) {
        if (!dcfg.enabled) {
            json d;
            d["id"] = dcfg.id; d["name"] = dcfg.name;
            d["health"] = "device_disabled";
            devs.push_back(d);
        }
    }

    j["devices"] = devs;

    std::string overall;
    if (!any_connected) overall = "disconnected";
    else if (!any_data) overall = "no_data";
    else {
        bool all_ok = true;
        for (auto& d : devs)
            if (d.value("health", "") != "ok") { all_ok = false; break; }
        overall = all_ok ? "ok" : "degraded";
    }
    j["health"] = overall;
    return j.dump();
}
