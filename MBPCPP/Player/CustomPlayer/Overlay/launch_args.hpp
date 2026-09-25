#pragma once

#include <string>
#include <string_view>
#include <cwctype>

struct LaunchOptions {
    std::wstring uri;
    bool hasUri = false;
    std::wstring bridgeName;
    bool hasBridge = false;
    std::wstring userAgent;
    bool hasUserAgent = false;
    bool videoPlayerQuits = false;
    bool hasVPQ = false;
};

inline bool LaunchTokenEquals(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(static_cast<wint_t>(a[i])) !=
            std::towlower(static_cast<wint_t>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline bool ParseLaunchBool(std::wstring_view value, bool& outValue) {
    if (LaunchTokenEquals(value, L"true") || value == L"1") {
        outValue = true;
        return true;
    }
    if (LaunchTokenEquals(value, L"false") || value == L"0") {
        outValue = false;
        return true;
    }
    return false;
}

// Parses the lpCmdLine portion passed to wWinMain.
// Supported forms:
//   --uri "https://example.com/video.mp4"
//   --uri=https://example.com/video.mp4
//   --bridge "MovieBoxPlayerMod-123-..."
//   --bridge=MovieBoxPlayerMod-123-...
//   --user-agent "MovieBoxPro/..."
//   --user-agent=MovieBoxPro/...
//   --VPQ True
//   --VPQ=False
//
// VPQ means "Video Player Quits". When enabled, the player X closes the
// application instead of returning to the dashboard.
//
// Quoted values preserve spaces. Backslashes are treated literally so Windows
// paths such as C:\\Videos\\My Movie.mp4 work as expected.
inline LaunchOptions ParseLaunchOptions(std::wstring_view commandLine) {
    LaunchOptions options;

    const auto isSpace = [](wchar_t c) {
        return std::iswspace(static_cast<wint_t>(c)) != 0;
    };

    size_t i = 0;
    const size_t n = commandLine.size();

    auto skipWhitespace = [&]() {
        while (i < n && isSpace(commandLine[i])) ++i;
    };

    auto readToken = [&]() -> std::wstring {
        skipWhitespace();
        std::wstring token;
        bool quoted = false;

        while (i < n) {
            const wchar_t c = commandLine[i];
            if (c == L'"') {
                quoted = !quoted;
                ++i;
                continue;
            }
            if (!quoted && isSpace(c)) break;
            token.push_back(c);
            ++i;
        }

        skipWhitespace();
        return token;
    };

    while (i < n) {
        std::wstring token = readToken();
        if (token.empty()) continue;

        constexpr std::wstring_view kUriPrefix = L"--uri=";
        if (token.rfind(kUriPrefix.data(), 0) == 0) {
            options.uri = token.substr(kUriPrefix.size());
            options.hasUri = !options.uri.empty();
            continue;
        }

        if (LaunchTokenEquals(token, L"--uri")) {
            if (i < n) {
                options.uri = readToken();
                options.hasUri = !options.uri.empty();
            }
            continue;
        }

        constexpr std::wstring_view kBridgePrefix = L"--bridge=";
        if (token.size() >= kBridgePrefix.size() &&
            LaunchTokenEquals(std::wstring_view(token).substr(0, kBridgePrefix.size()), kBridgePrefix)) {
            options.bridgeName = token.substr(kBridgePrefix.size());
            options.hasBridge = !options.bridgeName.empty();
            continue;
        }

        if (LaunchTokenEquals(token, L"--bridge")) {
            if (i < n) {
                options.bridgeName = readToken();
                options.hasBridge = !options.bridgeName.empty();
            }
            continue;
        }

        constexpr std::wstring_view kUserAgentPrefix = L"--user-agent=";
        if (token.size() >= kUserAgentPrefix.size() &&
            LaunchTokenEquals(std::wstring_view(token).substr(0, kUserAgentPrefix.size()), kUserAgentPrefix)) {
            options.userAgent = token.substr(kUserAgentPrefix.size());
            options.hasUserAgent = !options.userAgent.empty();
            continue;
        }

        if (LaunchTokenEquals(token, L"--user-agent")) {
            if (i < n) {
                options.userAgent = readToken();
                options.hasUserAgent = !options.userAgent.empty();
            }
            continue;
        }

        constexpr std::wstring_view kVpqPrefix = L"--VPQ=";
        if (token.size() >= kVpqPrefix.size() &&
            LaunchTokenEquals(std::wstring_view(token).substr(0, kVpqPrefix.size()), kVpqPrefix)) {
            bool value = false;
            if (ParseLaunchBool(std::wstring_view(token).substr(kVpqPrefix.size()), value)) {
                options.videoPlayerQuits = value;
                options.hasVPQ = true;
            }
            continue;
        }

        if (LaunchTokenEquals(token, L"--VPQ")) {
            if (i < n) {
                const std::wstring valueToken = readToken();
                bool value = false;
                if (ParseLaunchBool(valueToken, value)) {
                    options.videoPlayerQuits = value;
                    options.hasVPQ = true;
                }
            }
            continue;
        }
    }

    return options;
}

// Backward-compatible helper retained for existing callers/tests.
inline bool ParseLaunchUri(std::wstring_view commandLine, std::wstring& outUri) {
    const LaunchOptions options = ParseLaunchOptions(commandLine);
    outUri = options.uri;
    return options.hasUri;
}
