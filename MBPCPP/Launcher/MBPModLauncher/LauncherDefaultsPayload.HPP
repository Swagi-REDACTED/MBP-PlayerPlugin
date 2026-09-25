#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace launcher {
enum class SubtitleDefaultMode : std::uint8_t { Off, Language };
enum class QualityDefaultMode : std::uint8_t { Auto, TargetHeight };
enum class ServerDefaultMode : std::uint8_t { Auto, Preferred };
struct PlayerDefaults {
    SubtitleDefaultMode subtitleMode = SubtitleDefaultMode::Off;
    std::string subtitleLanguage;
    QualityDefaultMode qualityMode = QualityDefaultMode::Auto;
    std::uint16_t targetQualityHeight = 0;
    ServerDefaultMode serverMode = ServerDefaultMode::Auto;
    std::string serverPreference;
    std::uint8_t volume = 100;
};
struct LauncherSettingsData {
    std::string movieBoxPathUtf8;
    PlayerDefaults playerDefaults;
    bool legacyPathOnly = false;
};
bool EncodeLdef(const LauncherSettingsData&, std::string& out, std::string* error=nullptr);
bool DecodeLdef(std::string_view bytes, LauncherSettingsData& out, std::string* error=nullptr);
}
