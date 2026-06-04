#include <iostream>
#include <csignal>
#include <atomic>
#include "Config.hpp"
#include "AisManager.hpp"

static std::atomic<bool>  g_running{true};
static AisManager        *g_manager = nullptr;

static void sigHandler(int) {
    g_running = false;
    if (g_manager)
        g_manager->stop();
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <config.json | config.ini>\n"
              << "  Supports JSON (.json) and INI (.ini) config formats.\n"
              << "  See config/ais_config.json for an example.\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    AppConfig cfg;
    try {
        cfg = AppConfig::fromFile(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "AIS Manager starting\n"
              << "  MQTT:             " << cfg.mqtt.broker << ":" << cfg.mqtt.port << "\n"
              << "  Device:           " << cfg.ais.ais_device.name << "\n"
              << "  Transport:        " << cfg.ais.ais_device.transport.type << "\n"
              << "  Init commands:    " << cfg.ais.init_commands.size() << "\n"
              << "  Publish interval: " << cfg.ais.publish_interval_ms << " ms\n"
              << "  AIS topic:        " << cfg.mqtt.topics.ais    << "\n"
              << "  Status topic:     " << cfg.mqtt.topics.status << "\n";

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    try {
        AisManager manager(cfg);
        g_manager = &manager;
        manager.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }

    std::cout << "AIS Manager stopped\n";
    return 0;
}
