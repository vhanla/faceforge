#include "config.hpp"

#include "ff.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <sstream>

#if defined(_WIN32)
#define FF_WINDOWS 1
#else
#define FF_WINDOWS 0
#endif

namespace config {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// Minimal, tolerant reader for the flat settings object. Unknown keys and
// malformed values are ignored; the caller keeps defaults for whatever is
// missing. Values are normalized to strings plus a boolean for the type.
struct Scanned {
    std::map<std::string, std::string> values;
    std::map<std::string, bool> is_string;
};

bool scanJson(const std::string& text, Scanned& out) {
    size_t i = 0;
    const size_t n = text.size();
    auto skipWs = [&]() {
        while (i < n && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    };
    skipWs();
    if (i >= n || text[i] != '{') return false;
    ++i;
    while (true) {
        skipWs();
        if (i < n && text[i] == '}') break;
        if (i >= n || text[i] != '"') return false;
        // key
        ++i;
        std::string key;
        while (i < n && text[i] != '"') {
            if (text[i] == '\\' && i + 1 < n) ++i;
            key.push_back(text[i++]);
        }
        if (i >= n) return false;
        ++i; // closing quote
        skipWs();
        if (i >= n || text[i] != ':') return false;
        ++i;
        skipWs();
        if (i >= n) return false;
        std::string value;
        bool is_string = false;
        if (text[i] == '"') {
            is_string = true;
            ++i;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < n) {
                    ++i;
                    switch (text[i]) {
                        case 'n': value.push_back('\n'); break;
                        case 't': value.push_back('\t'); break;
                        case 'r': value.push_back('\r'); break;
                        default: value.push_back(text[i]); break;
                    }
                } else {
                    value.push_back(text[i]);
                }
                ++i;
            }
            if (i >= n) return false;
            ++i; // closing quote
        } else {
            while (i < n && text[i] != ',' && text[i] != '}') value.push_back(text[i++]);
            value = trim(value);
        }
        out.values[key] = value;
        out.is_string[key] = is_string;
        skipWs();
        if (i < n && text[i] == ',') {
            ++i;
            continue;
        }
        if (i < n && text[i] == '}') break;
        return false;
    }
    return true;
}

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace

const char* separator() { return FF_WINDOWS ? "\\" : "/"; }

std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const bool a_ends = a.back() == '/' || a.back() == '\\';
    return a_ends ? a + b : a + separator() + b;
}

std::string dirName(const std::string& path) {
    const size_t idx = path.find_last_of("/\\");
    if (idx == std::string::npos) return "";
    if (idx == 0) return path.substr(0, 1);
    return path.substr(0, idx);
}

std::string defaultPath() {
    return defaultPath(std::getenv("APPDATA"), std::getenv("USERPROFILE"),
                       std::getenv("XDG_CONFIG_HOME"), std::getenv("HOME"), FF_WINDOWS != 0);
}

std::string defaultPath(const char* appdata, const char* userprofile, const char* xdg,
                        const char* home, bool windows) {
    if (windows) {
        const char* base = appdata ? appdata : (userprofile ? userprofile : ".");
        return join(join(base, "faceforge"), "settings.json");
    }
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else {
        base = join(home ? home : ".", ".config");
    }
    return join(join(base, "faceforge"), "settings.json");
}

bool load(const std::string& path, Values& out) {
    out = Values{};
    if (path.empty()) return false;
    std::vector<uint8_t> bytes;
    if (!ff::readFile(path.c_str(), bytes)) return false;
    const std::string text(bytes.begin(), bytes.end());

    Scanned s;
    if (!scanJson(text, s)) return false;

    auto str = [&](const char* key) -> const std::string* {
        auto it = s.values.find(key);
        if (it == s.values.end() || !s.is_string[key]) return nullptr;
        return &it->second;
    };
    auto boolean = [&](const char* key) -> const bool* {
        auto it = s.values.find(key);
        if (it == s.values.end() || s.is_string[key]) return nullptr;
        static const bool t = true, f = false;
        if (it->second == "true") return &t;
        if (it->second == "false") return &f;
        return nullptr;
    };
    auto number = [&](const char* key, long long& v) -> bool {
        auto it = s.values.find(key);
        if (it == s.values.end() || s.is_string[key]) return false;
        char* end = nullptr;
        v = std::strtoll(it->second.c_str(), &end, 10);
        return end && *end == '\0';
    };

    if (const std::string* p = str("provider")) out.provider = providerFromName(*p);
    if (const std::string* p = str("models_dir")) out.models_dir = *p;
    if (const std::string* p = str("temp_dir")) out.temp_dir = *p;
    if (const std::string* p = str("export_dir")) out.export_dir = *p;
    if (const bool* b = boolean("unload_after_run")) out.unload_after_run = *b;
    if (const bool* b = boolean("check_updates")) out.check_updates = *b;
    if (const bool* b = boolean("acrylic")) out.acrylic = *b;
    long long lv = 0;
    if (number("update_interval_hours", lv)) {
        out.update_interval_hours =
            static_cast<uint32_t>(std::clamp<long long>(lv, 1, 24 * 365));
    }
    if (number("last_update_check", lv)) out.last_update_check = lv;
    return true;
}

bool save(const std::string& path, const Values& v) {
    if (path.empty()) return false;
    const std::string dir = dirName(path);
    if (!dir.empty()) ff::mkdirs(dir.c_str());

    std::ostringstream o;
    o << "{\n";
    o << "  \"version\": " << kFileVersion << ",\n";
    o << "  \"provider\": \"" << providerName(v.provider) << "\",\n";
    o << "  \"models_dir\": \"" << escape(v.models_dir) << "\",\n";
    o << "  \"temp_dir\": \"" << escape(v.temp_dir) << "\",\n";
    o << "  \"export_dir\": \"" << escape(v.export_dir) << "\",\n";
    o << "  \"unload_after_run\": " << (v.unload_after_run ? "true" : "false") << ",\n";
    o << "  \"check_updates\": " << (v.check_updates ? "true" : "false") << ",\n";
    o << "  \"acrylic\": " << (v.acrylic ? "true" : "false") << ",\n";
    o << "  \"update_interval_hours\": " << v.update_interval_hours << ",\n";
    o << "  \"last_update_check\": " << v.last_update_check << "\n";
    o << "}\n";

    const std::string text = o.str();
    return ff::writeFile(path.c_str(), reinterpret_cast<const uint8_t*>(text.data()),
                         text.size());
}

const char* providerName(uint8_t value) {
    switch (value) {
        case 1: return "cuda";
        case 2: return "cpu";
        default: return "auto";
    }
}

uint8_t providerFromName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower == "cuda" || lower == "gpu") return 1;
    if (lower == "cpu") return 2;
    return 0;
}

bool parseVersion(const std::string& text, uint32_t out[3]) {
    std::string s = trim(text);
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
    out[0] = out[1] = out[2] = 0;
    size_t start = 0;
    int i = 0;
    while (start <= s.size() && i < 3) {
        size_t dot = s.find('.', start);
        const size_t end = (dot == std::string::npos) ? s.size() : dot;
        std::string part = s.substr(start, end - start);
        const size_t cut = part.find_first_of("-+");
        if (cut != std::string::npos) part = part.substr(0, cut);
        char* e = nullptr;
        const long v = std::strtol(part.c_str(), &e, 10);
        if (e == part.c_str() || part.empty()) return i > 0;
        out[i] = static_cast<uint32_t>(v < 0 ? 0 : v);
        ++i;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return i > 0;
}

bool versionNewer(const std::string& candidate, const std::string& current) {
    uint32_t a[3], b[3];
    if (!parseVersion(candidate, a) || !parseVersion(current, b)) return false;
    for (int i = 0; i < 3; ++i) {
        if (a[i] != b[i]) return a[i] > b[i];
    }
    return false;
}

} // namespace config
