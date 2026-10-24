#pragma once
// WAV in and out for the prototype: PCM 16, 24 or 32 bit, or 32-bit float (WAVE_FORMAT_EXTENSIBLE
// too), mono or stereo (more channels: the first two). Out: 24-bit PCM.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mfw {

struct Audio {
    int rate = 0;
    std::vector<float> L, R;
};

inline uint32_t u32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
inline uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// False with `err` set if the file can't be read.
inline bool read(const std::string& path, Audio& a, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "can't open " + path;
        return false;
    }
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + got);
    std::fclose(f);
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        err = path + " is not a WAV file";
        return false;
    }
    int format = 0, channels = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t len = u32(&d[p + 4]);
        const unsigned char* body = &d[p + 8];
        const size_t avail = std::min<size_t>(len, d.size() - (p + 8));
        if (!std::memcmp(&d[p], "fmt ", 4) && avail >= 16) {
            format = u16(body);
            channels = u16(body + 2);
            a.rate = static_cast<int>(u32(body + 4));
            bits = u16(body + 14);
            if (format == 0xFFFE && avail >= 26) format = u16(body + 24);   // the subformat's first two bytes
        } else if (!std::memcmp(&d[p], "data", 4)) {
            data = body;
            dataLen = avail;
        }
        p += 8 + len + (len & 1);
    }
    if (!data || channels < 1) {
        err = path + ": no audio found";
        return false;
    }
    const bool pcm = format == 1 && (bits == 16 || bits == 24 || bits == 32), flt = format == 3 && bits == 32;
    if (!pcm && !flt) {
        err = path + ": PCM 16/24/32 or float 32 only (format " + std::to_string(format) + ", " + std::to_string(bits) + " bit)";
        return false;
    }
    const int bytes = bits / 8, frame = bytes * channels;
    const size_t n = dataLen / static_cast<size_t>(frame);
    a.L.resize(n);
    a.R.resize(n);
    auto sample = [&](const unsigned char* q) -> float {
        if (flt) {
            float v;
            std::memcpy(&v, q, 4);
            return v;
        }
        if (bits == 16) return static_cast<int16_t>(u16(q)) / 32768.0f;
        if (bits == 24) {   // into the top three bytes, then an arithmetic shift down: the sign comes along
            const uint32_t u = (static_cast<uint32_t>(q[0]) << 8) | (static_cast<uint32_t>(q[1]) << 16) | (static_cast<uint32_t>(q[2]) << 24);
            return static_cast<float>(static_cast<int32_t>(u) >> 8) / 8388608.0f;
        }
        return static_cast<float>(static_cast<int32_t>(u32(q)) / 2147483648.0);
    };
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* q = data + i * static_cast<size_t>(frame);
        a.L[i] = sample(q);
        a.R[i] = channels > 1 ? sample(q + bytes) : a.L[i];
    }
    return true;
}

inline bool write24(const std::string& path, const Audio& a, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        err = "can't write " + path;
        return false;
    }
    const uint32_t n = static_cast<uint32_t>(a.L.size()), dataLen = n * 6;
    auto put32 = [&](uint32_t v) {
        const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v >> 16),
                                    static_cast<unsigned char>(v >> 24)};
        std::fwrite(b, 1, 4, f);
    };
    auto put16 = [&](uint16_t v) {
        const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
        std::fwrite(b, 1, 2, f);
    };
    std::fwrite("RIFF", 1, 4, f);
    put32(36 + dataLen);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(16);
    put16(1);
    put16(2);
    put32(static_cast<uint32_t>(a.rate));
    put32(static_cast<uint32_t>(a.rate) * 6);
    put16(6);
    put16(24);
    std::fwrite("data", 1, 4, f);
    put32(dataLen);
    std::vector<unsigned char> out(dataLen);
    for (uint32_t i = 0; i < n; ++i) {
        const float s[2] = {a.L[i], a.R[i]};
        for (int c = 0; c < 2; ++c) {
            const double v = std::fmax(-1.0, std::fmin(1.0, static_cast<double>(s[c])));
            const int32_t q = static_cast<int32_t>(std::lrint(v * 8388607.0));
            unsigned char* o = &out[static_cast<size_t>(i) * 6 + static_cast<size_t>(c) * 3];
            o[0] = static_cast<unsigned char>(q);
            o[1] = static_cast<unsigned char>(q >> 8);
            o[2] = static_cast<unsigned char>(q >> 16);
        }
    }
    std::fwrite(out.data(), 1, out.size(), f);
    const bool ok = std::fclose(f) == 0;
    if (!ok) err = "writing " + path + " failed";
    return ok;
}

} // namespace mfw
