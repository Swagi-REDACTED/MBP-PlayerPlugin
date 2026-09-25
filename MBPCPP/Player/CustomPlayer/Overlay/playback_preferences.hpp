#pragma once

#include "launcher_defaults.hpp"
#include "player_settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace playback_preferences {

enum class ProfileSource { Unknown, EpisodeCock, LauncherDefaults };

struct Choice {
    std::string id;
    std::string label;
    bool selected = false;
};

struct Decision {
    std::string action;
    std::string id;
};

inline std::string Trim(std::string s) {
    auto ws=[](unsigned char c){ return std::isspace(c)!=0; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
inline std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}
inline bool IEquals(std::string a, std::string b) { return Lower(Trim(std::move(a))) == Lower(Trim(std::move(b))); }
inline bool ContainsWordToken(const std::string& text, const std::string& token) {
    std::size_t pos = text.find(token);
    auto isWord=[](unsigned char c){ return std::isalnum(c)!=0 || c=='_'; };
    while (pos != std::string::npos) {
        const bool leftOk = pos == 0 || !isWord(static_cast<unsigned char>(text[pos-1]));
        const std::size_t end = pos + token.size();
        const bool rightOk = end == text.size() || !isWord(static_cast<unsigned char>(text[end]));
        if (leftOk && rightOk) return true;
        pos = text.find(token, pos + 1);
    }
    return false;
}

inline const std::vector<std::pair<std::string,std::string>>& LanguageAliases() {
    static const std::vector<std::pair<std::string,std::string>> aliases = {
        {"en","English"},{"es","Spanish"},{"fr","French"},{"de","German"},
        {"it","Italian"},{"pt","Portuguese"},{"ja","Japanese"},{"ko","Korean"},
        {"zh","Chinese"},{"ar","Arabic"},{"ru","Russian"},{"hi","Hindi"}
    };
    return aliases;
}

inline bool TryParseQuality(const Choice& choice, int& height) {
    auto numeric=[](const std::string& value, int& out)->bool {
        for (int target : {2160,1440,1080,720,480,360}) {
            const std::string needle = std::to_string(target);
            std::size_t pos = value.find(needle);
            while (pos != std::string::npos) {
                const bool leftOk = pos == 0 || !std::isdigit(static_cast<unsigned char>(value[pos-1]));
                const std::size_t end = pos + needle.size();
                const bool rightOk = end == value.size() || !std::isdigit(static_cast<unsigned char>(value[end]));
                if (leftOk && rightOk) { out = target; return true; }
                pos = value.find(needle, pos + 1);
            }
        }
        return false;
    };
    if (numeric(choice.id, height) || numeric(choice.label, height)) return true;
    const std::string text = Lower(choice.id + " " + choice.label);
    if (text.find("uhd") != std::string::npos || ContainsWordToken(text,"4k")) { height=2160; return true; }
    if (text.find("qhd") != std::string::npos) { height=1440; return true; }
    if (text.find("fhd") != std::string::npos) { height=1080; return true; }
    // HD only after excluding QHD/FHD/UHD above.
    if (ContainsWordToken(text,"hd")) { height=720; return true; }
    height=0; return false;
}

inline std::optional<Choice> MatchQuality(std::uint16_t target, const std::vector<Choice>& choices) {
    std::optional<Choice> best;
    int bestDistance = 0x7fffffff, bestHeight = -1;
    for (const Choice& choice : choices) {
        int height=0; if (!TryParseQuality(choice,height)) continue;
        const int distance = std::abs(height - static_cast<int>(target));
        if (distance < bestDistance || (distance == bestDistance && height > bestHeight)) {
            best=choice; bestDistance=distance; bestHeight=height;
        }
    }
    return best;
}

inline std::optional<Choice> MatchServer(const std::string& preferred, const std::vector<Choice>& choices) {
    const std::string needle = Trim(preferred);
    if (needle.empty()) return std::nullopt;
    for (const Choice& c : choices) if (IEquals(c.id, needle)) return c;
    for (const Choice& c : choices) if (IEquals(c.label, needle)) return c;
    return std::nullopt;
}

inline std::optional<Choice> MatchSubtitle(const std::string& languageCode, const std::vector<Choice>& choices) {
    const std::string code = Lower(Trim(languageCode));
    if (code.empty()) return std::nullopt;
    for (const Choice& c : choices) if (Lower(Trim(c.id)) == code) return c;
    for (const Choice& c : choices) {
        const std::string id=Lower(Trim(c.id));
        if (id.rfind(code+"-",0)==0 || id.rfind(code+"_",0)==0) return c;
    }
    std::string alias;
    for (const auto& [c,a] : LanguageAliases()) if (c==code) { alias=a; break; }
    if (alias.empty()) return std::nullopt;
    for (const Choice& c : choices) if (IEquals(c.label, alias)) return c;
    const std::string lowerAlias=Lower(alias);
    for (const Choice& c : choices) {
        const std::string label=Lower(Trim(c.label));
        if (label.rfind(lowerAlias+" ",0)==0 || label.rfind(lowerAlias+"(",0)==0 ||
            label.rfind(lowerAlias+"-",0)==0 || label.rfind(lowerAlias+"[",0)==0) return c;
    }
    return std::nullopt;
}

inline std::string CanonicalLanguageForChoice(const Choice& choice) {
    const std::string id=Lower(Trim(choice.id));
    for (const auto& [code,alias] : LanguageAliases()) {
        if (id==code || id.rfind(code+"-",0)==0 || id.rfind(code+"_",0)==0) return code;
    }
    const std::string label=Lower(Trim(choice.label));
    for (const auto& [code,alias] : LanguageAliases()) {
        const std::string a=Lower(alias);
        if (label==a || label.rfind(a+" ",0)==0 || label.rfind(a+"(",0)==0) return code;
    }
    return {};
}

class Resolver {
public:
    ProfileSource Source() const noexcept { return source_; }
    bool HasPending() const noexcept { return subtitlePending_ || qualityPending_ || serverPending_; }

    void ResetLauncher(const launcher_defaults::Defaults& d) {
        source_=ProfileSource::LauncherDefaults;
        subtitlePending_=qualityPending_=serverPending_=true;
        subtitleOff_=d.subtitleOff; subtitleLanguage_=d.subtitleLanguage;
        qualityAuto_=d.qualityAuto; qualityTarget_=d.targetQualityHeight;
        serverAuto_=d.serverAuto; serverPreference_=d.serverPreference;
    }
    void ResetEpisode(const cock::Preferences& p) {
        source_=ProfileSource::EpisodeCock;
        subtitlePending_=p.subtitlePresent; qualityPending_=p.qualityPresent; serverPending_=p.serverPresent;
        subtitleOff_=p.subtitleOff; subtitleLanguage_=p.subtitleLanguage;
        qualityAuto_=p.qualityAuto; qualityTarget_=p.targetQualityHeight;
        serverAuto_=p.serverAuto; serverPreference_=p.serverPreference;
    }
    void ResetUnknown() {
        source_=ProfileSource::Unknown;
        subtitlePending_=qualityPending_=serverPending_=false;
    }

    std::vector<Decision> Settle(const std::vector<Choice>& subtitles,
                                 const std::vector<Choice>& qualities,
                                 const std::vector<Choice>& servers) {
        std::vector<Decision> out; out.reserve(3);
        if (subtitlePending_) {
            subtitlePending_=false;
            if (subtitleOff_) out.push_back({"subtitle","off"});
            else { auto m=MatchSubtitle(subtitleLanguage_,subtitles); out.push_back({"subtitle",m ? m->id : "off"}); }
        }
        if (qualityPending_) {
            qualityPending_=false;
            if (!qualityAuto_) { auto m=MatchQuality(qualityTarget_,qualities); if (m) out.push_back({"quality",m->id}); }
        }
        if (serverPending_) {
            serverPending_=false;
            if (!serverAuto_) { auto m=MatchServer(serverPreference_,servers); if (m) out.push_back({"server",m->id}); }
        }
        return out;
    }

private:
    ProfileSource source_=ProfileSource::Unknown;
    bool subtitlePending_=false, qualityPending_=false, serverPending_=false;
    bool subtitleOff_=true, qualityAuto_=true, serverAuto_=true;
    std::string subtitleLanguage_, serverPreference_;
    std::uint16_t qualityTarget_=0;
};

} // namespace playback_preferences
