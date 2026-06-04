#include "NmeaParser.hpp"
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace NmeaParser {

// ── helpers ──────────────────────────────────────────────────────────────────

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::string tok;
    for (char c : s) {
        if (c == delim) { parts.push_back(tok); tok.clear(); }
        else            { tok += c; }
    }
    parts.push_back(tok);
    return parts;
}

// ── public API ────────────────────────────────────────────────────────────────

bool validateChecksum(const std::string& sentence) {
    // $...*XX
    if (sentence.size() < 4 || sentence[0] != '$') return false;
    auto star = sentence.rfind('*');
    if (star == std::string::npos || star + 3 > sentence.size()) return false;

    uint8_t calc = 0;
    for (size_t i = 1; i < star; ++i) calc ^= (uint8_t)sentence[i];

    char hex[3] = { sentence[star+1], sentence[star+2], '\0' };
    uint8_t given = (uint8_t)std::strtoul(hex, nullptr, 16);
    return calc == given;
}

std::string sentenceType(const std::string& sentence) {
    // $GPGGA,... → "GGA"   $GNGGA,... → "GGA"
    if (sentence.size() < 6 || sentence[0] != '$') return "";
    // talker id is 2 chars after $, sentence type is next 3
    auto comma = sentence.find(',');
    size_t id_end = (comma == std::string::npos) ? sentence.size() : comma;
    if (id_end >= 6) return sentence.substr(3, id_end - 3);
    return "";
}

double nmeaToDecimal(const std::string& coord, char direction) {
    if (coord.empty()) return 0.0;
    // DDMM.MMMMM or DDDMM.MMMMM
    auto dot = coord.find('.');
    if (dot == std::string::npos) return 0.0;
    // minutes field is 2 digits before decimal point
    size_t deg_digits = dot - 2;
    double degrees = std::stod(coord.substr(0, deg_digits));
    double minutes  = std::stod(coord.substr(deg_digits));
    double result   = degrees + minutes / 60.0;
    if (direction == 'S' || direction == 'W') result = -result;
    return result;
}

std::vector<std::string> splitSentences(const std::string& data) {
    std::vector<std::string> lines;
    std::istringstream ss(data);
    for (std::string line; std::getline(ss, line);) {
        line = trim(line);
        if (!line.empty() && line[0] == '$') lines.push_back(line);
    }
    return lines;
}

const char* fixQualityName(int q) {
    switch (q) {
        case 0: return "Invalid";
        case 1: return "GPS";
        case 2: return "DGPS";
        case 3: return "PPS";
        case 4: return "RTK Fixed";
        case 5: return "RTK Float";
        case 6: return "Estimated";
        case 7: return "Manual";
        case 8: return "Simulation";
        default: return "Unknown";
    }
}

// ── GGA ───────────────────────────────────────────────────────────────────────
// $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47

std::optional<GgaData> parseGga(const std::string& sentence) {
    auto type = sentenceType(sentence);
    if (type != "GGA") return std::nullopt;

    // Strip checksum for splitting
    auto star = sentence.rfind('*');
    auto body = (star != std::string::npos) ? sentence.substr(0, star) : sentence;
    auto parts = split(body, ',');
    if (parts.size() < 10) return std::nullopt;

    GgaData d;
    d.raw = sentence;
    try {
        d.time_str    = parts[1];
        // Fix quality first — skip invalid fix
        d.fix_quality = parts[6].empty() ? 0 : std::stoi(parts[6]);
        if (d.fix_quality == 0) return std::nullopt;

        if (!parts[2].empty() && !parts[3].empty())
            d.lat = nmeaToDecimal(parts[2], parts[3][0]);
        if (!parts[4].empty() && !parts[5].empty())
            d.lon = nmeaToDecimal(parts[4], parts[5][0]);
        d.num_sats = parts[7].empty() ? 0 : std::stoi(parts[7]);
        d.hdop     = parts[8].empty() ? 0.0 : std::stod(parts[8]);
        d.altitude = parts[9].empty() ? 0.0 : std::stod(parts[9]);
    } catch (...) { return std::nullopt; }
    return d;
}

// ── ZDA ───────────────────────────────────────────────────────────────────────
// $GPZDA,112313.791,04,08,2021,,*55

std::optional<ZdaData> parseZda(const std::string& sentence) {
    auto type = sentenceType(sentence);
    if (type != "ZDA") return std::nullopt;

    auto star = sentence.rfind('*');
    auto body = (star != std::string::npos) ? sentence.substr(0, star) : sentence;
    auto parts = split(body, ',');
    if (parts.size() < 5) return std::nullopt;

    ZdaData d;
    d.raw = sentence;
    try {
        d.time_str = parts[1];
        if (!parts[2].empty() && !parts[3].empty() && !parts[4].empty())
            d.date_str = parts[4] + "-" + parts[3] + "-" + parts[2]; // YYYY-MM-DD
    } catch (...) { return std::nullopt; }
    return d;
}

// ── RMC ───────────────────────────────────────────────────────────────────────
// $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A

std::optional<RmcData> parseRmc(const std::string& sentence) {
    auto type = sentenceType(sentence);
    if (type != "RMC") return std::nullopt;

    auto star = sentence.rfind('*');
    auto body = (star != std::string::npos) ? sentence.substr(0, star) : sentence;
    auto parts = split(body, ',');
    if (parts.size() < 10) return std::nullopt;

    RmcData d;
    d.raw = sentence;
    try {
        d.time_str = parts[1];
        d.active   = (!parts[2].empty() && parts[2][0] == 'A');
        if (!d.active) return std::nullopt;

        if (!parts[3].empty() && !parts[4].empty())
            d.lat = nmeaToDecimal(parts[3], parts[4][0]);
        if (!parts[5].empty() && !parts[6].empty())
            d.lon = nmeaToDecimal(parts[5], parts[6][0]);
        d.speed_knots = parts[7].empty() ? 0.0 : std::stod(parts[7]);
        d.course_deg  = parts[8].empty() ? 0.0 : std::stod(parts[8]);

        // DDMMYY → YYYY-MM-DD
        if (parts[9].size() == 6) {
            std::string dd = parts[9].substr(0,2);
            std::string mm = parts[9].substr(2,2);
            std::string yy = parts[9].substr(4,2);
            int year = 2000 + std::stoi(yy);
            d.date_str = std::to_string(year) + "-" + mm + "-" + dd;
        }
    } catch (...) { return std::nullopt; }
    return d;
}

// ── VTG ───────────────────────────────────────────────────────────────────────
// $GPVTG,054.7,T,034.4,M,005.5,N,010.2,K*48

std::optional<VtgData> parseVtg(const std::string& sentence) {
    auto type = sentenceType(sentence);
    if (type != "VTG") return std::nullopt;

    auto star = sentence.rfind('*');
    auto body = (star != std::string::npos) ? sentence.substr(0, star) : sentence;
    auto parts = split(body, ',');
    if (parts.size() < 8) return std::nullopt;

    VtgData d;
    d.raw = sentence;
    try {
        d.course_true = parts[1].empty() ? 0.0 : std::stod(parts[1]);
        d.course_mag  = parts[3].empty() ? 0.0 : std::stod(parts[3]);
        d.speed_knots = parts[5].empty() ? 0.0 : std::stod(parts[5]);
        d.speed_kmh   = parts[7].empty() ? 0.0 : std::stod(parts[7]);
    } catch (...) { return std::nullopt; }
    return d;
}

} // namespace NmeaParser
