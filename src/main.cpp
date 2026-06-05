#include <cstdio>
#include <csignal>
#include <atomic>
#include "Config.hpp"
#include "Logger.hpp"
#include "AisManager.hpp"

#define MOD "Main"

static std::atomic<bool>  g_running{true};
static AisManager*        g_manager = nullptr;

static void sigHandler(int sig) {
    LOG_WRN(MOD, "Signal %d received — shutting down", sig);
    g_running = false;
    if (g_manager) g_manager->stop();
}

static LogLevel parseLevel(const std::string& s) {
    if (s == "debug") return LogLevel::DEBUG;
    if (s == "info")  return LogLevel::INFO;
    if (s == "error") return LogLevel::ERROR;
    return LogLevel::WARN;
}

int main(int argc, char* argv[]) {
    // Initialise logger FIRST — records app start time for tick counter
    Logger::init();

    std::printf("╔══════════════════════════╗\n");
    std::printf("║       AIS Manager        ║\n");
    std::printf("║  Comar R220U / NMEA AIS  ║\n");
    std::printf("╚══════════════════════════╝\n\n");

    if (argc < 2) {
        std::fprintf(stderr,
            "Usage: %s <config.json | config.ini>\n"
            "  Both JSON and INI config formats are supported.\n", argv[0]);
        return 1;
    }

    // ── Load config ───────────────────────────────────────────────────────────
    AppConfig cfg;
    try {
        cfg = AppConfig::fromFile(argv[1]);
    } catch (const std::exception& e) {
        LOG_ERR(MOD, "Config load failed: %s", e.what());
        return 1;
    }

    // ── Apply debug settings ──────────────────────────────────────────────────
    Logger::setEnabled(cfg.debug.enabled);
    Logger::setLevel(parseLevel(cfg.debug.level));

    // ── Startup summary ───────────────────────────────────────────────────────
    LOG_INF(MOD, "Config: %s", argv[1]);
    LOG_INF(MOD, "Debug: %s  level=%s  (ERR+WRN always shown)",
            cfg.debug.enabled ? "ON" : "OFF", cfg.debug.level.c_str());
    LOG_INF(MOD, "MQTT: %s:%d  client='%s'  enabled=%s",
            cfg.mqtt.broker.c_str(), cfg.mqtt.port,
            cfg.mqtt.client_id.c_str(),
            cfg.mqtt.enabled ? "yes" : "no");
    LOG_INF(MOD, "Transport pool: %zu  Devices: %zu",
            cfg.ais.transports.size(), cfg.ais.devices.size());

    for (const auto& td : cfg.ais.transports)
        LOG_INF(MOD, "  transport[%s] type=%s  enabled=%s",
                td.id.c_str(), td.type.c_str(), td.enabled ? "yes" : "NO");

    for (const auto& d : cfg.ais.devices)
        LOG_INF(MOD, "  device[%d] '%s'  enabled=%s  publish=%s  "
                "aivdm_debug=%s  gga_debug=%s",
                d.id, d.name.c_str(),
                d.enabled          ? "yes" : "NO",
                d.publish_enabled  ? "yes" : "no",
                d.aivdm_in.debug   ? "on"  : "off",
                d.gga_out.debug    ? "on"  : "off");

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    // ── Run ───────────────────────────────────────────────────────────────────
    try {
        AisManager manager(cfg);
        g_manager = &manager;
        manager.run();
    } catch (const std::exception& e) {
        LOG_ERR(MOD, "Fatal exception: %s", e.what());
        return 1;
    }

    LOG_INF(MOD, "Stopped cleanly  uptime=%llums", (unsigned long long)Logger::tick());
    return 0;
}
