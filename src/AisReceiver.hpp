#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <optional>
#include "Config.hpp"
#include "NmeaParser.hpp"
#include "Transport.hpp"

struct ReceiverSnapshot {
    int                    id          = 0;
    std::string            name;
    std::optional<GgaData> gga;
    std::optional<ZdaData> zda;
    std::optional<RmcData> rmc;
    std::optional<VtgData> vtg;
    std::chrono::steady_clock::time_point last_gga_time;
    bool                   healthy     = false;
    uint64_t               recv_count  = 0;
    uint64_t               error_count = 0;
};

class GnssReceiver {
public:
    explicit GnssReceiver(const ReceiverConfig& cfg);
    ~GnssReceiver();

    void start();
    void stop();

    ReceiverSnapshot snapshot() const;
    int              qualityScore(double timeout_sec) const;

    int         id()   const { return cfg_.id;   }
    std::string name() const { return cfg_.name; }

private:
    ReceiverConfig              cfg_;
    std::unique_ptr<ITransport> transport_;

    mutable std::mutex          mtx_;
    ReceiverSnapshot            state_;

    std::thread                 thread_;
    std::atomic<bool>           running_{false};

    void runLoop();
    void processLine(const std::string& line);
};
