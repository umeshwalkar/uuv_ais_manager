#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <sstream>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// Decoded AIS message (populated fields depend on message type)
// ─────────────────────────────────────────────────────────────────────────────

struct AisData {
    std::string sentence_type;  // "AIVDM" | "AIVDO"
    int         msg_type    = 0;
    uint32_t    mmsi        = 0;

    // Position fields (types 1,2,3,18,21)
    double      lat         = std::numeric_limits<double>::quiet_NaN();
    double      lon         = std::numeric_limits<double>::quiet_NaN();
    double      sog         = 0.0;   // knots
    double      cog         = 0.0;   // degrees
    int         heading     = 511;   // true heading, degrees (511 = unavailable)
    int         nav_status  = 15;    // navigation status (15 = undefined)
    int         rot         = -128;  // rate of turn deg/min (-128 = unavailable)
    int         time_stamp  = 63;    // UTC second (63 = unavailable)
    bool        pos_accuracy = false;
    bool        raim        = false;

    // Static data (types 5, 24)
    uint32_t    imo         = 0;
    std::string call_sign;
    std::string vessel_name;
    int         ship_type   = 0;
    int         dim_to_bow       = 0;
    int         dim_to_stern     = 0;
    int         dim_to_port      = 0;
    int         dim_to_starboard = 0;
    std::string destination;
    std::string eta;          // "MM/DD HH:MM"
    double      draught     = 0.0;  // metres

    // Type 21 AtoN
    int         aton_type   = 0;
    std::string aton_name;

    std::string raw;          // original NMEA sentence(s)
    double      recv_ts = 0.0; // Unix epoch seconds
};

// ─────────────────────────────────────────────────────────────────────────────
// AIS NMEA + 6-bit payload parser
// ─────────────────────────────────────────────────────────────────────────────

class AisParser {
public:
    // Feed one NMEA line. Returns AisData when a complete message is decoded
    // (immediately for single-part, after last fragment for multi-part).
    std::optional<AisData> parse(const std::string& sentence,
                                  bool validate_crc = true,
                                  double recv_ts = 0.0) {
        // Trim trailing whitespace
        std::string s = sentence;
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' '))
            s.pop_back();
        if (s.size() < 8) return std::nullopt;

        // Must start with ! (AIS sentences use ! not $)
        if (s[0] != '!') return std::nullopt;

        if (validate_crc && !validateChecksum(s)) return std::nullopt;

        // Strip checksum for splitting: !AIVDM,f,n,id,ch,payload,fill*CS
        std::string body = s;
        auto star = body.rfind('*');
        if (star != std::string::npos) body = body.substr(0, star);

        auto parts = split(body, ',');
        if (parts.size() < 7) return std::nullopt;

        std::string stype = parts[0].substr(1); // "AIVDM" or "AIVDO"
        if (stype != "AIVDM" && stype != "AIVDO") return std::nullopt;

        int frag_count = parts[1].empty() ? 1 : std::stoi(parts[1]);
        int frag_seq   = parts[2].empty() ? 1 : std::stoi(parts[2]);
        std::string seq_id  = parts[3];
        char channel        = parts[4].empty() ? 'A' : parts[4][0];
        std::string payload = parts[5];
        int fill_bits       = parts[6].empty() ? 0 : std::stoi(parts[6]);

        if (frag_count == 1) {
            return decode(payload, fill_bits, stype, s, recv_ts);
        }

        // Multi-part reassembly
        std::string key = seq_id + channel;
        auto& frag = fragments_[key];
        if (frag_seq == 1) {
            frag.total        = frag_count;
            frag.received     = 0;
            frag.payload.clear();
            frag.raw.clear();
            frag.sentence_type = stype;
            frag.channel      = channel;
        }
        frag.payload += payload;
        frag.raw     += (frag.raw.empty() ? "" : " | ") + s;
        frag.received++;
        frag.fill_bits = fill_bits;

        if (frag.received < frag.total) return std::nullopt;

        auto result = decode(frag.payload, frag.fill_bits, frag.sentence_type, frag.raw, recv_ts);
        fragments_.erase(key);
        return result;
    }

    static bool validateChecksum(const std::string& sentence) {
        if (sentence.size() < 4 || sentence[0] != '!') return false;
        auto star = sentence.rfind('*');
        if (star == std::string::npos || star + 3 > sentence.size()) return false;
        uint8_t calc = 0;
        for (size_t i = 1; i < star; ++i) calc ^= (uint8_t)sentence[i];
        char hex[3] = { sentence[star+1], sentence[star+2], '\0' };
        uint8_t given = (uint8_t)std::strtoul(hex, nullptr, 16);
        return calc == given;
    }

    // Build NMEA checksum string "*XX" for a sentence body (without ! or *)
    static std::string makeChecksum(const std::string& body) {
        uint8_t cs = 0;
        for (char c : body) cs ^= (uint8_t)c;
        char buf[8];
        std::snprintf(buf, sizeof(buf), "*%02X", cs);
        return buf;
    }

private:
    struct Fragment {
        int         total     = 0;
        int         received  = 0;
        int         fill_bits = 0;
        std::string payload;
        std::string raw;
        std::string sentence_type;
        char        channel   = ' ';
    };
    std::unordered_map<std::string, Fragment> fragments_;

    // ── 6-bit payload → bit vector ────────────────────────────────────────────

    static std::vector<uint8_t> payloadToBits(const std::string& payload, int fill_bits) {
        std::vector<uint8_t> bits;
        bits.reserve(payload.size() * 6);
        for (char c : payload) {
            int v = (int)(uint8_t)c - 48;
            if (v < 0) continue;
            if (v > 40) v -= 8;
            for (int b = 5; b >= 0; --b)
                bits.push_back((v >> b) & 1);
        }
        // Remove fill bits from the end
        int remove = fill_bits;
        while (remove-- > 0 && !bits.empty())
            bits.pop_back();
        return bits;
    }

    // ── bit extractors ────────────────────────────────────────────────────────

    static uint32_t ubits(const std::vector<uint8_t>& b, int start, int len) {
        uint32_t v = 0;
        int end = std::min(start + len, (int)b.size());
        for (int i = start; i < end; ++i)
            v = (v << 1) | b[i];
        int missing = (start + len) - end;
        v <<= missing;
        return v;
    }

    static int32_t sbits(const std::vector<uint8_t>& b, int start, int len) {
        uint32_t v = ubits(b, start, len);
        if (len > 0 && (v >> (len - 1)) & 1)
            v |= ~((1u << len) - 1);
        return (int32_t)v;
    }

    // 6-bit ASCII characters (len = number of characters, not bits)
    static std::string aisStr(const std::vector<uint8_t>& b, int start_bit, int chars) {
        std::string s;
        s.reserve(chars);
        for (int i = 0; i < chars; ++i) {
            int v = (int)ubits(b, start_bit + i * 6, 6);
            if (v < 32) v += 64;
            s += (char)v;
        }
        // Trim trailing spaces and @ (AIS padding character)
        while (!s.empty() && (s.back() == ' ' || s.back() == '@'))
            s.pop_back();
        return s;
    }

    // ── position conversion ───────────────────────────────────────────────────

    static double aisLon(int32_t raw) {  // 28-bit signed, 1/10000 min
        if (raw == 0x6791AC0) return std::numeric_limits<double>::quiet_NaN();
        return raw / 600000.0;
    }
    static double aisLat(int32_t raw) {  // 27-bit signed, 1/10000 min
        if (raw == 0x3412140) return std::numeric_limits<double>::quiet_NaN();
        return raw / 600000.0;
    }
    static double aisSog(uint32_t raw) {  // 10-bit, 0.1 knot units
        if (raw >= 1023) return 0.0;
        return raw / 10.0;
    }
    static double aisCog(uint32_t raw) {  // 12-bit, 0.1 degree units
        if (raw >= 3600) return 0.0;
        return raw / 10.0;
    }

    // ── message decoders ──────────────────────────────────────────────────────

    static AisData decode123(const std::vector<uint8_t>& b,
                              const std::string& raw, const std::string& stype, double ts) {
        AisData d;
        d.raw          = raw;
        d.recv_ts      = ts;
        d.sentence_type = stype;
        d.msg_type     = (int)ubits(b, 0, 6);
        d.mmsi         = ubits(b, 8, 30);
        d.nav_status   = (int)ubits(b, 38, 4);
        d.rot          = sbits(b, 42, 8);
        d.sog          = aisSog(ubits(b, 50, 10));
        d.pos_accuracy = ubits(b, 60, 1) != 0;
        d.lon          = aisLon(sbits(b, 61, 28));
        d.lat          = aisLat(sbits(b, 89, 27));
        d.cog          = aisCog(ubits(b, 116, 12));
        d.heading      = (int)ubits(b, 128, 9);
        d.time_stamp   = (int)ubits(b, 137, 6);
        d.raim         = ubits(b, 148, 1) != 0;
        return d;
    }

    static AisData decode5(const std::vector<uint8_t>& b,
                            const std::string& raw, const std::string& stype, double ts) {
        AisData d;
        d.raw           = raw;
        d.recv_ts       = ts;
        d.sentence_type = stype;
        d.msg_type      = 5;
        d.mmsi          = ubits(b, 8, 30);
        d.imo           = ubits(b, 40, 30);
        d.call_sign     = aisStr(b, 70,  7);
        d.vessel_name   = aisStr(b, 112, 20);
        d.ship_type     = (int)ubits(b, 232, 8);
        d.dim_to_bow    = (int)ubits(b, 240, 9);
        d.dim_to_stern  = (int)ubits(b, 249, 9);
        d.dim_to_port   = (int)ubits(b, 258, 6);
        d.dim_to_starboard = (int)ubits(b, 264, 6);
        // ETA: month(4) day(5) hour(5) minute(6)
        int eta_mon  = (int)ubits(b, 274, 4);
        int eta_day  = (int)ubits(b, 278, 5);
        int eta_hr   = (int)ubits(b, 283, 5);
        int eta_min  = (int)ubits(b, 288, 6);
        char etabuf[24];
        std::snprintf(etabuf, sizeof(etabuf), "%02d/%02d %02d:%02d",
                      eta_mon, eta_day, eta_hr, eta_min);
        d.eta         = etabuf;
        d.draught     = ubits(b, 294, 8) / 10.0;
        d.destination = aisStr(b, 302, 20);
        return d;
    }

    static AisData decode18(const std::vector<uint8_t>& b,
                             const std::string& raw, const std::string& stype, double ts) {
        AisData d;
        d.raw           = raw;
        d.recv_ts       = ts;
        d.sentence_type = stype;
        d.msg_type      = 18;
        d.mmsi          = ubits(b, 8, 30);
        d.sog           = aisSog(ubits(b, 46, 10));
        d.pos_accuracy  = ubits(b, 56, 1) != 0;
        d.lon           = aisLon(sbits(b, 57, 28));
        d.lat           = aisLat(sbits(b, 85, 27));
        d.cog           = aisCog(ubits(b, 112, 12));
        d.heading       = (int)ubits(b, 124, 9);
        d.time_stamp    = (int)ubits(b, 133, 6);
        d.raim          = ubits(b, 147, 1) != 0;
        return d;
    }

    static AisData decode21(const std::vector<uint8_t>& b,
                             const std::string& raw, const std::string& stype, double ts) {
        AisData d;
        d.raw           = raw;
        d.recv_ts       = ts;
        d.sentence_type = stype;
        d.msg_type      = 21;
        d.mmsi          = ubits(b, 8, 30);
        d.aton_type     = (int)ubits(b, 38, 5);
        d.aton_name     = aisStr(b, 43, 20);
        d.pos_accuracy  = ubits(b, 163, 1) != 0;
        d.lon           = aisLon(sbits(b, 164, 28));
        d.lat           = aisLat(sbits(b, 192, 27));
        return d;
    }

    static AisData decode24(const std::vector<uint8_t>& b,
                             const std::string& raw, const std::string& stype, double ts) {
        AisData d;
        d.raw           = raw;
        d.recv_ts       = ts;
        d.sentence_type = stype;
        d.msg_type      = 24;
        d.mmsi          = ubits(b, 8, 30);
        int part        = (int)ubits(b, 38, 2);
        if (part == 0) {
            d.vessel_name = aisStr(b, 40, 20);
        } else {
            d.ship_type        = (int)ubits(b, 40, 8);
            d.call_sign        = aisStr(b, 90, 7);
            d.dim_to_bow       = (int)ubits(b, 132, 9);
            d.dim_to_stern     = (int)ubits(b, 141, 9);
            d.dim_to_port      = (int)ubits(b, 150, 6);
            d.dim_to_starboard = (int)ubits(b, 156, 6);
        }
        return d;
    }

    // ── dispatch ──────────────────────────────────────────────────────────────

    std::optional<AisData> decode(const std::string& payload, int fill_bits,
                                   const std::string& stype,
                                   const std::string& raw, double ts) {
        if (payload.empty()) return std::nullopt;
        auto bits = payloadToBits(payload, fill_bits);
        if (bits.size() < 6) return std::nullopt;

        int msg_type = (int)ubits(bits, 0, 6);
        switch (msg_type) {
            case 1: case 2: case 3:
                if (bits.size() >= 149) return decode123(bits, raw, stype, ts);
                break;
            case 5:
                if (bits.size() >= 424) return decode5(bits, raw, stype, ts);
                break;
            case 18:
                if (bits.size() >= 148) return decode18(bits, raw, stype, ts);
                break;
            case 21:
                if (bits.size() >= 219) return decode21(bits, raw, stype, ts);
                break;
            case 24:
                if (bits.size() >= 40)  return decode24(bits, raw, stype, ts);
                break;
            default:
                // Return minimal record for unhandled types
                AisData d;
                d.raw = raw; d.recv_ts = ts; d.sentence_type = stype;
                d.msg_type = msg_type;
                if (bits.size() >= 38) d.mmsi = ubits(bits, 8, 30);
                return d;
        }
        return std::nullopt;
    }

    static std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> v;
        std::string tok;
        for (char c : s) {
            if (c == delim) { v.push_back(tok); tok.clear(); }
            else tok += c;
        }
        v.push_back(tok);
        return v;
    }
};
