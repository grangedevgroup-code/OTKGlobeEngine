#include "otk/klv.hpp"

#include "core/geo.hpp"

#include "misb.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace otk {

namespace {

constexpr std::uint8_t kUasKey[16] = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x0B, 0x01, 0x01, 0x0E, 0x01, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00};
constexpr std::size_t kTsPacket = 188;

std::uint16_t bcc16(const std::uint8_t* p, std::size_t n) {
    std::uint16_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum = static_cast<std::uint16_t>(sum + (p[i] << (8 * ((i + 1) % 2))));
    return sum;
}

std::uint64_t unsignedValue(const std::uint8_t* v, std::size_t n) {
    std::uint64_t r = 0;
    for (std::size_t i = 0; i < n && i < 8; ++i) r = (r << 8) | v[i];
    return r;
}

double mapUnsigned(const std::uint8_t* v, std::size_t n, double lo, double hi) {
    if (n == 0 || n > 8) return UasMetadata::kNone;
    double maxRaw = std::ldexp(1.0, static_cast<int>(8 * n)) - 1.0;
    return lo + static_cast<double>(unsignedValue(v, n)) / maxRaw * (hi - lo);
}

double mapSigned(const std::uint8_t* v, std::size_t n, double halfRange) {
    if (n == 0 || n > 8) return UasMetadata::kNone;
    std::uint64_t raw = unsignedValue(v, n);
    int bits = static_cast<int>(8 * n);
    std::uint64_t sign = 1ull << (bits - 1);
    if (raw == sign) return UasMetadata::kNone;
    std::int64_t value = (raw & sign) ? static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(bits == 64 ? 0 : (1ull << bits)) : static_cast<std::int64_t>(raw);
    return static_cast<double>(value) / (std::ldexp(1.0, bits - 1) - 1.0) * halfRange;
}

void apply(UasMetadata& m, std::uint32_t tag, const std::uint8_t* v, std::size_t n) {
    auto text = [&]() { return std::string(reinterpret_cast<const char*>(v), n); };
    switch (tag) {
    case 2: m.time = static_cast<double>(unsignedValue(v, n)) / 1.0e6; break;
    case 3: m.missionId = text(); break;
    case 4: m.platformTail = text(); break;
    case 5: m.platformHeading = mapUnsigned(v, n, 0, 360); break;
    case 6: m.platformPitch = mapSigned(v, n, 20); break;
    case 7: m.platformRoll = mapSigned(v, n, 50); break;
    case 10: m.platformDesignation = text(); break;
    case 11: m.imageSensor = text(); break;
    case 12: m.imageCoordinateSystem = text(); break;
    case 13: m.sensorLat = mapSigned(v, n, 90); break;
    case 14: m.sensorLon = mapSigned(v, n, 180); break;
    case 15: m.sensorAlt = mapUnsigned(v, n, -900, 19000); break;
    case 16: m.hfov = mapUnsigned(v, n, 0, 180); break;
    case 17: m.vfov = mapUnsigned(v, n, 0, 180); break;
    case 18: m.sensorRelAzimuth = mapUnsigned(v, n, 0, 360); break;
    case 19: m.sensorRelElevation = mapSigned(v, n, 180); break;
    case 20: m.sensorRelRoll = mapUnsigned(v, n, 0, 360); break;
    case 21: m.slantRange = mapUnsigned(v, n, 0, 5000000); break;
    case 22: m.targetWidth = mapUnsigned(v, n, 0, 10000); break;
    case 23: m.frameCenterLat = mapSigned(v, n, 90); break;
    case 24: m.frameCenterLon = mapSigned(v, n, 180); break;
    case 25: m.frameCenterElevation = mapUnsigned(v, n, -900, 19000); break;
    case 65: m.version = n ? v[0] : 0; break;
    default:
        if (tag >= 26 && tag <= 33) {
            double offset = mapSigned(v, n, 0.075);
            int corner = (tag - 26) / 2;
            if ((tag - 26) % 2 == 0) m.cornerLat[corner] = m.frameCenterLat + offset;
            else m.cornerLon[corner] = m.frameCenterLon + offset;
        } else if (tag >= 82 && tag <= 89) {
            int corner = (tag - 82) / 2;
            if ((tag - 82) % 2 == 0) m.cornerLat[corner] = mapSigned(v, n, 90);
            else m.cornerLon[corner] = mapSigned(v, n, 180);
        }
        break;
    }
}

bool parsePacket(const std::uint8_t* p, std::size_t size, std::size_t& consumed, UasMetadata& out) {
    auto packet = misb::st0601::Packet::parse(p, size, nullptr, &consumed);
    if (!packet) return false;
    UasMetadata m;
    m.checksumOk = true;
    misb::klv::Items items = packet->items();
    for (const auto& item : items)
        if (item.tag == 23 || item.tag == 24) apply(m, item.tag, item.value.data(), item.value.size());
    for (const auto& item : items)
        if (item.tag != 23 && item.tag != 24) apply(m, item.tag, item.value.data(), item.value.size());
    if (items.error() != misb::Error::none) return false;
    out = std::move(m);
    return true;
}

void putBer(std::vector<std::uint8_t>& out, std::size_t n) {
    if (n < 0x80) {
        out.push_back(static_cast<std::uint8_t>(n));
        return;
    }
    int bytes = n < 0x100 ? 1 : n < 0x10000 ? 2 : 4;
    out.push_back(static_cast<std::uint8_t>(0x80 | bytes));
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
}

void putItem(std::vector<std::uint8_t>& out, std::uint8_t tag, const std::vector<std::uint8_t>& value) {
    out.push_back(tag);
    putBer(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> bigEndian(std::uint64_t v, int bytes) {
    std::vector<std::uint8_t> out(static_cast<size_t>(bytes));
    for (int i = 0; i < bytes; ++i) out[static_cast<size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * (bytes - 1 - i)));
    return out;
}

std::vector<std::uint8_t> encodeUnsigned(double value, int bytes, double lo, double hi) {
    double maxRaw = std::ldexp(1.0, 8 * bytes) - 1.0;
    double raw = std::round((std::clamp(value, lo, hi) - lo) / (hi - lo) * maxRaw);
    return bigEndian(static_cast<std::uint64_t>(raw), bytes);
}

std::vector<std::uint8_t> encodeSigned(double value, int bytes, double halfRange) {
    double maxRaw = std::ldexp(1.0, 8 * bytes - 1) - 1.0;
    std::int64_t raw = static_cast<std::int64_t>(std::round(std::clamp(value, -halfRange, halfRange) / halfRange * maxRaw));
    return bigEndian(static_cast<std::uint64_t>(raw), bytes);
}

struct Mat3 {
    double m[3][3];
};

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

Mat3 rotX(double a) {
    double c = std::cos(a), s = std::sin(a);
    return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
}

Mat3 rotY(double a) {
    double c = std::cos(a), s = std::sin(a);
    return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
}

Mat3 rotZ(double a) {
    double c = std::cos(a), s = std::sin(a);
    return {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
}

double orZero(double v) { return std::isfinite(v) ? v : 0.0; }

void scanKlv(const std::uint8_t* data, std::size_t size, std::vector<UasMetadata>& out) {
    std::size_t i = 0;
    while (i + 18 <= size) {
        const void* hit = std::memchr(data + i, kUasKey[0], size - i);
        if (!hit) break;
        i = static_cast<const std::uint8_t*>(hit) - data;
        std::size_t consumed = 0;
        UasMetadata m;
        if (i + 16 <= size && std::memcmp(data + i, kUasKey, 16) == 0 && parsePacket(data + i, size - i, consumed, m)) {
            out.push_back(std::move(m));
            i += consumed;
        } else {
            ++i;
        }
    }
}

class TsDemuxer {
public:
    void feed(const std::uint8_t* data, std::size_t size, std::vector<UasMetadata>& out) {
        std::size_t i = 0;
        if (!pending_.empty()) {
            std::size_t need = kTsPacket - pending_.size();
            std::size_t take = std::min(need, size);
            pending_.insert(pending_.end(), data, data + take);
            i = take;
            if (pending_.size() == kTsPacket) {
                packet(pending_.data(), out);
                pending_.clear();
            }
        }
        while (i < size) {
            if (data[i] != 0x47) {
                ++i;
                continue;
            }
            if (size - i < kTsPacket) {
                pending_.assign(data + i, data + size);
                break;
            }
            packet(data + i, out);
            i += kTsPacket;
        }
    }

    void finish(std::vector<UasMetadata>& out) {
        for (auto& [pid, buf] : pes_) flush(buf, out);
    }

    bool sawTables() const { return !pmtPids_.empty(); }

private:
    void packet(const std::uint8_t* p, std::vector<UasMetadata>& out) {
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        bool start = (p[1] & 0x40) != 0;
        int afc = (p[3] >> 4) & 3;
        if (!(afc & 1)) return;
        std::size_t off = 4;
        if (afc & 2) off += 1 + static_cast<std::size_t>(p[4]);
        if (off >= kTsPacket) return;
        const std::uint8_t* payload = p + off;
        std::size_t len = kTsPacket - off;
        if (pid == 0 && start) {
            table(payload, len, true);
        } else if (pmtPids_.count(pid) && start) {
            table(payload, len, false);
        } else if (klvPids_.count(pid)) {
            std::vector<std::uint8_t>& buf = pes_[pid];
            if (start) {
                flush(buf, out);
                buf.assign(payload, payload + len);
            } else if (!buf.empty()) {
                buf.insert(buf.end(), payload, payload + len);
            }
        }
    }

    void table(const std::uint8_t* payload, std::size_t len, bool pat) {
        std::size_t s = 1 + static_cast<std::size_t>(payload[0]);
        if (s + 8 > len) return;
        const std::uint8_t* t = payload + s;
        std::size_t sectionLength = ((t[1] & 0x0F) << 8) | t[2];
        std::size_t end = std::min(len - s, 3 + sectionLength);
        if (end < 4) return;
        end -= 4;
        if (pat && t[0] == 0x00) {
            for (std::size_t k = 8; k + 4 <= end; k += 4) {
                int program = (t[k] << 8) | t[k + 1];
                int pid = ((t[k + 2] & 0x1F) << 8) | t[k + 3];
                if (program != 0) pmtPids_.insert(pid);
            }
        } else if (!pat && t[0] == 0x02) {
            if (12 > end) return;
            std::size_t info = ((t[10] & 0x0F) << 8) | t[11];
            for (std::size_t k = 12 + info; k + 5 <= end;) {
                int type = t[k];
                int pid = ((t[k + 1] & 0x1F) << 8) | t[k + 2];
                std::size_t esInfo = ((t[k + 3] & 0x0F) << 8) | t[k + 4];
                bool klva = false;
                for (std::size_t d = k + 5; d + 2 <= k + 5 + esInfo && d + 2 <= end; d += 2 + t[d + 1])
                    if (t[d] == 0x05 && t[d + 1] >= 4 && std::memcmp(t + d + 2, "KLVA", 4) == 0) klva = true;
                if (type == 0x15 || (type == 0x06 && klva)) klvPids_.insert(pid);
                k += 5 + esInfo;
            }
        }
    }

    void flush(std::vector<std::uint8_t>& buf, std::vector<UasMetadata>& out) {
        if (buf.size() >= 9 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1) {
            std::size_t off = 9 + static_cast<std::size_t>(buf[8]);
            if (buf[3] == 0xBF || buf[3] == 0xBE) off = 6;
            if (off < buf.size()) scanKlv(buf.data() + off, buf.size() - off, out);
        }
        buf.clear();
    }

    std::vector<std::uint8_t> pending_;
    std::set<int> pmtPids_;
    std::set<int> klvPids_;
    std::map<int, std::vector<std::uint8_t>> pes_;
};

bool looksLikeTs(const std::uint8_t* data, std::size_t size) {
    return size >= kTsPacket * 3 && data[0] == 0x47 && data[kTsPacket] == 0x47 && data[2 * kTsPacket] == 0x47;
}

Coord coord(double lat, double lon, double alt) {
    Coord c;
    c.lat = lat;
    c.lon = lon;
    c.alt = alt;
    return c;
}

}

std::vector<UasMetadata> parseUasKlv(const std::uint8_t* data, std::size_t size) {
    std::vector<UasMetadata> out;
    if (data) scanKlv(data, size, out);
    return out;
}

std::vector<UasMetadata> demuxUasMetadata(const std::uint8_t* data, std::size_t size) {
    std::vector<UasMetadata> out;
    if (!data) return out;
    if (!looksLikeTs(data, size)) return parseUasKlv(data, size);
    TsDemuxer first;
    first.feed(data, size, out);
    first.finish(out);
    return out;
}

std::vector<UasMetadata> readUasMetadata(const std::string& path) {
    std::vector<UasMetadata> out;
#if defined(_WIN32)
    FILE* f = _wfopen(std::filesystem::u8path(path).wstring().c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return out;
    std::vector<std::uint8_t> chunk(kTsPacket * 4096);
    std::size_t n = std::fread(chunk.data(), 1, chunk.size(), f);
    if (!looksLikeTs(chunk.data(), n)) {
        std::vector<std::uint8_t> all(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0) all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        std::fclose(f);
        return parseUasKlv(all.data(), all.size());
    }
    TsDemuxer demux;
    do {
        demux.feed(chunk.data(), n, out);
    } while ((n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0);
    demux.finish(out);
    std::fclose(f);
    return out;
}

std::vector<std::uint8_t> encodeUasKlv(const UasMetadata& m) {
    std::vector<std::uint8_t> body;
    auto num = [&](std::uint8_t tag, double v, std::vector<std::uint8_t> bytes) {
        if (std::isfinite(v)) putItem(body, tag, bytes);
    };
    if (std::isfinite(m.time)) putItem(body, 2, bigEndian(static_cast<std::uint64_t>(std::llround(m.time * 1.0e6)), 8));
    auto str = [&](std::uint8_t tag, const std::string& s) {
        if (!s.empty()) putItem(body, tag, std::vector<std::uint8_t>(s.begin(), s.end()));
    };
    str(3, m.missionId);
    str(4, m.platformTail);
    num(5, m.platformHeading, encodeUnsigned(m.platformHeading, 2, 0, 360));
    num(6, m.platformPitch, encodeSigned(m.platformPitch, 2, 20));
    num(7, m.platformRoll, encodeSigned(m.platformRoll, 2, 50));
    str(10, m.platformDesignation);
    str(11, m.imageSensor);
    str(12, m.imageCoordinateSystem);
    num(13, m.sensorLat, encodeSigned(m.sensorLat, 4, 90));
    num(14, m.sensorLon, encodeSigned(m.sensorLon, 4, 180));
    num(15, m.sensorAlt, encodeUnsigned(m.sensorAlt, 2, -900, 19000));
    num(16, m.hfov, encodeUnsigned(m.hfov, 2, 0, 180));
    num(17, m.vfov, encodeUnsigned(m.vfov, 2, 0, 180));
    num(18, m.sensorRelAzimuth, encodeUnsigned(m.sensorRelAzimuth, 4, 0, 360));
    num(19, m.sensorRelElevation, encodeSigned(m.sensorRelElevation, 4, 180));
    num(20, m.sensorRelRoll, encodeUnsigned(m.sensorRelRoll, 4, 0, 360));
    num(21, m.slantRange, encodeUnsigned(m.slantRange, 4, 0, 5000000));
    num(22, m.targetWidth, encodeUnsigned(m.targetWidth, 2, 0, 10000));
    num(23, m.frameCenterLat, encodeSigned(m.frameCenterLat, 4, 90));
    num(24, m.frameCenterLon, encodeSigned(m.frameCenterLon, 4, 180));
    num(25, m.frameCenterElevation, encodeUnsigned(m.frameCenterElevation, 2, -900, 19000));
    for (int c = 0; c < 4; ++c) {
        num(static_cast<std::uint8_t>(82 + 2 * c), m.cornerLat[c], encodeSigned(m.cornerLat[c], 4, 90));
        num(static_cast<std::uint8_t>(83 + 2 * c), m.cornerLon[c], encodeSigned(m.cornerLon[c], 4, 180));
    }
    putItem(body, 65, {static_cast<std::uint8_t>(m.version > 0 ? m.version : 17)});
    std::vector<std::uint8_t> out(kUasKey, kUasKey + 16);
    putBer(out, body.size() + 4);
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(1);
    out.push_back(2);
    std::uint16_t sum = bcc16(out.data(), out.size());
    out.push_back(static_cast<std::uint8_t>(sum >> 8));
    out.push_back(static_cast<std::uint8_t>(sum & 0xFF));
    return out;
}

bool uasFootprint(const UasMetadata& m, double lat[4], double lon[4]) {
    bool corners = true;
    for (int c = 0; c < 4; ++c) corners = corners && std::isfinite(m.cornerLat[c]) && std::isfinite(m.cornerLon[c]);
    if (corners) {
        for (int c = 0; c < 4; ++c) {
            lat[c] = m.cornerLat[c];
            lon[c] = m.cornerLon[c];
        }
        return true;
    }
    if (!std::isfinite(m.sensorLat) || !std::isfinite(m.sensorLon) || !std::isfinite(m.sensorAlt) || !std::isfinite(m.hfov) ||
        !std::isfinite(m.vfov) || !std::isfinite(m.sensorRelElevation))
        return false;
    Mat3 platform = mul(mul(rotZ(orZero(m.platformHeading) * kDegToRad), rotY(orZero(m.platformPitch) * kDegToRad)), rotX(orZero(m.platformRoll) * kDegToRad));
    Mat3 sensor = mul(mul(rotZ(orZero(m.sensorRelAzimuth) * kDegToRad), rotY(m.sensorRelElevation * kDegToRad)), rotX(orZero(m.sensorRelRoll) * kDegToRad));
    Mat3 r = mul(platform, sensor);
    Vec3 origin = geodeticToEcef(m.sensorLat, m.sensorLon, m.sensorAlt);
    Vec3 e, n, u;
    enuFrame(m.sensorLat, m.sensorLon, e, n, u);
    double th = std::tan(m.hfov * 0.5 * kDegToRad), tv = std::tan(m.vfov * 0.5 * kDegToRad);
    const double sy[4] = {-1, 1, 1, -1}, sz[4] = {-1, -1, 1, 1};
    double ground = std::isfinite(m.frameCenterElevation) ? m.frameCenterElevation : 0.0;
    for (int c = 0; c < 4; ++c) {
        double v[3] = {1.0, sy[c] * th, sz[c] * tv};
        double d[3] = {0, 0, 0};
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) d[i] += r.m[i][k] * v[k];
        Vec3 dir = normalize(n * d[0] + e * d[1] - u * d[2]);
        double t = 0;
        if (!rayEllipsoid(origin, dir, ground, t) || t <= 0) return false;
        GeoPoint g = ecefToGeodetic(origin + dir * t);
        lat[c] = g.lat;
        lon[c] = g.lon;
    }
    return true;
}

std::vector<Feature> uasFeatures(const std::vector<UasMetadata>& packets, int footprintEvery) {
    std::vector<Feature> out;
    if (packets.empty()) return out;
    const UasMetadata& last = packets.back();
    std::string platform = !last.platformTail.empty() ? last.platformTail : !last.missionId.empty() ? last.missionId : "UAS";
    Feature track;
    track.name = platform + " track";
    track.type = "uas-sensor-track";
    track.altitudeMode = AltitudeMode::Absolute;
    track.style.strokeColor = 0xFF00E5FF;
    track.style.showLabel = false;
    Geometry line;
    line.type = GeometryType::LineString;
    Feature centers;
    centers.name = platform + " frame center";
    centers.type = "uas-frame-center-track";
    centers.style.strokeColor = 0xFFFFD400;
    centers.style.showLabel = false;
    Geometry centerLine;
    centerLine.type = GeometryType::LineString;
    int every = std::max(1, footprintEvery);
    for (size_t i = 0; i < packets.size(); ++i) {
        const UasMetadata& m = packets[i];
        if (std::isfinite(m.sensorLat) && std::isfinite(m.sensorLon)) line.points.push_back(coord(m.sensorLat, m.sensorLon, orZero(m.sensorAlt)));
        if (std::isfinite(m.frameCenterLat) && std::isfinite(m.frameCenterLon)) centerLine.points.push_back(coord(m.frameCenterLat, m.frameCenterLon, 0));
        double la[4], lo[4];
        bool lastPacket = i + 1 == packets.size();
        if ((i % static_cast<size_t>(every) == 0 || lastPacket) && uasFootprint(m, la, lo)) {
            Feature fp;
            fp.name = lastPacket ? platform + " footprint" : std::string();
            fp.type = "uas-footprint";
            fp.time = orZero(m.time);
            fp.style.strokeColor = lastPacket ? 0xFFFF5000 : 0x80FF8C00;
            fp.style.fillColor = lastPacket ? 0x50FF5000 : 0x20FF8C00;
            fp.style.showLabel = lastPacket;
            Geometry poly;
            poly.type = GeometryType::Polygon;
            for (int c = 0; c < 4; ++c) poly.points.push_back(coord(la[c], lo[c], 0));
            poly.points.push_back(poly.points.front());
            fp.geometries.push_back(std::move(poly));
            out.push_back(std::move(fp));
        }
    }
    if (line.points.size() >= 2) {
        track.geometries.push_back(std::move(line));
        out.push_back(std::move(track));
    }
    if (centerLine.points.size() >= 2) {
        centers.geometries.push_back(std::move(centerLine));
        out.push_back(std::move(centers));
    }
    if (std::isfinite(last.sensorLat) && std::isfinite(last.sensorLon)) {
        Feature sensor;
        sensor.name = platform;
        sensor.type = "a-f-A-M-F-Q";
        sensor.time = orZero(last.time);
        sensor.altitudeMode = AltitudeMode::Absolute;
        Geometry pt;
        pt.type = GeometryType::Point;
        pt.points.push_back(coord(last.sensorLat, last.sensorLon, orZero(last.sensorAlt)));
        sensor.geometries.push_back(std::move(pt));
        if (!last.missionId.empty()) sensor.attributes.push_back({"mission", last.missionId});
        if (!last.imageSensor.empty()) sensor.attributes.push_back({"sensor", last.imageSensor});
        out.push_back(std::move(sensor));
        if (std::isfinite(last.frameCenterLat) && std::isfinite(last.frameCenterLon)) {
            Feature los;
            los.name = platform + " line of sight";
            los.type = "uas-line-of-sight";
            los.altitudeMode = AltitudeMode::Absolute;
            los.style.strokeColor = 0xFFFF2020;
            los.style.showLabel = false;
            Geometry l;
            l.type = GeometryType::LineString;
            l.points.push_back(coord(last.sensorLat, last.sensorLon, orZero(last.sensorAlt)));
            l.points.push_back(coord(last.frameCenterLat, last.frameCenterLon, orZero(last.frameCenterElevation)));
            los.geometries.push_back(std::move(l));
            out.push_back(std::move(los));
        }
    }
    return out;
}

}
