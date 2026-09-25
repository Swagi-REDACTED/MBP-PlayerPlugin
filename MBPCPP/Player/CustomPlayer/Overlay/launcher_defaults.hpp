#pragma once

#include "player_settings.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace launcher_defaults {

struct Defaults {
    bool subtitleOff = true;
    std::string subtitleLanguage;
    bool qualityAuto = true;
    std::uint16_t targetQualityHeight = 0;
    bool serverAuto = true;
    std::string serverPreference;
    std::uint8_t volume = 100;
};

inline Defaults BuiltIn() { return {}; }

namespace detail {
inline std::uint16_t ReadU16(const std::string& b, std::size_t p) {
    return static_cast<std::uint16_t>(static_cast<unsigned char>(b[p])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(b[p + 1]) << 8);
}

inline std::filesystem::path SettingsPath() {
#ifdef _WIN32
    char* local = nullptr;
    std::size_t localLen = 0;
    const errno_t envResult = _dupenv_s(&local, &localLen, "LOCALAPPDATA");
    std::filesystem::path base = (envResult == 0 && local && *local)
        ? std::filesystem::path(local)
        : std::filesystem::temp_directory_path();
    std::free(local);
#else
    const char* local = std::getenv("LOCALAPPDATA");
    std::filesystem::path base = (local && *local)
        ? std::filesystem::path(local)
        : std::filesystem::temp_directory_path();
#endif
    return base / "MovieBoxPlayerMod" / "Launcher" / "Settings.cock";
}
}

inline Defaults DecodeFileBytes(const std::vector<std::uint8_t>& bytes) {
    Defaults defaults = BuiltIn();
    cock::Settings outer{};
    if (!cock::Decode(bytes, outer)) return defaults;

    const std::string& b = outer.title;
    if (b.size() < 4 || b.compare(0, 4, "LDEF") != 0) return defaults; // legacy path-only
    if (b.size() < 13 || static_cast<std::uint8_t>(b[4]) != 0x01u) return defaults;

    const std::uint8_t flags = static_cast<std::uint8_t>(b[5]);
    if ((flags & 0xF8u) != 0u) return defaults;
    const std::uint8_t volume = static_cast<std::uint8_t>(b[6]);
    const std::uint16_t quality = detail::ReadU16(b, 7);
    const std::uint16_t pathLength = detail::ReadU16(b, 9);
    const std::size_t languageLength = static_cast<std::uint8_t>(b[11]);
    const std::size_t serverLength = static_cast<std::uint8_t>(b[12]);
    const std::size_t expected = 13u + pathLength + languageLength + serverLength;
    if (expected != b.size()) return defaults;

    const bool subtitleOff = (flags & (1u << 0)) != 0;
    const bool qualityAuto = (flags & (1u << 1)) != 0;
    const bool serverAuto = (flags & (1u << 2)) != 0;
    if (subtitleOff != (languageLength == 0u) || qualityAuto != (quality == 0u) || serverAuto != (serverLength == 0u))
        return defaults;

    std::size_t p = 13u + pathLength;
    std::string language = b.substr(p, languageLength); p += languageLength;
    std::string server = b.substr(p, serverLength);

    defaults.subtitleOff = subtitleOff;
    defaults.subtitleLanguage = std::move(language);
    defaults.qualityAuto = qualityAuto;
    defaults.targetQualityHeight = quality;
    defaults.serverAuto = serverAuto;
    defaults.serverPreference = std::move(server);
    defaults.volume = static_cast<std::uint8_t>(std::min<unsigned int>(100u, volume));
    return defaults;
}

inline Defaults Load() {
    try {
        std::ifstream file(detail::SettingsPath(), std::ios::binary);
        if (!file) return BuiltIn();
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return DecodeFileBytes(bytes);
    } catch (...) {
        return BuiltIn();
    }
}

} // namespace launcher_defaults
