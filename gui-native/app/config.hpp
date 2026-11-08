#pragma once
// Persistent application settings. Direct port of gui/src/config.zig.
//
// Only rarely-modified options live here: execution provider, the folders for
// models, temporary files and exports, and update-check preferences.
// Precedence at startup is CLI > environment > config file > defaults.

#include <cstdint>
#include <string>

namespace config {

constexpr uint32_t kFileVersion = 1;

struct Values {
    uint8_t provider = 0; // 0 auto, 1 cuda, 2 cpu
    std::string models_dir = "models";
    std::string temp_dir;
    std::string export_dir;
    bool unload_after_run = false;
    bool check_updates = true;
    // Translucent, compositor-blurred window (acrylic). Only read at startup,
    // because the window is created before the rest of the settings apply.
    bool acrylic = true;
    uint32_t update_interval_hours = 24;
    int64_t last_update_check = 0;
};

const char* separator();
// Join two path fragments, avoiding a doubled separator when `a` already ends
// with one.
std::string join(const std::string& a, const std::string& b);
// Directory part of `path`, or "" when it has none.
std::string dirName(const std::string& path);

// Platform-specific location of the settings file. Reads the environment.
std::string defaultPath();
// Same, but with explicit environment values (used by tests).
std::string defaultPath(const char* appdata, const char* userprofile, const char* xdg,
                        const char* home, bool windows);

// Read the JSON file. Returns false when it is missing, unreadable or corrupt;
// the caller keeps the defaults in that case.
bool load(const std::string& path, Values& out);
// Write the settings file, creating its directory when needed.
bool save(const std::string& path, const Values& v);

const char* providerName(uint8_t value);
uint8_t providerFromName(const std::string& name);

// "1.2.3" / "v1.2.3" (and pre-release suffixes) to a comparable triple.
bool parseVersion(const std::string& text, uint32_t out[3]);
bool versionNewer(const std::string& candidate, const std::string& current);

} // namespace config
