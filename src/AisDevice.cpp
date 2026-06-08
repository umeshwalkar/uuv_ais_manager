#include "AisDevice.hpp"
#include <iomanip>
#include <sstream>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>

// ── helpers ───────────────────────────────────────────────────────────────────

double AisDevice::epochNow() {
    return std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

// ── construction ──────────────────────────────────────────────────────────────

AisDevice::AisDevice(const AisDeviceConfig& cfg,
                     const ResolvedTransport& rx,
                     const ResolvedTransport& tx)
    : cfg_(cfg), rx_(rx), tx_(tx),
      mod_(cfg.name),
      last_data_time_(std::chrono::steady_clock::now()),
      last_data_warn_time_(std::chrono::steady_clock::now())
{
    state_.id                  = cfg_.id;
    state_.name                = cfg_.name;
    state_.publish_enabled     = cfg_.publish_enabled;
    state_.rx_transport_enabled = rx_.enabled;
    state_.tx_transport_enabled = tx_.enabled;
    state_.aivdm_ch_enabled    = cfg_.aivdm_in.enabled && rx_.enabled;
    state_.gga_ch_enabled      = cfg_.gga_out.enabled  && tx_.enabled;

    LOG_INF(mod_.c_str(),
            "Created  id=%d  rx_transport=%s[%s]  gga_out=%s  publish=%s",
            cfg_.id,
            cfg_.aivdm_in.transport.id.c_str(),
            rx_.enabled ? "enabled" : "DISABLED",
            cfg_.gga_out.enabled  ? "enabled" : "off",
            cfg_.publish_enabled  ? "yes" : "no");
}

AisDevice::~AisDevice() { stop(); }

// ── lifecycle ─────────────────────────────────────────────────────────────────

void AisDevice::start() {
    running_   = true;
    rx_thread_ = std::thread(&AisDevice::rxLoop, this);

    if (cfg_.gga_out.enabled && tx_.enabled && tx_.ptr) {
        gga_thread_ = std::thread(&AisDevice::ggaOutputLoop, this);
        LOG_INF(mod_.c_str(), "GGA output thread started (interval=%dms  transport=%s)",
                cfg_.gga_out.send_interval_ms,
                cfg_.gga_out.transport.id.c_str());
    } else if (cfg_.gga_out.enabled && !tx_.enabled) {
        LOG_WRN(mod_.c_str(), "GGA output channel enabled but transport '%s' is DISABLED",
                cfg_.gga_out.transport.id.c_str());
    }
}

void AisDevice::stop() {
    running_ = false;
    cv_.notify_all();
    if (rx_thread_.joinable())  rx_thread_.join();
    if (gga_thread_.joinable()) gga_thread_.join();
    if (rx_.ptr && rx_.ptr->isOpen()) rx_.ptr->close();
    if (tx_.ptr && tx_.ptr != rx_.ptr && tx_.ptr->isOpen()) tx_.ptr->close();
}

// ── thread-safe accessors ─────────────────────────────────────────────────────

DeviceSnapshot AisDevice::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    DeviceSnapshot snap = state_;
    snap.connected = rx_.ptr && rx_.ptr->isOpen();
    return snap;
}

void AisDevice::setGga(const std::string& gga_sentence, double ts) {
    std::lock_guard<std::mutex> lk(gga_mutex_);
    last_gga_    = gga_sentence;
    last_gga_ts_ = ts;
}

void AisDevice::setAivdmCallback(AivdmCallback cb) {
    aivdm_cb_ = std::move(cb);
}

// ── init commands ─────────────────────────────────────────────────────────────

void AisDevice::sendInitCommands() {
    if (cfg_.init_commands.empty() || !rx_.ptr) return;

    LOG_INF(mod_.c_str(), "Sending %zu init command(s)", cfg_.init_commands.size());
    for (const auto& cmd : cfg_.init_commands) {
        if (rx_.ptr->send(cmd + "\r\n")) {
            LOG_INF(mod_.c_str(), "Init cmd sent: %s", cmd.c_str());
        } else {
            LOG_ERR(mod_.c_str(), "Init cmd FAILED: %s", cmd.c_str());
        }
        std::unique_lock<std::mutex> lk(cv_mutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(150),
                     [this] { return !running_.load(); });
        if (!running_) break;
    }
}

// ── receive loop ──────────────────────────────────────────────────────────────

void AisDevice::rxLoop() {
    const char* mod = mod_.c_str();

    // Transport disabled — idle; status will report connected=false
    if (!rx_.enabled || !rx_.ptr) {
        LOG_WRN(mod, "RX transport '%s' DISABLED — idling (status will show disconnected)",
                cfg_.aivdm_in.transport.id.c_str());
        while (running_) {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, std::chrono::seconds(5),
                         [this] { return !running_.load(); });
        }
        LOG_INF(mod, "RX loop exited (transport was disabled)");
        return;
    }

    if (!cfg_.aivdm_in.enabled) {
        LOG_WRN(mod, "input_channels.aivdm DISABLED — transport will connect but lines discarded");
    }

    LOG_INF(mod, "RX loop started  transport='%s'  crc_check=%s  ch_debug=%s",
            cfg_.aivdm_in.transport.id.c_str(),
            cfg_.validate_checksum ? "on" : "off",
            cfg_.aivdm_in.debug   ? "on" : "off");

    const int reconnect_s = 3;
    const int timeout_ms  = 1000;
    bool init_sent = false;

    while (running_) {
        // ── Connect ───────────────────────────────────────────────────────────
        if (!rx_.ptr->isOpen()) {
            LOG_INF(mod, "Connecting to transport '%s'...",
                    cfg_.aivdm_in.transport.id.c_str());
            if (!rx_.ptr->open()) {
                LOG_ERR(mod, "Connect FAILED — retrying in %ds", reconnect_s);
                std::unique_lock<std::mutex> lk(cv_mutex_);
                cv_.wait_for(lk, std::chrono::seconds(reconnect_s),
                             [this] { return !running_.load(); });
                continue;
            }
            LOG_INF(mod, "Connected to transport '%s'",
                    cfg_.aivdm_in.transport.id.c_str());
            last_data_time_     = std::chrono::steady_clock::now();
            last_data_warn_time_= std::chrono::steady_clock::now();
            if (cfg_.send_init_on_reconnect) init_sent = false;
        }

        if (!init_sent) { sendInitCommands(); init_sent = true; }
        if (!running_) break;

        // ── Read line ─────────────────────────────────────────────────────────
        std::string line = rx_.ptr->readLine(timeout_ms);

        if (line.empty()) {
            if (!rx_.ptr->isOpen()) {
                LOG_WRN(mod, "Connection lost on transport '%s'",
                        cfg_.aivdm_in.transport.id.c_str());
            }

            // Data-timeout warning (rate-limited to once per data_timeout_sec)
            if (state_.packets_received > 0) {
                auto now = std::chrono::steady_clock::now();
                double age = std::chrono::duration<double>(now - last_data_time_).count();
                double warn_period = cfg_.aivdm_in.data_timeout_sec;
                if (age > warn_period &&
                    std::chrono::duration<double>(now - last_data_warn_time_).count() > warn_period) {
                    LOG_WRN(mod, "No data for %.1fs (timeout=%.1fs) — check transponder",
                            age, warn_period);
                    last_data_warn_time_ = now;
                }
            }
            continue;
        }

        // Only !AIVDM sentences are processed; everything else is discarded
        if (line.size() < 7 || line.compare(0, 7, "!AIVDM,") != 0) {
            if (cfg_.aivdm_in.debug)
                LOG_DBG(mod, "Ignoring non-AIVDM line: %.60s", line.c_str());
            continue;
        }

        if (cfg_.aivdm_in.debug)
            LOG_DBG(mod, "RX !AIVDM (%zu bytes): %s", line.size(), line.c_str());

        double ts = epochNow();
        last_data_time_ = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.packets_received++;
        }

        // ── CRC check ─────────────────────────────────────────────────────────
        if (cfg_.validate_checksum && !AisParser::validateChecksum(line)) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.crc_errors++;
            LOG_ERR(mod, "CRC error on packet #%llu: %s",
                    (unsigned long long)state_.packets_received, line.c_str());
            continue;
        }

        // Channel disabled: count but discard
        if (!cfg_.aivdm_in.enabled) continue;

        processLine(line, ts);
    }

    LOG_INF(mod, "RX loop stopped  packets=%llu  crc_errors=%llu",
            (unsigned long long)state_.packets_received,
            (unsigned long long)state_.crc_errors);
}

void AisDevice::processLine(const std::string& line, double ts) {
    auto result = parser_.parse(line, false, ts);
    if (!result) return;   // incomplete fragment — not an error

    const AisData& d = *result;

    LOG_INF(mod_.c_str(), "MMSI=%-9u type=%2d%s%s%s",
            d.mmsi, d.msg_type,
            (!std::isnan(d.lat) ? " pos=ok" : ""),
            (d.sog > 0.0 ? " sog=ok" : ""),
            (!d.vessel_name.empty() ? " name=ok" : ""));

    if (cfg_.aivdm_in.debug) {
        if (!std::isnan(d.lat)) {
            LOG_DBG(mod_.c_str(),
                    "  MMSI=%u lat=%.5f lon=%.5f sog=%.1fkn cog=%.1f hdg=%d nav=%d",
                    d.mmsi, d.lat, d.lon, d.sog, d.cog, d.heading, d.nav_status);
        }
        if (!d.vessel_name.empty()) {
            LOG_DBG(mod_.c_str(),
                    "  MMSI=%u name='%s' callsign='%s' type=%d imo=%u",
                    d.mmsi, d.vessel_name.c_str(), d.call_sign.c_str(),
                    d.ship_type, d.imo);
        }
    }

    updateVessel(d, ts);

    // Notify manager to publish immediately — complete !AIVDM message assembled
    if (aivdm_cb_) aivdm_cb_(cfg_.id);
}

void AisDevice::updateVessel(const AisData& d, double ts) {
    if (d.mmsi == 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    auto& rec = state_.vessels[d.mmsi];
    rec.source_device_id   = cfg_.id;
    rec.source_device_name = cfg_.name;
    rec.last_seen          = std::chrono::system_clock::now();

    switch (d.msg_type) {
        case 1: case 2: case 3: case 18:
            rec.data.mmsi = d.mmsi; rec.data.lat = d.lat; rec.data.lon = d.lon;
            rec.data.sog = d.sog; rec.data.cog = d.cog; rec.data.heading = d.heading;
            rec.data.nav_status = d.nav_status; rec.data.rot = d.rot;
            rec.data.time_stamp = d.time_stamp; rec.data.msg_type = d.msg_type;
            rec.data.recv_ts = ts;
            break;
        case 5:
            rec.data.mmsi = d.mmsi; rec.data.imo = d.imo;
            rec.data.call_sign = d.call_sign; rec.data.vessel_name = d.vessel_name;
            rec.data.ship_type = d.ship_type; rec.data.destination = d.destination;
            rec.data.eta = d.eta; rec.data.draught = d.draught;
            break;
        case 24:
            rec.data.mmsi = d.mmsi;
            if (!d.vessel_name.empty()) rec.data.vessel_name = d.vessel_name;
            if (!d.call_sign.empty())   rec.data.call_sign   = d.call_sign;
            if (d.ship_type)            rec.data.ship_type   = d.ship_type;
            break;
        default:
            rec.data = d;
            break;
    }
    state_.is_valid     = true;
    state_.last_recv_ts = ts;
}

// ── GGA output loop ───────────────────────────────────────────────────────────

void AisDevice::ggaOutputLoop() {
    const char* mod     = mod_.c_str();
    const auto  interval = std::chrono::milliseconds(cfg_.gga_out.send_interval_ms);

    LOG_INF(mod, "GGA output loop started  transport='%s'  interval=%dms  debug=%s",
            cfg_.gga_out.transport.id.c_str(),
            cfg_.gga_out.send_interval_ms,
            cfg_.gga_out.debug ? "on" : "off");

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, interval, [this] { return !running_.load(); });
        }
        if (!running_) break;

        if (!tx_.ptr || !tx_.ptr->isOpen()) {
            LOG_DBG(mod, "GGA output: TX transport not open — skipping");
            continue;
        }

        sendGgaToDevice();
    }

    LOG_INF(mod, "GGA output loop stopped  gga_sent=%llu",
            (unsigned long long)state_.gga_sent_count);
}

bool AisDevice::sendGgaToDevice() {
    const char* mod = mod_.c_str();
    std::string gga;
    double gga_ts;
    {
        std::lock_guard<std::mutex> lk(gga_mutex_);
        gga    = last_gga_;
        gga_ts = last_gga_ts_;
    }

    if (gga.empty()) {
        LOG_DBG(mod, "GGA output: no GGA data available yet");
        return false;
    }

    double age = epochNow() - gga_ts;
    if (age > cfg_.gga_out.data_timeout_sec) {
        LOG_WRN(mod, "GGA output: GPS data stale (%.1fs > %.1fs) — not sent",
                age, cfg_.gga_out.data_timeout_sec);
        return false;
    }

    if (!tx_.ptr->send(gga + "\r\n")) {
        LOG_ERR(mod, "GGA output: send FAILED on transport '%s'",
                cfg_.gga_out.transport.id.c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.gga_sent_count++;
    }

    if (cfg_.gga_out.debug) {
        LOG_DBG(mod, "GGA TX (%zu bytes): %s", gga.size(), gga.c_str());
    }
    return true;
}
