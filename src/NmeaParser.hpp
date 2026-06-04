#pragma once
#include <string>
#include <optional>
#include <vector>

struct GgaData {
    std::string raw;
    std::string time_str;
    double      lat         = 0.0;
    double      lon         = 0.0;
    int         fix_quality = 0;
    int         num_sats    = 0;
    double      hdop        = 0.0;
    double      altitude    = 0.0;
};

struct ZdaData {
    std::string raw;
    std::string time_str;
    std::string date_str;   // YYYY-MM-DD
};

struct RmcData {
    std::string raw;
    std::string time_str;
    std::string date_str;   // YYYY-MM-DD
    double      lat         = 0.0;
    double      lon         = 0.0;
    double      speed_knots = 0.0;
    double      course_deg  = 0.0;
    bool        active      = false;
};

struct VtgData {
    std::string raw;
    double      course_true  = 0.0;
    double      course_mag   = 0.0;
    double      speed_knots  = 0.0;
    double      speed_kmh    = 0.0;
};

namespace NmeaParser {

bool              validateChecksum(const std::string& sentence);
std::string       sentenceType(const std::string& sentence);  // "GGA", "ZDA", etc.

std::optional<GgaData> parseGga(const std::string& sentence);
std::optional<ZdaData> parseZda(const std::string& sentence);
std::optional<RmcData> parseRmc(const std::string& sentence);
std::optional<VtgData> parseVtg(const std::string& sentence);

double nmeaToDecimal(const std::string& coord, char direction);

std::vector<std::string> splitSentences(const std::string& data);

const char* fixQualityName(int quality);

} // namespace NmeaParser
