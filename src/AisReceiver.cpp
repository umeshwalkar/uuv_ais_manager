#include "GnssReceiver.hpp"
#include <iostream>
#include <chrono>

using Clock = std::chrono::steady_clock;

GnssReceiver::GnssReceiver(const ReceiverConfig& cfg)
    : cfg_(cfg), transport_(makeTransport(cfg.transport))
{
    state_.id   = cfg.id;
    state_.name = cfg.name;
}

GnssReceiver::~GnssReceiver() { stop(); }

void GnssReceiver::start() {
    if (!cfg_.enabled) return;
    running_ = true;
    thread_ = std::thread(&GnssReceiver::runLoop, this);
}

void GnssReceiver::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    transport_->close();
}

ReceiverSnapshot GnssReceiver::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return state_;
}

int GnssReceiver::qualityScore(double timeout_sec) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!state_.gga) return -1;

    auto age = std::chrono::duration<double>(Clock::now() - state_.last_gga_time).count();
    if (age > timeout_sec) return -1;

    int score = state_.gga->fix_quality * 100;
    score += std::min(state_.gga->num_sats, 12) * 10;
    score -= static_cast<int>(state_.gga->hdop * 50);
    return score;
}

void GnssReceiver::runLoop() {
    if (!transport_->open()) {
        std::cerr << "[RX" << cfg_.id << "] Failed to open transport\n";
        return;
    }
    std::cout << "[RX" << cfg_.id << "] (" << cfg_.name << ") transport open\n";

    while (running_) {
        std::string line = transport_->readLine(cfg_.transport.read_timeout_ms);
        if (!line.empty()) processLine(line);
    }
}

void GnssReceiver::processLine(const std::string& line) {
    if (line.empty() || line[0] != '$') return;

    auto type = NmeaParser::sentenceType(line);
    if (type.empty()) return;

    std::lock_guard<std::mutex> lk(mtx_);
    state_.recv_count++;

    if (type == "GGA") {
        auto d = NmeaParser::parseGga(line);
        if (d) {
            state_.gga         = d;
            state_.last_gga_time = Clock::now();
            state_.healthy     = true;
            state_.error_count = 0;
            std::cout << "[RX" << cfg_.id << "] GGA lat=" << d->lat
                      << " lon=" << d->lon
                      << " fix=" << NmeaParser::fixQualityName(d->fix_quality)
                      << " sats=" << d->num_sats << "\n";
        } else {
            state_.error_count++;
        }
    } else if (type == "ZDA") {
        auto d = NmeaParser::parseZda(line);
        if (d) state_.zda = d;
    } else if (type == "RMC") {
        auto d = NmeaParser::parseRmc(line);
        if (d) state_.rmc = d;
    } else if (type == "VTG") {
        auto d = NmeaParser::parseVtg(line);
        if (d) state_.vtg = d;
    }
}
