#include "Config.hpp"
#include <fstream>
#include <algorithm>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── helpers ───────────────────────────────────────────────────────────────────

static TransportDef parseTransportDef(const json& t) {
    TransportDef d;
    if (t.contains("id"))                  d.id                  = t["id"];
    if (t.contains("enabled"))             d.enabled             = t["enabled"];
    if (t.contains("type"))                d.type                = t["type"];
    if (t.contains("bind_host"))           d.bind_host           = t["bind_host"];
    if (t.contains("bind_port"))           d.bind_port           = t["bind_port"];
    if (t.contains("host"))                d.host                = t["host"];
    if (t.contains("port"))                d.port                = t["port"];
    if (t.contains("serial_port"))         d.serial_port         = t["serial_port"];
    if (t.contains("serial_baud"))         d.serial_baud         = t["serial_baud"];
    if (t.contains("serial_data_bits"))    d.serial_data_bits    = t["serial_data_bits"];
    if (t.contains("serial_stop_bits"))    d.serial_stop_bits    = t["serial_stop_bits"];
    if (t.contains("serial_parity"))       d.serial_parity       = t["serial_parity"];
    if (t.contains("connect_timeout_sec")) d.connect_timeout_sec = t["connect_timeout_sec"];
    if (t.contains("reconnect_delay_sec")) d.reconnect_delay_sec = t["reconnect_delay_sec"];
    if (t.contains("read_timeout_ms"))     d.read_timeout_ms     = t["read_timeout_ms"];
    if (t.contains("buffer_size_bytes"))   d.buffer_size_bytes   = t["buffer_size_bytes"];
    return d;
}

static ChannelTransportRef parseRef(const json& t) {
    ChannelTransportRef r;
    if (t.contains("id")) r.id = t["id"];
    return r;
}

static AisDeviceConfig parseDevice(const json& d, int default_id) {
    AisDeviceConfig dev;
    dev.id = default_id;
    if (d.contains("id"))                     dev.id                    = d["id"];
    if (d.contains("name"))                   dev.name                  = d["name"];
    if (d.contains("enabled"))                dev.enabled               = d["enabled"];
    if (d.contains("sync_timeout_sec"))       dev.sync_timeout_sec      = d["sync_timeout_sec"];
    if (d.contains("send_init_on_reconnect")) dev.send_init_on_reconnect = d["send_init_on_reconnect"];
    if (d.contains("publish_enabled"))        dev.publish_enabled        = d["publish_enabled"];
    if (d.contains("publish_raw_ais"))        dev.publish_raw_ais        = d["publish_raw_ais"];
    if (d.contains("publish_interval_ms"))    dev.publish_interval_ms    = d["publish_interval_ms"];
    if (d.contains("validate_checksum"))      dev.validate_checksum      = d["validate_checksum"];

    if (d.contains("init_commands") && d["init_commands"].is_array())
        for (auto& cmd : d["init_commands"])
            dev.init_commands.push_back(cmd.get<std::string>());

    if (d.contains("input_channels")) {
        auto& ic = d["input_channels"];
        if (ic.contains("aivdm")) {
            auto& a = ic["aivdm"];
            if (a.contains("enabled"))          dev.aivdm_in.enabled          = a["enabled"];
            if (a.contains("debug"))            dev.aivdm_in.debug            = a["debug"];
            if (a.contains("data_timeout_sec")) dev.aivdm_in.data_timeout_sec = a["data_timeout_sec"];
            if (a.contains("transport"))        dev.aivdm_in.transport        = parseRef(a["transport"]);
        }
    }

    if (d.contains("output_channels")) {
        auto& oc = d["output_channels"];
        if (oc.contains("gga")) {
            auto& g = oc["gga"];
            if (g.contains("enabled"))          dev.gga_out.enabled          = g["enabled"];
            if (g.contains("debug"))            dev.gga_out.debug            = g["debug"];
            if (g.contains("send_interval_ms")) dev.gga_out.send_interval_ms = g["send_interval_ms"];
            if (g.contains("data_timeout_sec")) dev.gga_out.data_timeout_sec = g["data_timeout_sec"];
            if (g.contains("transport"))        dev.gga_out.transport        = parseRef(g["transport"]);
        }
    }
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

    if (j.contains("debug")) {
        auto& d = j["debug"];
        if (d.contains("enabled")) cfg.debug.enabled = d["enabled"];
        if (d.contains("level"))   cfg.debug.level   = d["level"];
    }

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
        if (a.contains("status_interval_sec")) cfg.ais.status_interval_sec = a["status_interval_sec"];

        if (a.contains("transport") && a["transport"].is_array())
            for (auto& t : a["transport"])
                cfg.ais.transports.push_back(parseTransportDef(t));

        if (a.contains("devices") && a["devices"].is_array()) {
            int idx = 1;
            for (auto& d : a["devices"])
                cfg.ais.devices.push_back(parseDevice(d, idx++));
        }
    }

    if (cfg.ais.transports.empty()) throw std::runtime_error("Config: ais.transport array is empty");
    if (cfg.ais.devices.empty())    throw std::runtime_error("Config: ais.devices array is empty");
    return cfg;
}

// ── INI loader ────────────────────────────────────────────────────────────────
// Section conventions:
//   [ais.transport.ch1]  — one transport definition (id = section suffix)
//   [ais.device1]        — device 1 settings
//   [ais.device1.input.aivdm]   — aivdm input channel
//   [ais.device1.output.gga]    — gga output channel
//   [ais.device1.init_commands] — one cmd = VALUE per line

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
            (unsigned char)line[2] == 0xBF) line.erase(0, 3);
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
        auto v = get(s,k); return v.empty() ? d : std::stoi(v); };
    auto getD = [&](const std::string& s, const std::string& k, double d) {
        auto v = get(s,k); return v.empty() ? d : std::stod(v); };
    auto getB = [&](const std::string& s, const std::string& k, bool d) {
        auto v = get(s,k); return v.empty() ? d : (v=="true"||v=="1"); };

    // Debug
    cfg.debug.enabled = getB("debug","enabled", cfg.debug.enabled);
    cfg.debug.level   = get ("debug","level",   cfg.debug.level);

    // MQTT
    cfg.mqtt.enabled   = getB("mqtt","enabled",   cfg.mqtt.enabled);
    cfg.mqtt.broker    = get ("mqtt","broker",    cfg.mqtt.broker);
    cfg.mqtt.port      = getI("mqtt","port",      cfg.mqtt.port);
    cfg.mqtt.client_id = get ("mqtt","client_id", cfg.mqtt.client_id);
    cfg.mqtt.keepalive = getI("mqtt","keepalive", cfg.mqtt.keepalive);
    cfg.mqtt.qos       = getI("mqtt","qos",       cfg.mqtt.qos);
    cfg.mqtt.retain    = getB("mqtt","retain",    cfg.mqtt.retain);
    cfg.mqtt.topics.ais      = get("mqtt.topics","ais",      cfg.mqtt.topics.ais);
    cfg.mqtt.topics.status   = get("mqtt.topics","status",   cfg.mqtt.topics.status);
    cfg.mqtt.topics.gnss_gga = get("mqtt.topics","gnss_gga", cfg.mqtt.topics.gnss_gga);

    // AIS global
    cfg.ais.status_interval_sec = getI("ais","status_interval_sec", cfg.ais.status_interval_sec);

    // Transport pool: [ais.transport.<id>]
    const std::string tp_prefix = "ais.transport.";
    for (auto& [sec, kv] : data) {
        if (sec.substr(0, tp_prefix.size()) != tp_prefix) continue;
        std::string tid = sec.substr(tp_prefix.size());
        TransportDef td;
        td.id                  = get(sec, "id",                  tid);  // allow override
        td.enabled             = getB(sec,"enabled",             true);
        td.type                = get (sec,"type",                "tcp_client");
        td.bind_host           = get (sec,"bind_host",           td.bind_host);
        td.bind_port           = getI(sec,"bind_port",           0);
        td.host                = get (sec,"host",                "");
        td.port                = getI(sec,"port",                0);
        td.serial_port         = get (sec,"serial_port",         "");
        td.serial_baud         = getI(sec,"serial_baud",         9600);
        td.connect_timeout_sec = getI(sec,"connect_timeout_sec", 5);
        td.reconnect_delay_sec = getI(sec,"reconnect_delay_sec", 3);
        td.read_timeout_ms     = getI(sec,"read_timeout_ms",     1000);
        td.buffer_size_bytes   = getI(sec,"buffer_size_bytes",   1024);
        cfg.ais.transports.push_back(std::move(td));
    }

    // Devices: [ais.device1], [ais.device2], ...
    for (int i = 1; i <= 8; ++i) {
        std::string sec = "ais.device" + std::to_string(i);
        if (!data.count(sec)) continue;

        AisDeviceConfig dev;
        dev.id                    = getI(sec,"id",                    i);
        dev.name                  = get (sec,"name",                  "ais"+std::to_string(i));
        dev.enabled               = getB(sec,"enabled",               true);
        dev.sync_timeout_sec      = getD(sec,"sync_timeout_sec",      dev.sync_timeout_sec);
        dev.send_init_on_reconnect= getB(sec,"send_init_on_reconnect",false);
        dev.publish_enabled       = getB(sec,"publish_enabled",       true);
        dev.publish_raw_ais       = getB(sec,"publish_raw_ais",       false);
        dev.publish_interval_ms   = getI(sec,"publish_interval_ms",   1000);
        dev.validate_checksum     = getB(sec,"validate_checksum",     true);

        // init commands
        std::string cmd_sec = sec + ".init_commands";
        if (data.count(cmd_sec))
            for (auto& [k,v] : data[cmd_sec])
                dev.init_commands.push_back(v);

        // input_channels.aivdm
        std::string ain = sec + ".input.aivdm";
        dev.aivdm_in.enabled          = getB(ain,"enabled",          true);
        dev.aivdm_in.debug            = getB(ain,"debug",            false);
        dev.aivdm_in.data_timeout_sec = getD(ain,"data_timeout_sec", 5.0);
        dev.aivdm_in.transport.id = get(ain, "id", "");

        // output_channels.gga
        std::string gout = sec + ".output.gga";
        dev.gga_out.enabled          = getB(gout,"enabled",          false);
        dev.gga_out.debug            = getB(gout,"debug",            false);
        dev.gga_out.send_interval_ms = getI(gout,"send_interval_ms", 1000);
        dev.gga_out.data_timeout_sec = getD(gout,"data_timeout_sec", 2.0);
        dev.gga_out.transport.id = get(gout, "id", "");

        cfg.ais.devices.push_back(std::move(dev));
    }

    if (cfg.ais.transports.empty()) throw std::runtime_error("Config: no [ais.transport.*] sections");
    if (cfg.ais.devices.empty())    throw std::runtime_error("Config: no [ais.device*] sections");
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
