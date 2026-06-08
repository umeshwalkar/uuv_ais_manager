#include "AisManager.hpp"
#include <chrono>
#include <cmath>
#include <map>
#include <stdexcept>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#define MOD "AisManager"

// ── helpers ───────────────────────────────────────────────────────────────────

double AisManager::epochNow() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}


// Build one vessel JSON object
static json vesselJson(const VesselRecord& rec, double age,
                       int dev_id, const std::string& dev_name, bool publish_raw) {
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
            LOG_INF(MOD, "Device '%s' (id=%d) disabled — skipping", dcfg.name.c_str(), dcfg.id);
            continue;
        }

        ResolvedTransport rx = resolveTransport(dcfg.aivdm_in.transport.id);
        if (!dcfg.aivdm_in.enabled) {
            LOG_INF(MOD, "Device '%s': aivdm input channel disabled", dcfg.name.c_str());
            rx.enabled = false;
        }

        ResolvedTransport tx = resolveTransport(dcfg.gga_out.transport.id);
        if (!dcfg.gga_out.enabled) tx.enabled = false;

        auto& dev = devices_.emplace_back(std::make_unique<AisDevice>(dcfg, rx, tx));
        dev->setAivdmCallback([this](int id) { publishAisVessels(id); });
    }

    if (cfg_.mqtt.enabled)
        mqtt_ = std::make_unique<MqttClient>(cfg_.mqtt);
}

AisManager::~AisManager() { stop(); }

void AisManager::buildTransportPool() {
    LOG_INF(MOD, "Transport pool — %zu entries", cfg_.ais.transports.size());
    for (const auto& td : cfg_.ais.transports) {
        try {
            pool_[td.id] = makeTransport(td);
            if (td.enabled) {
                LOG_INF(MOD, "  [%s] type=%s %s%s",
                        td.id.c_str(), td.type.c_str(),
                        td.type == "tcp_client"  ? (td.host + ":" + std::to_string(td.port)).c_str() :
                        td.type == "serial"      ? td.serial_port.c_str() :
                        (td.bind_host + ":" + std::to_string(td.bind_port)).c_str(), "");
            } else {
                LOG_WRN(MOD, "  [%s] type=%s  DISABLED", td.id.c_str(), td.type.c_str());
            }
        } catch (const std::exception& e) {
            LOG_ERR(MOD, "Transport '%s' creation failed: %s", td.id.c_str(), e.what());
            throw;
        }
    }
}

ResolvedTransport AisManager::resolveTransport(const std::string& id) const {
    if (id.empty()) return {nullptr, false};
    auto it = pool_.find(id);
    if (it == pool_.end()) {
        LOG_WRN(MOD, "Transport '%s' not found in pool — channel will be disabled", id.c_str());
        return {nullptr, false};
    }
    bool enabled = false;
    for (const auto& td : cfg_.ais.transports)
        if (td.id == id) { enabled = td.enabled; break; }
    if (!enabled)
        LOG_WRN(MOD, "Transport '%s' is disabled — dependent channel will be disabled", id.c_str());
    return {it->second.get(), enabled};
}

// ── public interface ──────────────────────────────────────────────────────────

void AisManager::run() {
    if (running_) return;
    running_ = true;

    LOG_INF(MOD, "Starting — %zu active device(s)  status_interval=%ds",
            devices_.size(), cfg_.ais.status_interval_sec);

    for (const auto& dcfg : cfg_.ais.devices) {
        if (!dcfg.enabled) {
            LOG_INF(MOD, "  [%s] id=%d  DISABLED", dcfg.name.c_str(), dcfg.id);
            continue;
        }
        LOG_INF(MOD, "  [%s] id=%d  publish=%s(%dms)  aivdm='%s'  gga='%s'",
                dcfg.name.c_str(), dcfg.id,
                dcfg.publish_enabled ? "yes" : "no",
                dcfg.publish_interval_ms,
                dcfg.aivdm_in.enabled ? dcfg.aivdm_in.transport.id.c_str() : "off",
                dcfg.gga_out.enabled  ? dcfg.gga_out.transport.id.c_str()  : "off");
    }

    if (mqtt_) {
        if (!mqtt_->connect()) {
            LOG_WRN(MOD, "MQTT connect failed to %s:%d — will retry on publish",
                    cfg_.mqtt.broker.c_str(), cfg_.mqtt.port);
        } else {
            LOG_INF(MOD, "MQTT connected to %s:%d  client='%s'",
                    cfg_.mqtt.broker.c_str(), cfg_.mqtt.port,
                    cfg_.mqtt.client_id.c_str());
        }
        // Subscribe to GGA input topic and wire callback → setGga()
        const auto* sub_gga = cfg_.mqtt.topics.findSub("gnss_gga");
        if (sub_gga && !sub_gga->topic.empty()) {
            mqtt_->setMessageCallback([this](const std::string& t, const std::string& p) {
                onMqttMessage(t, p);
            });
            mqtt_->subscribe(sub_gga->topic, cfg_.mqtt.qos);
        }
    } else {
        LOG_WRN(MOD, "MQTT disabled — no vessel data will be published");
    }

    for (auto& dev : devices_) dev->start();

    LOG_INF(MOD, "publishLoop starting");
    publish_thread_ = std::thread(&AisManager::publishLoop, this);
    if (publish_thread_.joinable()) publish_thread_.join();

    LOG_INF(MOD, "All threads stopped");
}

void AisManager::stop() {
    running_ = false;
    cv_.notify_all();
    for (auto& dev : devices_) dev->stop();
    LOG_INF(MOD, "Stop requested");
}

void AisManager::setGga(const std::string& gga_sentence, double ts) {
    int fed = 0;
    for (auto& dev : devices_) {
        for (const auto& dcfg : cfg_.ais.devices)
            if (dcfg.id == dev->id() && dcfg.enabled && dcfg.gga_out.enabled) {
                dev->setGga(gga_sentence, ts);
                ++fed;
            }
    }
    if (fed == 0) {
        LOG_DBG(MOD, "setGga: no devices with gga_out enabled — GPS data dropped");
    }
}

void AisManager::onMqttMessage(const std::string& topic, const std::string& payload) {
    const auto* sub_gga = cfg_.mqtt.topics.findSub("gnss_gga");
    if (sub_gga && topic == sub_gga->topic) {
        if (sub_gga->debug)
            LOG_DBG(MOD, "MQTT RX [%s]  %zu bytes", topic.c_str(), payload.size());
        setGga(payload, epochNow());
    }
}

void AisManager::publishAisVessels(int device_id) {
    if (!mqtt_) return;
    const auto* pub = cfg_.mqtt.topics.findPub("ais");
    if (!pub || pub->publish_interval_ms == 0 || pub->topic.empty()) return;

    DeviceSnapshot snap;
    bool found = false;
    for (const auto& dev : devices_)
        if (dev->id() == device_id) { snap = dev->snapshot(); found = true; break; }
    if (!found || !snap.publish_enabled) return;

    double purge_after = 300.0;
    bool   publish_raw = false;
    for (const auto& d : cfg_.ais.devices)
        if (d.id == device_id) {
            purge_after = d.aivdm_in.data_timeout_sec * 60.0;
            publish_raw = d.publish_raw_ais;
            break;
        }

    double now_epoch = epochNow();
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
    std::string payload = j.dump();

    bool ok = mqtt_->publish(pub->topic, payload);
    if (ok) {
        if (pub->debug)
            LOG_DBG(MOD, "MQTT TX [%s] dev='%s' vessels=%d  %zu bytes",
                    pub->topic.c_str(), snap.name.c_str(), (int)arr.size(), payload.size());
    } else {
        LOG_ERR(MOD, "MQTT publish FAILED on topic '%s' for device '%s'",
                pub->topic.c_str(), snap.name.c_str());
    }
}

// ── vessel merge ──────────────────────────────────────────────────────────────

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

// ── publish loop ──────────────────────────────────────────────────────────────

void AisManager::publishLoop() {
    LOG_INF(MOD, "publishLoop started");

    auto now_steady  = std::chrono::steady_clock::now();
    auto last_status = now_steady;

    const auto* pub_status = cfg_.mqtt.topics.findPub("status");
    const std::string status_topic = pub_status ? pub_status->topic : "";
    const bool        status_dbg   = pub_status ? pub_status->debug : false;
    const int  status_interval_ms  = pub_status ? pub_status->publish_interval_ms : 0;

    const auto status_ivl = status_interval_ms > 0
        ? std::chrono::milliseconds(status_interval_ms)
        : std::chrono::seconds(cfg_.ais.status_interval_sec);
    const auto tick = std::chrono::milliseconds(100);

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, tick, [this] { return !running_.load(); });
        }
        if (!running_) break;

        now_steady       = std::chrono::steady_clock::now();
        double now_epoch = epochNow();

        std::vector<DeviceSnapshot> snaps;
        for (const auto& dev : devices_) snaps.push_back(dev->snapshot());

        // ── Status publish ────────────────────────────────────────────────────
        if (status_interval_ms > 0 && now_steady - last_status >= status_ivl) {
            if (mqtt_ && !status_topic.empty()) {
                std::string status = formatStatusJson(snaps, now_epoch);
                bool ok = mqtt_->publish(status_topic, status);
                if (ok) {
                    if (status_dbg)
                        LOG_DBG(MOD, "MQTT TX [%s]  %zu bytes",
                                status_topic.c_str(), status.size());
                } else {
                    LOG_ERR(MOD, "MQTT publish FAILED on topic '%s'",
                            status_topic.c_str());
                }
            }
            last_status = now_steady;

            // Periodic console status summary (INFO level)
            for (const auto& snap : snaps) {
                LOG_INF(MOD, "Status  dev='%s'  connected=%s  vessels=%d  "
                        "pkts=%llu  crc_err=%llu  health=%s",
                        snap.name.c_str(),
                        snap.connected ? "yes" : "no",
                        (int)snap.vessels.size(),
                        (unsigned long long)snap.packets_received,
                        (unsigned long long)snap.crc_errors,
                        snap.connected && snap.is_valid ? "ok" :
                        !snap.connected                 ? "disconnected" : "no_data");
            }
        }
    }

    LOG_INF(MOD, "publishLoop stopped");
}

// ── JSON formatters ───────────────────────────────────────────────────────────

std::string AisManager::formatVesselsJson(
    const std::unordered_map<uint32_t, VesselRecord>&,
    const std::vector<DeviceSnapshot>&, double) const { return "{}"; }

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
        bool   fresh = false;
        for (const auto& d : cfg_.ais.devices)
            if (d.id == snap.id) {
                fresh = snap.is_valid && age >= 0.0 && age < d.aivdm_in.data_timeout_sec;
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

    // Disabled devices appear in status with minimal info
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
