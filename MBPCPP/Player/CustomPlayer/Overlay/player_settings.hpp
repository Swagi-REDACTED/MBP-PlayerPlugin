#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cock {

struct Preferences {
    bool subtitlePresent = false;
    bool subtitleOff = false;
    std::string subtitleLanguage;

    bool qualityPresent = false;
    bool qualityAuto = false;
    std::uint16_t targetQualityHeight = 0;

    bool serverPresent = false;
    bool serverAuto = false;
    std::string serverPreference;

    bool AnyPresent() const noexcept {
        return subtitlePresent || qualityPresent || serverPresent;
    }
};

struct Settings {
    std::string title;
    std::int16_t season = 0;
    std::int16_t episode = 0;
    std::uint8_t boxType = 0;
    bool muted = false;
    std::int16_t volume = 100;
    float playbackSpeed = 1.0f;
    float subtitleDelay = 0.0f;
    std::int32_t audioTrackIndex = -1;
    Preferences preferences;

    bool Matches(std::string_view otherTitle, std::int16_t otherSeason,
                 std::int16_t otherEpisode, std::uint8_t otherBoxType) const {
        return title == otherTitle && season == otherSeason && episode == otherEpisode && boxType == otherBoxType;
    }
};

namespace detail {

constexpr std::uint8_t kVersion = 0x01;
constexpr std::int16_t kMutedFlag = 1 << 0;
constexpr std::int16_t kRawTitleFlag = 1 << 1;

inline void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
}
inline void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xffu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xffu));
}
inline void WriteFloat(std::vector<std::uint8_t>& out, float v) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    WriteU32(out, bits);
}

inline bool ReadU16(const std::vector<std::uint8_t>& in, std::size_t& p, std::uint16_t& v) {
    if (p + 2 > in.size()) return false;
    v = static_cast<std::uint16_t>(in[p]) |
        (static_cast<std::uint16_t>(in[p + 1]) << 8);
    p += 2;
    return true;
}
inline bool ReadU32(const std::vector<std::uint8_t>& in, std::size_t& p, std::uint32_t& v) {
    if (p + 4 > in.size()) return false;
    v = static_cast<std::uint32_t>(in[p]) |
        (static_cast<std::uint32_t>(in[p + 1]) << 8) |
        (static_cast<std::uint32_t>(in[p + 2]) << 16) |
        (static_cast<std::uint32_t>(in[p + 3]) << 24);
    p += 4;
    return true;
}
inline bool ReadFloat(const std::vector<std::uint8_t>& in, std::size_t& p, float& v) {
    std::uint32_t bits = 0;
    if (!ReadU32(in, p, bits)) return false;
    std::memcpy(&v, &bits, sizeof(v));
    return true;
}

inline std::vector<std::uint8_t> EncodeRle(const std::string& raw) {
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() * 2);
    std::size_t i = 0;
    while (i < raw.size()) {
        const std::uint8_t value = static_cast<std::uint8_t>(raw[i]);
        std::size_t count = 1;
        while (i + count < raw.size() &&
               static_cast<std::uint8_t>(raw[i + count]) == value && count < 255) {
            ++count;
        }
        out.push_back(value);
        out.push_back(static_cast<std::uint8_t>(count));
        i += count;
    }
    return out;
}

inline bool DecodeRle(const std::vector<std::uint8_t>& encoded, std::string& out) {
    if ((encoded.size() & 1u) != 0u) return false;
    out.clear();
    for (std::size_t i = 0; i < encoded.size(); i += 2) {
        const std::uint8_t value = encoded[i];
        const std::uint8_t count = encoded[i + 1];
        if (count == 0) return false;
        out.append(static_cast<std::size_t>(count), static_cast<char>(value));
    }
    return true;
}

inline int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string PercentDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = HexValue(in[i + 1]);
            const int lo = HexValue(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

inline std::string Trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

inline std::string SafeBaseName(const std::string& videoUri, const std::string& title) {
    std::string candidate;
    try {
        std::string pathPart = videoUri;
        const std::size_t scheme = pathPart.find("://");
        if (scheme != std::string::npos) {
            const std::size_t slash = pathPart.find('/', scheme + 3);
            pathPart = slash == std::string::npos ? std::string{} : pathPart.substr(slash);
        }
        const std::size_t cut = pathPart.find_first_of("?#");
        if (cut != std::string::npos) pathPart.resize(cut);
        pathPart = PercentDecode(pathPart);
        std::replace(pathPart.begin(), pathPart.end(), '\\', '/');
        const std::size_t slash = pathPart.find_last_of('/');
        std::string leaf = slash == std::string::npos ? pathPart : pathPart.substr(slash + 1);
        const std::size_t dot = leaf.find_last_of('.');
        if (dot != std::string::npos && dot != 0) leaf.resize(dot);
        candidate = leaf;
    } catch (...) {
        candidate.clear();
    }
    if (Trim(candidate).empty()) candidate = title.empty() ? "video" : title;

    static constexpr std::string_view invalid = "<>:\"/\\|?*";
    for (char& c : candidate) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || invalid.find(c) != std::string_view::npos) c = '_';
    }
    candidate = Trim(candidate);
    if (candidate.size() > 96) candidate.resize(96);
    return candidate.empty() ? "video" : candidate;
}

// Small standalone SHA-256 used solely for the C#-compatible identity suffix.
inline std::array<std::uint8_t, 32> Sha256(std::string_view input) {
    static constexpr std::uint32_t K[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    auto rotr=[](std::uint32_t x, int n){ return (x>>n)|(x<<(32-n)); };
    std::vector<std::uint8_t> msg(input.begin(), input.end());
    const std::uint64_t bitLen = static_cast<std::uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u) msg.push_back(0);
    for (int i=7;i>=0;--i) msg.push_back(static_cast<std::uint8_t>((bitLen>>(i*8))&0xffu));

    std::uint32_t h0=0x6a09e667u,h1=0xbb67ae85u,h2=0x3c6ef372u,h3=0xa54ff53au;
    std::uint32_t h4=0x510e527fu,h5=0x9b05688cu,h6=0x1f83d9abu,h7=0x5be0cd19u;
    for (std::size_t off=0; off<msg.size(); off+=64) {
        std::uint32_t w[64]{};
        for (int i=0;i<16;++i) {
            const std::size_t p=off+static_cast<std::size_t>(i)*4u;
            w[i]=(static_cast<std::uint32_t>(msg[p])<<24)|(static_cast<std::uint32_t>(msg[p+1])<<16)|
                 (static_cast<std::uint32_t>(msg[p+2])<<8)|static_cast<std::uint32_t>(msg[p+3]);
        }
        for (int i=16;i<64;++i) {
            const std::uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            const std::uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i]=w[i-16]+s0+w[i-7]+s1;
        }
        std::uint32_t a=h0,b=h1,c=h2,d=h3,e=h4,f=h5,g=h6,h=h7;
        for (int i=0;i<64;++i) {
            const std::uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            const std::uint32_t ch=(e&f)^((~e)&g);
            const std::uint32_t temp1=h+S1+ch+K[i]+w[i];
            const std::uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            const std::uint32_t maj=(a&b)^(a&c)^(b&c);
            const std::uint32_t temp2=S0+maj;
            h=g;g=f;f=e;e=d+temp1;d=c;c=b;b=a;a=temp1+temp2;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;h5+=f;h6+=g;h7+=h;
    }
    std::array<std::uint8_t,32> out{};
    const std::uint32_t hs[8]={h0,h1,h2,h3,h4,h5,h6,h7};
    for (int i=0;i<8;++i) for (int j=0;j<4;++j) out[static_cast<std::size_t>(i)*4u+j]=static_cast<std::uint8_t>((hs[i]>>(24-j*8))&0xffu);
    return out;
}

inline std::filesystem::path SettingsDirectory() {
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
    return base / "MovieBoxPlayerMod" / "Settings";
}

inline std::string IdentitySuffix(const std::string& title, std::int16_t season, std::int16_t episode, std::uint8_t boxType) {
    const std::string identity = std::to_string(static_cast<unsigned int>(boxType)) + "|" + title + "|" +
        std::to_string(season) + "|" + std::to_string(episode);
    const auto hash = Sha256(identity);
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (int i=0;i<5;++i) ss << std::setw(2) << static_cast<unsigned int>(hash[static_cast<std::size_t>(i)]);
    return ss.str();
}

inline std::filesystem::path PrimaryPath(const std::string& uri, const std::string& title) {
    return SettingsDirectory() / (SafeBaseName(uri, title) + ".cock");
}
inline std::filesystem::path IdentityPath(const std::string& uri, const std::string& title, std::int16_t season, std::int16_t episode, std::uint8_t boxType) {
    return SettingsDirectory() / (SafeBaseName(uri, title) + "." + IdentitySuffix(title,season,episode,boxType) + ".cock");
}

} // namespace detail

inline std::vector<std::uint8_t> Encode(const Settings& settings) {
    const std::vector<std::uint8_t> rle = detail::EncodeRle(settings.title);
    const bool useRaw = rle.size() >= settings.title.size();
    std::int16_t flags = 0;
    if (settings.muted) flags |= detail::kMutedFlag;
    if (useRaw) flags |= detail::kRawTitleFlag;

    const auto volume = static_cast<std::int16_t>(std::clamp<int>(settings.volume, 0, 100));
    const float speed = std::isfinite(settings.playbackSpeed) ? std::clamp(settings.playbackSpeed, 0.25f, 4.0f) : 1.0f;
    const float delay = std::isfinite(settings.subtitleDelay) ? std::clamp(settings.subtitleDelay, -30.0f, 30.0f) : 0.0f;

    std::vector<std::uint8_t> out;
    const std::size_t payloadSize = useRaw ? settings.title.size() : rle.size();
    out.reserve(30 + payloadSize);
    out.insert(out.end(), {'C','O','C','K'});
    out.push_back(detail::kVersion);
    detail::WriteU16(out, static_cast<std::uint16_t>(settings.season));
    detail::WriteU16(out, static_cast<std::uint16_t>(settings.episode));
    detail::WriteU16(out, static_cast<std::uint16_t>(flags));
    detail::WriteU16(out, static_cast<std::uint16_t>(volume));
    detail::WriteFloat(out, speed);
    detail::WriteFloat(out, delay);
    detail::WriteU32(out, static_cast<std::uint32_t>(payloadSize));
    if (useRaw) out.insert(out.end(), settings.title.begin(), settings.title.end());
    else out.insert(out.end(), rle.begin(), rle.end());
    out.push_back(settings.boxType);
    detail::WriteU32(out, static_cast<std::uint32_t>(settings.audioTrackIndex));

    const Preferences& pref = settings.preferences;
    if (pref.AnyPresent() && pref.subtitleLanguage.size() <= 255u && pref.serverPreference.size() <= 255u) {
        std::uint8_t prefFlags = 0;
        if (pref.subtitlePresent) prefFlags |= 1u << 0;
        if (pref.qualityPresent) prefFlags |= 1u << 1;
        if (pref.serverPresent) prefFlags |= 1u << 2;
        if (pref.subtitlePresent && pref.subtitleOff) prefFlags |= 1u << 3;
        if (pref.qualityPresent && pref.qualityAuto) prefFlags |= 1u << 4;
        if (pref.serverPresent && pref.serverAuto) prefFlags |= 1u << 5;

        const std::string subtitle = (pref.subtitlePresent && !pref.subtitleOff) ? pref.subtitleLanguage : std::string{};
        const std::string server = (pref.serverPresent && !pref.serverAuto) ? pref.serverPreference : std::string{};
        if (subtitle.size() <= 255u && server.size() <= 255u) {
            out.insert(out.end(), {'P','R','E','F'});
            out.push_back(0x01u);
            out.push_back(prefFlags);
            detail::WriteU16(out, (pref.qualityPresent && !pref.qualityAuto) ? pref.targetQualityHeight : 0u);
            out.push_back(static_cast<std::uint8_t>(subtitle.size()));
            out.push_back(static_cast<std::uint8_t>(server.size()));
            out.insert(out.end(), subtitle.begin(), subtitle.end());
            out.insert(out.end(), server.begin(), server.end());
        }
    }
    return out;
}

inline bool Decode(const std::vector<std::uint8_t>& bytes, Settings& out) {
    if (bytes.size() < 25) return false;
    if (bytes[0] != 'C' || bytes[1] != 'O' || bytes[2] != 'C' || bytes[3] != 'K' || bytes[4] != detail::kVersion) return false;
    std::size_t p = 5;
    std::uint16_t season=0,episode=0,flags=0,volume=0;
    std::uint32_t titleLength=0;
    float speed=1.0f,delay=0.0f;
    if (!detail::ReadU16(bytes,p,season) || !detail::ReadU16(bytes,p,episode) ||
        !detail::ReadU16(bytes,p,flags) || !detail::ReadU16(bytes,p,volume) ||
        !detail::ReadFloat(bytes,p,speed) || !detail::ReadFloat(bytes,p,delay) ||
        !detail::ReadU32(bytes,p,titleLength)) return false;
    if (titleLength > bytes.size() - p) return false;
    std::vector<std::uint8_t> stored(bytes.begin()+static_cast<std::ptrdiff_t>(p), bytes.begin()+static_cast<std::ptrdiff_t>(p+titleLength));
    p += titleLength;
    std::string title;
    if ((flags & detail::kRawTitleFlag) != 0) title.assign(stored.begin(), stored.end());
    else if (!detail::DecodeRle(stored,title)) return false;

    std::uint8_t boxType = (season != 0 || episode != 0) ? 2u : 1u;
    std::int32_t audioTrack = -1;
    if (p < bytes.size()) boxType = bytes[p++];
    if (bytes.size() - p >= 4) {
        std::uint32_t raw=0;
        if (!detail::ReadU32(bytes,p,raw)) return false;
        audioTrack = static_cast<std::int32_t>(raw);
    }

    out.title = std::move(title);
    out.season = static_cast<std::int16_t>(season);
    out.episode = static_cast<std::int16_t>(episode);
    out.boxType = boxType;
    out.muted = (flags & detail::kMutedFlag) != 0;
    out.volume = static_cast<std::int16_t>(std::clamp<int>(static_cast<int>(static_cast<std::int16_t>(volume)),0,100));
    out.playbackSpeed = std::isfinite(speed) ? std::clamp(speed,0.25f,4.0f) : 1.0f;
    out.subtitleDelay = std::isfinite(delay) ? std::clamp(delay,-30.0f,30.0f) : 0.0f;
    out.audioTrackIndex = audioTrack;
    out.preferences = {};

    // PREF is an optional trailing extension. Any malformed or unsupported
    // extension is ignored without invalidating the otherwise valid v1 base.
    if (bytes.size() - p >= 4 && bytes[p] == 'P' && bytes[p + 1] == 'R' && bytes[p + 2] == 'E' && bytes[p + 3] == 'F') {
        Preferences pref{};
        std::size_t q = p + 4;
        bool prefValid = true;
        if (q + 6 > bytes.size()) prefValid = false;
        if (prefValid) {
            const std::uint8_t version = bytes[q++];
            const std::uint8_t prefFlags = bytes[q++];
            std::uint16_t quality = 0;
            if (version != 0x01u || (prefFlags & 0xC0u) != 0u || !detail::ReadU16(bytes, q, quality)) {
                prefValid = false;
            } else if (q + 2 > bytes.size()) {
                prefValid = false;
            } else {
                const std::size_t subtitleLen = bytes[q++];
                const std::size_t serverLen = bytes[q++];
                if (q + subtitleLen + serverLen != bytes.size()) {
                    prefValid = false;
                } else {
                    pref.subtitlePresent = (prefFlags & (1u << 0)) != 0;
                    pref.qualityPresent = (prefFlags & (1u << 1)) != 0;
                    pref.serverPresent = (prefFlags & (1u << 2)) != 0;
                    pref.subtitleOff = (prefFlags & (1u << 3)) != 0;
                    pref.qualityAuto = (prefFlags & (1u << 4)) != 0;
                    pref.serverAuto = (prefFlags & (1u << 5)) != 0;
                    pref.targetQualityHeight = quality;
                    pref.subtitleLanguage.assign(reinterpret_cast<const char*>(bytes.data() + q), subtitleLen);
                    q += subtitleLen;
                    pref.serverPreference.assign(reinterpret_cast<const char*>(bytes.data() + q), serverLen);

                    if ((!pref.subtitlePresent && (pref.subtitleOff || subtitleLen != 0u)) ||
                        (pref.subtitlePresent && pref.subtitleOff && subtitleLen != 0u) ||
                        (pref.subtitlePresent && !pref.subtitleOff && subtitleLen == 0u) ||
                        (!pref.qualityPresent && (pref.qualityAuto || quality != 0u)) ||
                        (pref.qualityPresent && pref.qualityAuto && quality != 0u) ||
                        (pref.qualityPresent && !pref.qualityAuto && quality == 0u) ||
                        (!pref.serverPresent && (pref.serverAuto || serverLen != 0u)) ||
                        (pref.serverPresent && pref.serverAuto && serverLen != 0u) ||
                        (pref.serverPresent && !pref.serverAuto && serverLen == 0u)) {
                        prefValid = false;
                    }
                }
            }
        }
        if (prefValid) out.preferences = std::move(pref);
    }
    return true;
}

inline std::optional<Settings> TryRead(const std::filesystem::path& path) {
    try {
        std::ifstream file(path, std::ios::binary);
        if (!file) return std::nullopt;
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Settings settings;
        if (!Decode(bytes, settings)) return std::nullopt;
        return settings;
    } catch (...) {
        return std::nullopt;
    }
}

inline std::optional<Settings> Load(const std::string& videoUri, const std::string& title,
                                    std::int16_t season, std::int16_t episode, std::uint8_t boxType) {
    const auto primary = TryRead(detail::PrimaryPath(videoUri,title));
    if (primary && primary->Matches(title,season,episode,boxType)) return primary;
    const auto fallback = TryRead(detail::IdentityPath(videoUri,title,season,episode,boxType));
    if (fallback && fallback->Matches(title,season,episode,boxType)) return fallback;
    return std::nullopt;
}

inline bool Save(const std::string& videoUri, const Settings& settings) {
    try {
        std::error_code ec;
        const auto dir = detail::SettingsDirectory();
        std::filesystem::create_directories(dir, ec);
        if (ec) return false;

        const auto primaryPath = detail::PrimaryPath(videoUri,settings.title);
        auto path = primaryPath;
        if (std::filesystem::exists(primaryPath,ec)) {
            const auto existing = TryRead(primaryPath);
            if (!existing || !existing->Matches(settings.title,settings.season,settings.episode,settings.boxType))
                path = detail::IdentityPath(videoUri,settings.title,settings.season,settings.episode,settings.boxType);
        }

        const auto bytes = Encode(settings);
        auto tempPath = path;
        tempPath += ".tmp";
        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            file.flush();
            if (!file) return false;
        }
#ifdef _WIN32
        if (!MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::filesystem::remove(tempPath,ec);
            return false;
        }
#else
        std::filesystem::remove(path,ec);
        ec.clear();
        std::filesystem::rename(tempPath,path,ec);
        if (ec) { std::filesystem::remove(tempPath,ec); return false; }
#endif
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace cock
