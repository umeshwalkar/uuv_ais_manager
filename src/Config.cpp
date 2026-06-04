#include "Config.hpp"
#include <fstream>
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ───────────────────────────────────────────────────────────────────

static TransportConfig parseTransport(const json& t) {
    TransportConfig c;
    if (t.contains("type"))                c.type                = t["type"];
    if (t.contains("bind_host"))           c.bind_host           = t["bind_host"];
    if (t.contains("bind_port"))           c.bind_port           = t["bind_port"];
    if (t.contains("host"))                c.host                = t["host"];
    if (t.contains("port"))                c.port                = t["port"];
    if (t.contains("serial_port"))         c.serial_port         = t["serial_port"];
    if (t.contains("serial_baud"))         c.serial_baud         = t["serial_baud"];
    if (t.contains("serial_data_bits"))    c.serial_data_bits    = t["serial_data_bits"];
    if (t.contains("serial_stop_bits"))    c.serial_stop_bits    = t["serial_stop_bits"];
    if (t.contains("serial_parity"))       c.serial_parity       = t["serial_parity"];
    if (t.contains("connect_timeout_sec")) c.connect_timeout_sec = t["connect_timeout_sec"];
    if (t.contains("reconnect_delay_sec")) c.reconnect_delay_sec = t["reconnect_delay_sec"];
    if (t.contains("read_timeout_ms"))     c.read_timeout_ms     = t["read_timeout_ms"];
    if (t.contains("buffer_size_bytes"))   c.buffer_size_bytes   = t["buffer_size_bytes"];
    if (t.contains("shared_with"))         c.shared_with         = t["shared_with"];
    return c;
}

static GgaOutputConfig parseGgaOutput(const json& g) {
    GgaOutputConfig o;
    if (g.contains("enabled"))          o.enabled          = g["enabled"];
    if (g.contains("send_interval_ms")) o.send_interval_ms = g["send_interval_ms"];
    if (g.contains("data_timeout_sec")) o.data_timeout_sec = g["data_timeout_sec"];
    if (g.contains("transport"))        o.transport        = parseTransport(g["transport"]);
    return o;
}

static AisDeviceConfig parseDevice(const json& d, int default_id) {
    AisDeviceConfig dev;
    dev.id = default_id;
    if (d.contains("id"))                     dev.id                    = d["id"];
    if (d.contains("name"))                   dev.name                  = d["name"];
    if (d.contains("enabled"))                dev.enabled               = d["enabled"];
    if (d.contains("data_timeout_sec"))       dev.data_timeout_sec      = d["data_timeout_sec"];
    if (d.contains("sync_timeout_sec"))       dev.sync_timeout_sec      = d["sync_timeout_sec"];
    if (d.contains("send_init_on_reconnect")) dev.send_init_on_reconnect = d["send_init_on_reconnect"];
    if (d.contains("init_commands") && d["init_commands"].is_array())
        for (auto& cmd : d["init_commands"])
            dev.init_commands.push_back(cmd.get<std::string>());
    if (d.contains("transport")) dev.transport = parseTransport(d["transport"]);

    // Per-device GGA output: output_channels.gga
    if (d.contains("output_channels") && d["output_channels"].contains("gga"))
        dev.gga_output = parseGgaOutput(d["output_channels"]["gga"]);

    return dev;
}

// ── JSON loader ───────────────────────────────────────────────────────────────

AppConfig AppConfig::fromJsonFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config: " + path);

    json j;
    try { j = json::parse(f, nullptr, true, true); }
    catch (json::exception& e) {
        throw std::runtime_error(std::string("JSON parse error: ") + e.what());
    }

    AppConfig cfg;

    if (j.contains("mqtt")) {
        auto& m = j["mqtt"];
        if (m.contains("enabled"))   cfg.mqtt.enabled   = m["enabled"];
        if (m.contains("broker"))    cfg.mqtt.broker    = m["broker"];
        if (m.contains("port"))      cfg.mqtt.port      = m["port"];
        if (m.contains("client_id")) cfg.mqtt.client_id = m["client_id"];
        if (m.contains("keepalive")) cfg.mqtt.keepalive = m["keepalive"];
        if (m.contains("qos"))       cfg.mqtt.qos       = m["qos"];
        if (m.contains("retain"))    cfg.mqtt.retain    = m["retain"];
        if (m.contains("topics")) {
            auto& t = m["topics"];
            if (t.contains("ais"))      cfg.mqtt.topics.ais      = t["ais"];
            if (t.contains("status"))   cfg.mqtt.topics.status   = t["status"];
            if (t.contains("gnss_gga")) cfg.mqtt.topics.gnss_gga = t["gnss_gga"];
        }
    }

    if (j.contains("ais")) {
        auto& a = j["ais"];
        if (a.contains("publish_interval_ms"))    cfg.ais.publish_interval_ms    = a["publish_interval_ms"];
        if (a.contains("data_timeout_sec"))       cfg.ais.data_timeout_sec       = a["data_timeout_sec"];
        if (a.contains("status_interval_sec"))    cfg.ais.status_interval_sec    = a["status_interval_sec"];
        if (a.contains("publish_raw_ais"))        cfg.ais.publish_raw_ais        = a["publish_raw_ais"];
        if (a.contains("validate_crc"))           cfg.ais.validate_crc           = a["validate_crc"];
        if (a.contains("require_position_valid")) cfg.ais.require_position_valid = a["require_position_valid"];

        if (a.contains("devices") && a["devices"].is_array()) {
            int idx = 1;
            for (auto& d : a["devices"])
                cfg.ais.devices.push_back(parseDevice(d, idx++));
        }
    }

    if (cfg.ais.devices.empty())
        throw std::runtime_error("Config error: ais.devices array is empty");
    return cfg;
}

// ── INI loader ────────────────────────────────────────────────────────────────

AppConfig AppConfig::fromIniFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config: " + path);

    AppConfig cfg;
    std::string section;
    std::map<std::string, std::map<std::string, std::string>> data;

    for (std::string line; std::getline(f, line);) {
        if (line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        auto last = line.find_last_not_of(" \t\r\n");
        if (last == std::string::npos) continue;
        line.erase(last + 1);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        val.erase(0, val.find_first_not_of(" \t"));
        if (val.empty()) continue;
        data[section][key] = val;
    }

    auto get  = [&](const std::string& s, const std::string& k, const std::string& d = "") {
        auto si = data.find(s); if (si == data.end()) return d;
        auto ki = si->second.find(k); return ki == si->second.end() ? d : ki->second;
    };
    auto getI = [&](const std::string& s, const std::string& k, int d) {
        auto v = get(s, k); return v.empty() ? d : std::stoi(v);
    };
    auto getD = [&](const std::string& s, const std::string& k, double d) {
        auto v = get(s, k); return v.empty() ? d : std::stod(v);
    };
    auto getB = [&](const std::string& s, const std::string& k, bool d) {
        auto v = get(s, k); return v.empty() ? d : (v == "true" || v == "1");
    };

    cfg.mqtt.enabled   = getB("mqtt", "enabled",   cfg.mqtt.enabled);
    cfg.mqtt.broker    = get ("mqtt", "broker",    cfg.mqtt.broker);
    cfg.mqtt.port      = getI("mqtt", "port",      cfg.mqtt.port);
    cfg.mqtt.client_id = get ("mqtt", "client_id", cfg.mqtt.client_id);
    cfg.mqtt.keepalive = getI("mqtt", "keepalive", cfg.mqtt.keepalive);
    cfg.mqtt.qos       = getI("mqtt", "qos",       cfg.mqtt.qos);
    cfg.mqtt.retain    = getB("mqtt", "retain",    cfg.mqtt.retain);
    cfg.mqtt.topics.ais      = get("mqtt.topics", "ais",      cfg.mqtt.topics.ais);
    cfg.mqtt.topics.status   = get("mqtt.topics", "status",   cfg.mqtt.topics.status);
    cfg.mqtt.topics.gnss_gga = get("mqtt.topics", "gnss_gga", cfg.mqtt.topics.gnss_gga);

    cfg.ais.publish_interval_ms    = getI("ais", "publish_interval_ms",    cfg.ais.publish_interval_ms);
    cfg.ais.data_timeout_sec       = getD("ais", "data_timeout_sec",       cfg.ais.data_timeout_sec);
    cfg.ais.status_interval_sec    = getI("ais", "status_interval_sec",    cfg.ais.status_interval_sec);
    cfg.ais.publish_raw_ais        = getB("ais", "publish_raw_ais",        cfg.ais.publish_raw_ais);
    cfg.ais.validate_crc           = getB("ais", "validate_crc",           cfg.ais.validate_crc);
    cfg.ais.require_position_valid = getB("ais", "require_position_valid", cfg.ais.require_position_valid);

    // Devices: [ais.device1], [ais.device2], ...
    for (int i = 1; i <= 8; ++i) {
        std::string sec = "ais.device" + std::to_string(i);
        if (data.find(sec) == data.end()) continue;

        AisDeviceConfig dev;
        dev.id      = getI(sec, "id",      i);
        dev.name    = get (sec, "name",    "ais" + std::to_string(i));
        dev.enabled = getB(sec, "enabled", true);
        dev.data_timeout_sec       = getD(sec, "data_timeout_sec",       dev.data_timeout_sec);
        dev.sync_timeout_sec       = getD(sec, "sync_timeout_sec",       dev.sync_timeout_sec);
        dev.send_init_on_reconnect = getB(sec, "send_init_on_reconnect", dev.send_init_on_reconnect);

        dev.transport.type                = get (sec, "type",                dev.transport.type);
        dev.transport.bind_host           = get (sec, "bind_host",           dev.transport.bind_host);
        dev.transport.bind_port           = getI(sec, "bind_port",           dev.transport.bind_port);
        dev.transport.host                = get (sec, "host",                dev.transport.host);
        dev.transport.port                = getI(sec, "port",                dev.transport.port);
        dev.transport.serial_port         = get (sec, "serial_port",         dev.transport.serial_port);
        dev.transport.serial_baud         = getI(sec, "serial_baud",         dev.transport.serial_baud);
        dev.transport.connect_timeout_sec = getI(sec, "connect_timeout_sec", dev.transport.connect_timeout_sec);
        dev.transport.reconnect_delay_sec = getI(sec, "reconnect_delay_sec", dev.transport.reconnect_delay_sec);
        dev.transport.read_timeout_ms     = getI(sec, "read_timeout_ms",     dev.transport.read_timeout_ms);
        dev.transport.buffer_size_bytes   = getI(sec, "buffer_size_bytes",   dev.transport.buffer_size_bytes);

        // Per-device init commands: [ais.device1.init_commands]
        std::string cmd_sec = sec + ".init_commands";
        if (data.count(cmd_sec))
            for (auto& [k, v] : data[cmd_sec])
                dev.init_commands.push_back(v);

        // Per-device GGA output: [ais.device1.gga_output]
        std::string gga_sec = sec + ".gga_output";
        dev.gga_output.enabled          = getB(gga_sec, "enabled",          dev.gga_output.enabled);
        dev.gga_output.send_interval_ms = getI(gga_sec, "send_interval_ms", dev.gga_output.send_interval_ms);
        dev.gga_output.data_timeout_sec = getD(gga_sec, "data_timeout_sec", dev.gga_output.data_timeout_sec);
        // shared_with defaults to device's own name (use same transport)
        std::string sw = get(gga_sec, "shared_with", dev.name);
        dev.gga_output.transport.shared_with = sw;

        cfg.ais.devices.push_back(std::move(dev));
    }

    if (cfg.ais.devices.empty())
        throw std::runtime_error("Config error: no [ais.deviceN] sections found");
    return cfg;
}

// ── auto-detect ───────────────────────────────────────────────────────────────

AppConfig AppConfig::fromFile(const std::string& path) {
    std::string ext;
    auto pos = path.rfind('.');
    if (pos != std::string::npos) {
        ext = path.substr(pos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    if (ext == ".json") return fromJsonFile(path);
    if (ext == ".ini")  return fromIniFile(path);
    throw std::runtime_error("Unknown config format: " + path + " (expected .json or .ini)");
}
