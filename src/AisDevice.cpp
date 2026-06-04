#include "AisDevice.hpp"
#include <iostream>
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
    : cfg_(cfg), rx_(rx), tx_(tx)
{
    state_.id                  = cfg_.id;
    state_.name                = cfg_.name;
    state_.publish_enabled     = cfg_.publish_enabled;
    state_.rx_transport_enabled = rx_.enabled;
    state_.tx_transport_enabled = tx_.enabled;
    state_.aivdm_ch_enabled    = cfg_.aivdm_in.enabled && rx_.enabled;
    state_.gga_ch_enabled      = cfg_.gga_out.enabled  && tx_.enabled;
}

AisDevice::~AisDevice() { stop(); }

// ── lifecycle ─────────────────────────────────────────────────────────────────

void AisDevice::start() {
    running_   = true;
    rx_thread_ = std::thread(&AisDevice::rxLoop, this);

    // Start GGA output only if both the channel and transport are enabled
    if (cfg_.gga_out.enabled && tx_.enabled && tx_.ptr)
        gga_thread_ = std::thread(&AisDevice::ggaOutputLoop, this);
}

void AisDevice::stop() {
    running_ = false;
    cv_.notify_all();
    if (rx_thread_.joinable())  rx_thread_.join();
    if (gga_thread_.joinable()) gga_thread_.join();
    // Close rx transport (tx may be the same ptr — only close once)
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

// ── init commands ─────────────────────────────────────────────────────────────

void AisDevice::sendInitCommands() {
    if (cfg_.init_commands.empty() || !rx_.ptr) return;
    std::cout << "[" << cfg_.name << "] Sending " << cfg_.init_commands.size()
              << " init command(s)\n";
    for (const auto& cmd : cfg_.init_commands) {
        if (rx_.ptr->send(cmd + "\r\n"))
            std::cout << "  [" << cfg_.name << " cmd] " << cmd << "  OK\n";
        else
            std::cerr << "  [" << cfg_.name << " cmd] " << cmd << "  FAILED\n";
        std::unique_lock<std::mutex> lk(cv_mutex_);
        cv_.wait_for(lk, std::chrono::milliseconds(150),
                     [this] { return !running_.load(); });
        if (!running_) break;
    }
}

// ── receive loop ──────────────────────────────────────────────────────────────

void AisDevice::rxLoop() {
    std::cout << "[" << cfg_.name << "] RX loop started";

    // Transport disabled — idle and report disconnected in status
    if (!rx_.enabled || !rx_.ptr) {
        std::cout << " (transport DISABLED — idling)\n";
        while (running_) {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, std::chrono::seconds(1),
                         [this] { return !running_.load(); });
        }
        std::cout << "[" << cfg_.name << "] RX loop stopped (transport disabled)\n";
        return;
    }

    std::cout << " (transport=" << cfg_.aivdm_in.transport.shared_with << ")\n";

    const int reconnect_s = rx_.ptr ? 3 : 0;
    const int timeout_ms  = 1000;
    bool init_sent = false;

    while (running_) {
        if (!rx_.ptr->isOpen()) {
            std::cout << "[" << cfg_.name << "] Connecting...\n";
            if (!rx_.ptr->open()) {
                std::cerr << "[" << cfg_.name
                          << "] Connect failed — retrying in " << reconnect_s << "s\n";
                std::unique_lock<std::mutex> lk(cv_mutex_);
                cv_.wait_for(lk, std::chrono::seconds(reconnect_s),
                             [this] { return !running_.load(); });
                continue;
            }
            std::cout << "[" << cfg_.name << "] Connected\n";
            if (cfg_.send_init_on_reconnect) init_sent = false;
        }

        if (!init_sent) { sendInitCommands(); init_sent = true; }
        if (!running_) break;

        std::string line = rx_.ptr->readLine(timeout_ms);
        if (line.empty()) {
            if (!rx_.ptr->isOpen())
                std::cerr << "[" << cfg_.name << "] Connection lost\n";
            continue;
        }

        if (line[0] != '!') continue;  // ignore non-AIS lines

        double ts = epochNow();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.packets_received++;
        }

        // CRC validation
        if (cfg_.validate_checksum && !AisParser::validateChecksum(line)) {
            std::lock_guard<std::mutex> lk(mtx_);
            state_.crc_errors++;
            std::cerr << "[" << cfg_.name << "] CRC error: " << line << "\n";
            continue;
        }

        // If aivdm input channel is disabled: receive + count but don't parse
        if (!cfg_.aivdm_in.enabled) continue;

        processLine(line, ts);
    }

    std::cout << "[" << cfg_.name << "] RX loop stopped\n";
}

void AisDevice::processLine(const std::string& line, double ts) {
    auto result = parser_.parse(line, false, ts);
    if (!result) return;

    const AisData& d = *result;
    std::cout << "[" << cfg_.name << "] MMSI=" << d.mmsi
              << " type=" << d.msg_type;
    if (!std::isnan(d.lat))
        std::cout << std::fixed << std::setprecision(5)
                  << " lat=" << d.lat << " lon=" << d.lon;
    if (d.sog > 0.0) std::cout << " sog=" << d.sog << "kn";
    if (!d.vessel_name.empty()) std::cout << " name=" << d.vessel_name;
    std::cout << "\n";

    updateVessel(d, ts);
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
    const auto interval = std::chrono::milliseconds(cfg_.gga_out.send_interval_ms);
    std::cout << "[" << cfg_.name << "] GGA output loop started"
              << " (transport=" << cfg_.gga_out.transport.shared_with
              << " interval=" << cfg_.gga_out.send_interval_ms << "ms)\n";

    while (running_) {
        {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            cv_.wait_for(lk, interval, [this] { return !running_.load(); });
        }
        if (!running_) break;
        if (!tx_.ptr || !tx_.ptr->isOpen()) continue;

        sendGgaToDevice();
    }

    std::cout << "[" << cfg_.name << "] GGA output loop stopped\n";
}

bool AisDevice::sendGgaToDevice() {
    std::string gga;
    double gga_ts;
    {
        std::lock_guard<std::mutex> lk(gga_mutex_);
        gga    = last_gga_;
        gga_ts = last_gga_ts_;
    }

    if (gga.empty()) return false;

    double age = epochNow() - gga_ts;
    if (age > cfg_.gga_out.data_timeout_sec) {
        std::cerr << "[" << cfg_.name << "] GGA stale ("
                  << std::fixed << std::setprecision(1) << age << "s) — skipping\n";
        return false;
    }

    // Send the raw $GPGGA sentence directly to the transponder
    if (!tx_.ptr->send(gga + "\r\n")) {
        std::cerr << "[" << cfg_.name << "] GGA send failed\n";
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.gga_sent_count++;
    }
    std::cout << "[" << cfg_.name << "] GGA→device: " << gga << "\n";
    return true;
}
