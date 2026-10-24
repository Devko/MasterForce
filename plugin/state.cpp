#include "state.h"

#include "master_map.h"

#include <charconv>
#include <cmath>
#include <cstring>
#include <vector>

namespace mf {
namespace {
constexpr const char* kMagic = "masterforce ";

bool saved(int i) { return PARAM_INFO[i].kind == Kind::Synth; }

bool learned(int i) {
    for (int k = 0; k < kNumLearned; ++k)
        if (kLearnedParam[k] == i) return true;
    return false;
}

// Numbers in the C locale whatever the process's is (a "0,5" would misread every preset).
std::string number(float v) {
    char b[32];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::general, 6);
    return std::string(b, r.ptr);
}

bool parse(const std::string& s, float& out) {
    size_t a = 0, e = s.size();
    while (a < e && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (e > a && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    if (a < e && s[a] == '+') ++a;
    float v = 0.0f;
    const auto r = std::from_chars(s.data() + a, s.data() + e, v);
    if (r.ec != std::errc() || r.ptr != s.data() + e || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// A parameter's value as a state line writes it: the real value, or the option's name.
std::string valueText(int i, float norm) {
    const float v = paramValue(i, norm);
    return PARAM_INFO[i].nopts > 0 ? std::string(PARAM_INFO[i].opts[static_cast<int>(v)]) : number(v);
}

// An option by its name ("Loud", "120 Hz") or its index; -1 if it is neither.
int option(int i, const std::string& val) {
    for (int o = 0; o < PARAM_INFO[i].nopts; ++o)
        if (val == PARAM_INFO[i].opts[o]) return o;
    float v = 0.0f;
    if (parse(val, v) && v == std::floor(v) && v >= 0.0f && v < static_cast<float>(PARAM_INFO[i].nopts))
        return static_cast<int>(v);
    return -1;
}

// A line's value as parameter i's 0..1, or -1 if it isn't one.
float normOf(int i, const std::string& val) {
    if (PARAM_INFO[i].nopts > 0) {
        const int o = option(i, val);
        return o >= 0 ? paramNorm(i, static_cast<float>(o)) : -1.0f;
    }
    float v = 0.0f;
    return parse(val, v) ? paramNorm(i, v) : -1.0f;
}

int savedParam(const std::string& key) {
    for (int i = 0; i < P_COUNT; ++i)
        if (saved(i) && key == PARAM_INFO[i].key) return i;
    return -1;
}

// The fields of a learn= line, split at `sep`.
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}

bool parseCount(const std::string& s, uint32_t& out) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}
} // namespace

std::string measurementText(const Measurement& m) {
    std::string out = "1;" + number(m.loudness) + ";" + number(m.truePeakDb) + ";";
    for (int b = 0; b < kBands; ++b) out += (b ? "," : "") + number(m.bandDb[b]);
    out += ";";
    bool first = true;
    for (int b = 0; b < kHistBins;) {
        int run = 0;
        while (b + run < kHistBins && m.glueHist[b + run] == 0) ++run;
        out += first ? "" : ",";
        first = false;
        if (run > 1) {
            out += "0*" + std::to_string(run);
            b += run;
        } else {
            out += std::to_string(m.glueHist[b]);
            ++b;
        }
    }
    return out;
}

bool parseMeasurement(const std::string& text, Measurement& m) {
    m = Measurement{};
    const std::vector<std::string> f = split(text, ';');
    if (f.size() != 5 || f[0] != "1") return false;
    Measurement r;
    if (!parse(f[1], r.loudness) || !parse(f[2], r.truePeakDb)) return false;
    const std::vector<std::string> bands = split(f[3], ',');
    if (static_cast<int>(bands.size()) != kBands) return false;
    for (int b = 0; b < kBands; ++b)
        if (!parse(bands[static_cast<size_t>(b)], r.bandDb[b])) return false;
    int at = 0;
    for (const std::string& item : split(f[4], ',')) {
        uint32_t v = 0, run = 1;
        const size_t star = item.find('*');
        if (star != std::string::npos) {
            if (!parseCount(item.substr(0, star), v) || v != 0 || !parseCount(item.substr(star + 1), run)) return false;
        } else if (!parseCount(item, v)) {
            return false;
        }
        if (run > static_cast<uint32_t>(kHistBins - at)) return false;
        for (uint32_t k = 0; k < run; ++k) r.glueHist[at++] = v;
    }
    if (at != kHistBins) return false;
    r.valid = r.loudness > kMinLoudness && r.loudness < 30.0f && r.truePeakDb < 60.0f;
    if (!r.valid) return false;
    m = r;
    return true;
}

bool isStateText(const std::string& text) {
    size_t at = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;   // a UTF-8 byte-order mark (a text editor's)
    if (text.compare(at, std::strlen(kMagic), kMagic) != 0) return false;
    at += std::strlen(kMagic);
    int version = 0;
    const auto r = std::from_chars(text.data() + at, text.data() + text.size(), version);
    return r.ec == std::errc() && version >= 1;
}

std::string saveState(const Surface& s, bool asPreset) {
    std::string out = std::string(kMagic) + std::to_string(kStateVersion) + "\n";
    for (int i = 0; i < P_COUNT; ++i) {
        if (!saved(i) || (asPreset && learned(i))) continue;   // a preset: how to master, not what LEARN set
        out += PARAM_INFO[i].key;
        out += '=';
        out += valueText(i, s.get(i));
        out += '\n';
    }
    if (!asPreset) {
        if (s.measurement().valid) out += "learn=" + measurementText(s.measurement()) + "\n";
        if (!s.presetKey().empty()) out += "preset=" + s.presetKey() + "\n";
    }
    return out;
}

bool loadState(Surface& s, const std::string& textIn, bool asPreset) {
    if (!isStateText(textIn)) return false;
    const std::string text = textIn.compare(0, 3, "\xEF\xBB\xBF") == 0 ? textIn.substr(3) : textIn;
    Measurement m;
    {
        Surface::Batch batch(s);   // the audio thread never plays a half-loaded sound
        if (asPreset)
            for (int i = 0; i < P_COUNT; ++i)
                if (saved(i)) s.setValue(i, PARAM_INFO[i].def);
        // A preset is complete (what it doesn't name is the default); a project changes only what it lists.
        std::string preset;
        size_t at = text.find('\n');
        while (at != std::string::npos && at + 1 < text.size()) {
            const size_t end = text.find('\n', at + 1);
            std::string line = text.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1);
            at = end;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == "preset") {
                preset = val;
                continue;
            }
            if (key == "learn") {
                if (!asPreset) parseMeasurement(val, m);
                continue;
            }
            const int i = savedParam(key);
            if (i < 0) continue;
            const float n = normOf(i, val);
            if (n < 0.0f) continue;
            s.setValue(i, n);
        }
        if (!asPreset) s.setPresetKey(preset);   // a project without one came from no preset
    }
    if (!asPreset) s.setMeasurement(m);   // the project's own, or none
    s.refresh();
    return true;
}

} // namespace mf
