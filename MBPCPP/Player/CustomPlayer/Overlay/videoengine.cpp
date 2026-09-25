#include "videoengine.hpp"
#include "HtmlRenderer.hpp"
#include "moviebox_bridge.hpp"
#include "player_settings.hpp"
#include "launcher_defaults.hpp"
#include "playback_preferences.hpp"
#include <algorithm>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <limits>
#include <cctype>

// Defer window state sizing to the Windows Dispatch loop to prevent ImGui Re-entrancy crashes
static VOID CALLBACK PipTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(hwnd, idEvent);
    TogglePIP(hwnd);
}

static VOID CALLBACK MaximizeTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(hwnd, idEvent);
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_MAXIMIZE);
}

// ---------------------------------------------------------
// REDESIGNED UI FADING LOGIC (Immune to Render Stalls)
// ---------------------------------------------------------
void LerpFade(float& current, float target, float speed) {
    float dt = ImGui::GetIO().DeltaTime;
    if (dt <= 0.0f) return;

    float safeDt = std::min(dt, 0.05f); // Cap step size to 50ms max to prevent jump spikes

    float diff = target - current;
    if (std::abs(diff) < 0.001f) {
        current = target;
        return;
    }

    // FIX: True framerate-independent exponential smoothing.
    // Completely immune to snapping, and scales flawlessly for both coordinates (1000.0) and alphas (1.0)!
    float blend = 1.0f - std::exp(-speed * safeDt);
    current += diff * blend;
}

// Global Animation State Maps for Crossfading
static std::map<ImGuiID, float> s_radioHoverMap;
static std::map<ImGuiID, float> s_radioActiveMap;

// NEW: Deterministic State Machine (Replaces the broken s_lastMouseTime)
static float s_controlsAlpha = 1.0f;
static float s_sleepTimer = 3.0f;
static bool  s_isAwake = true;

static float s_lastVolume = 1.0f; // Remembers volume before mute

static bool s_showServerMenu = false;
static bool s_showSubMenu = false;
static bool s_showEpPanel = false;
static bool s_seasonDropdownOpen = false;
static std::string s_bridgeSeasonGroup;
static bool s_showContextMenu = false;

static float s_serverMenuAlpha = 0.0f;
static float s_subMenuAlpha = 0.0f;
static float s_epPanelAlpha = 0.0f;
static float s_seasonPopupAlpha = 0.0f;
static float s_contextMenuAlpha = 0.0f;
static ImVec2 s_contextMenuPos = ImVec2(0, 0);

// Sequential Icon States
static bool s_lastIsPlaying = true;
static bool s_targetIsPlaying = true;
static float s_playPauseAlpha = 1.0f;
static bool s_isSwappingIcon = false;

static bool isGlobalEnginesInit = false;
static float s_pendingStartProgress = -1.0f; // Remembers where we were during engine swaps/resumes!

// Shared instances for the HTML rendering surfaces
static dynhtml::HtmlRenderer* s_TopCardRenderer = nullptr;
static dynhtml::HtmlRenderer* s_PanelRenderer = nullptr;

// --- YouTube Style Toast Indicators ---
static float s_indicatorTimer = 0.0f;
static int s_indicatorType = 0; // 1: Play, 2: Pause, 3: Fwd, 4: Bwd, 5: Vol
static std::string s_indicatorText = "";

void TriggerIndicator(int type, const std::string& text) {
    s_indicatorType = type;
    s_indicatorText = text;
    s_indicatorTimer = 0.8f;
    s_sleepTimer = 3.0f; // Instantly force UI awake!
    s_isAwake = true;
}

// -----------------------------------------------------------------------------
// STARTUP / MEDIA LOADING PRESENTATION
// -----------------------------------------------------------------------------
static float s_loadingOverlayAlpha = 1.0f;
// Bridge mode used to perform its expensive first player frame while the HWND
// was still hidden. Keep a one-frame visible startup gate so the same loading
// shader the standalone player uses is actually presented before media/UI init.
static bool s_bridgeVisibleStartupFrameShown = false;

// -----------------------------------------------------------------------------
// MOVIEBOX BRIDGE RENDER-THREAD STATE
// -----------------------------------------------------------------------------
static mbp::PlaybackMetadata s_bridgeMetadata;
static bool s_bridgeMetadataValid = false;
static std::vector<mbp::SubtitleCue> s_bridgeCues;
static double s_bridgeSubtitleDelay = 0.0;
static std::string s_bridgeError;
static bool s_bridgeResumePending = false;
static bool s_bridgeRequestedPlaying = true;
static bool s_bridgeOpenedSent = false;
static bool s_bridgeFailedSent = false;
static bool s_bridgeEndedSent = false;

// C#-compatible per-video/per-episode settings identity. The bridge is the
// source of identity; this state exists only to save the outgoing episode
// before a MovieBox source revision replaces it.
static bool s_cockIdentityReady = false;
static bool s_cockSettingsLoaded = false;
static std::string s_cockVideoUri;
static std::string s_cockTitle;
static short s_cockSeason = 0;
static short s_cockEpisode = 0;
static unsigned char s_cockBoxType = 0;

// The source revision chooses exactly one profile source. Any valid episode
// .cock suppresses launcher defaults completely; launcher defaults are only
// used for content with no valid episode profile.
static playback_preferences::ProfileSource s_profileSource = playback_preferences::ProfileSource::Unknown;
static playback_preferences::Resolver s_preferenceResolver;
static launcher_defaults::Defaults s_launcherDefaults;
static cock::Preferences s_activePreferences;

static std::string TrimSettingsTitle(std::string value) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

static void ConfigureSettingsIdentity(const mbp::PlaybackSource& source) {
    s_cockVideoUri = source.uri;
    s_cockTitle = TrimSettingsTitle(source.settingsTitle.empty() ? source.title : source.settingsTitle);
    if (s_cockTitle.empty()) s_cockTitle = TrimSettingsTitle(source.title);
    if (s_cockTitle.empty()) s_cockTitle = cock::detail::SafeBaseName(source.uri, "video");
    s_cockSeason = std::max<short>(0, source.season);
    s_cockEpisode = std::max<short>(0, source.episode);
    s_cockBoxType = (source.boxType == 1 || source.boxType == 2)
        ? source.boxType
        : static_cast<unsigned char>((s_cockSeason != 0 || s_cockEpisode != 0) ? 2 : 1);
    s_cockIdentityReady = !s_cockVideoUri.empty();
    s_cockSettingsLoaded = false;
}

void SaveCurrentEpisodeSettings() {
    if (!s_cockIdentityReady) return;

    cock::Settings settings;
    settings.title = s_cockTitle;
    settings.season = s_cockSeason;
    settings.episode = s_cockEpisode;
    settings.boxType = s_cockBoxType;
    settings.muted = g_Settings.muted;
    const float savedVolume = g_Settings.muted ? s_lastVolume : g_Settings.volume;
    settings.volume = static_cast<short>(std::clamp(static_cast<int>(std::lround(savedVolume * 100.0f)), 0, 100));
    settings.playbackSpeed = static_cast<float>(std::clamp(g_Settings.bridgePlaybackRate, 0.25, 4.0));
    settings.subtitleDelay = static_cast<float>(std::clamp(s_bridgeSubtitleDelay, -30.0, 30.0));
    settings.audioTrackIndex = g_Settings.bridgeAudioTrackIndex;
    settings.preferences = s_activePreferences;
    cock::Save(s_cockVideoUri, settings); // Best-effort by design.
}

static bool LoadCurrentEpisodeSettings() {
    if (!s_cockIdentityReady) return false;
    const auto settings = cock::Load(s_cockVideoUri, s_cockTitle, s_cockSeason, s_cockEpisode, s_cockBoxType);
    if (!settings) return false;

    g_Settings.volume = static_cast<float>(settings->volume) / 100.0f;
    s_lastVolume = g_Settings.volume > 0.0001f ? g_Settings.volume : 1.0f;
    g_Settings.muted = settings->muted;
    g_Settings.bridgePlaybackRate = settings->playbackSpeed;
    s_bridgeSubtitleDelay = settings->subtitleDelay;
    g_Settings.bridgeAudioTrackIndex = settings->audioTrackIndex;
    s_activePreferences = settings->preferences;
    s_cockSettingsLoaded = true;

    if (isGlobalEnginesInit) {
        const float effectiveVolume = g_Settings.muted ? 0.0f : g_Settings.volume;
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(effectiveVolume);
        else SetVLCVolume(effectiveVolume);
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerRate(static_cast<float>(g_Settings.bridgePlaybackRate));
        else SetVLCRate(static_cast<float>(g_Settings.bridgePlaybackRate));
    }
    return true;
}

static cock::Preferences PreferencesFromLauncher(const launcher_defaults::Defaults& defaults) {
    cock::Preferences p;
    p.subtitlePresent = true;
    p.subtitleOff = defaults.subtitleOff;
    p.subtitleLanguage = defaults.subtitleOff ? std::string{} : defaults.subtitleLanguage;
    p.qualityPresent = true;
    p.qualityAuto = defaults.qualityAuto;
    p.targetQualityHeight = defaults.qualityAuto ? 0u : defaults.targetQualityHeight;
    p.serverPresent = true;
    p.serverAuto = defaults.serverAuto;
    p.serverPreference = defaults.serverAuto ? std::string{} : defaults.serverPreference;
    return p;
}

static std::vector<playback_preferences::Choice> ToPreferenceChoices(const std::vector<mbp::PlaybackChoice>& choices) {
    std::vector<playback_preferences::Choice> out;
    out.reserve(choices.size());
    for (const mbp::PlaybackChoice& c : choices) out.push_back({ c.id, c.label, c.selected });
    return out;
}

static void CaptureSubtitleOffPreference() {
    s_activePreferences.subtitlePresent = true;
    s_activePreferences.subtitleOff = true;
    s_activePreferences.subtitleLanguage.clear();
}

static void CaptureBridgePreference(const std::string& action, const mbp::PlaybackChoice& choice) {
    const playback_preferences::Choice prefChoice{ choice.id, choice.label, choice.selected };
    if (action == "quality") {
        const std::string id = playback_preferences::Lower(playback_preferences::Trim(choice.id));
        const std::string label = playback_preferences::Lower(playback_preferences::Trim(choice.label));
        s_activePreferences.qualityPresent = true;
        if (id == "auto" || label == "auto") {
            s_activePreferences.qualityAuto = true;
            s_activePreferences.targetQualityHeight = 0;
            return;
        }
        int height = 0;
        if (playback_preferences::TryParseQuality(prefChoice, height) && height > 0 && height <= 65535) {
            s_activePreferences.qualityAuto = false;
            s_activePreferences.targetQualityHeight = static_cast<std::uint16_t>(height);
        } else {
            s_activePreferences.qualityPresent = false;
            s_activePreferences.qualityAuto = false;
            s_activePreferences.targetQualityHeight = 0;
        }
        return;
    }
    if (action == "server") {
        s_activePreferences.serverPresent = true;
        const std::string id = playback_preferences::Trim(choice.id);
        const std::string label = playback_preferences::Trim(choice.label);
        if (playback_preferences::IEquals(id, "auto") || playback_preferences::IEquals(label, "auto")) {
            s_activePreferences.serverAuto = true;
            s_activePreferences.serverPreference.clear();
        } else {
            s_activePreferences.serverAuto = false;
            s_activePreferences.serverPreference = !id.empty() ? id : label;
            if (s_activePreferences.serverPreference.empty()) s_activePreferences.serverPresent = false;
        }
        return;
    }
    if (action == "subtitle") {
        const std::string language = playback_preferences::CanonicalLanguageForChoice(prefChoice);
        if (!language.empty()) {
            s_activePreferences.subtitlePresent = true;
            s_activePreferences.subtitleOff = false;
            s_activePreferences.subtitleLanguage = language;
        }
    }
}

static void CaptureAppliedPreferenceDecision(const playback_preferences::Decision& decision) {
    if (decision.action == "subtitle") {
        if (decision.id == "off") {
            CaptureSubtitleOffPreference();
            return;
        }
        for (const mbp::PlaybackChoice& choice : s_bridgeMetadata.subtitles)
            if (choice.id == decision.id) { CaptureBridgePreference("subtitle", choice); return; }
    } else if (decision.action == "quality") {
        for (const mbp::PlaybackChoice& choice : s_bridgeMetadata.qualities)
            if (choice.id == decision.id) { CaptureBridgePreference("quality", choice); return; }
    } else if (decision.action == "server") {
        for (const mbp::PlaybackChoice& choice : s_bridgeMetadata.servers)
            if (choice.id == decision.id) { CaptureBridgePreference("server", choice); return; }
    }
}

static void SettlePendingBridgePreferences() {
    if (!s_bridgeMetadataValid || !s_preferenceResolver.HasPending()) return;
    const auto decisions = s_preferenceResolver.Settle(
        ToPreferenceChoices(s_bridgeMetadata.subtitles),
        ToPreferenceChoices(s_bridgeMetadata.qualities),
        ToPreferenceChoices(s_bridgeMetadata.servers));
    for (const playback_preferences::Decision& decision : decisions) {
        CaptureAppliedPreferenceDecision(decision);
        MovieBoxBridgeQueueAction(decision.action, decision.id);
        if (decision.action == "subtitle" && decision.id == "off") {
            s_bridgeCues.clear();
            if (g_Settings.engineMode == 1) SetVLCSpuTrack(-1);
        }
    }
}

static void ApplyResolvedLauncherVolume() {
    g_Settings.volume = static_cast<float>(s_launcherDefaults.volume) / 100.0f;
    if (g_Settings.volume > 0.0001f) s_lastVolume = g_Settings.volume;
    if (isGlobalEnginesInit) {
        const float effectiveVolume = g_Settings.muted ? 0.0f : g_Settings.volume;
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(effectiveVolume);
        else SetVLCVolume(effectiveVolume);
    }
}

static void ResolveCurrentProfile() {
    s_profileSource = playback_preferences::ProfileSource::Unknown;
    s_preferenceResolver.ResetUnknown();
    s_activePreferences = {};
    s_launcherDefaults = launcher_defaults::BuiltIn();

    if (LoadCurrentEpisodeSettings()) {
        s_profileSource = playback_preferences::ProfileSource::EpisodeCock;
        s_preferenceResolver.ResetEpisode(s_activePreferences);
        return;
    }

    s_launcherDefaults = launcher_defaults::Load();
    s_profileSource = playback_preferences::ProfileSource::LauncherDefaults;
    s_activePreferences = PreferencesFromLauncher(s_launcherDefaults);
    s_preferenceResolver.ResetLauncher(s_launcherDefaults);
    ApplyResolvedLauncherVolume();
}

static float BridgeEngineDuration() {
    return (g_Settings.engineMode == 0 || g_Settings.engineMode == 2)
        ? GetCPlayerDuration()
        : GetVLCDuration();
}

static float BridgeEngineProgress() {
    return (g_Settings.engineMode == 0 || g_Settings.engineMode == 2)
        ? GetCPlayerProgress()
        : GetVLCPlayerProgress();
}

static void BridgePauseEngine(bool pause) {
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) PauseCPlayer(pause);
    else PauseVLCPlayer(pause);
    g_Settings.isPlaying = !pause;
}

static void BridgeSetRate(double rate) {
    const float safe = static_cast<float>(std::clamp(rate, 0.25, 4.0));
    g_Settings.bridgePlaybackRate = safe;
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerRate(safe);
    else SetVLCRate(safe);
}

static void BridgeSetVolumePercent(double percent) {
    const float normalized = static_cast<float>(std::clamp(percent, 0.0, 100.0) / 100.0);
    g_Settings.volume = normalized;
    if (normalized > 0.0001f) {
        s_lastVolume = normalized;
        g_Settings.muted = false;
    }
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(normalized);
    else SetVLCVolume(normalized);
}

static void BridgeSetMuted(bool muted) {
    if (muted) {
        if (g_Settings.volume > 0.0001f) s_lastVolume = g_Settings.volume;
        g_Settings.muted = true;
        g_Settings.volume = 0.0f;
    } else {
        g_Settings.muted = false;
        if (g_Settings.volume <= 0.0001f) g_Settings.volume = s_lastVolume > 0.0001f ? s_lastVolume : 1.0f;
    }
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(g_Settings.volume);
    else SetVLCVolume(g_Settings.volume);
}

static bool BridgeSeekSeconds(double seconds) {
    const float duration = BridgeEngineDuration();
    if (!std::isfinite(duration) || duration <= 0.0f || !std::isfinite(seconds)) return false;
    g_Settings.progress = std::clamp(static_cast<float>(seconds / duration), 0.0f, 1.0f);
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SeekCPlayer(g_Settings.progress);
    else SeekVLCPlayer(g_Settings.progress);
    return true;
}

static double ParseBridgeTimeSpan(const std::string& text) {
    try {
        const size_t c1 = text.find(':');
        const size_t c2 = c1 == std::string::npos ? std::string::npos : text.find(':', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) return -1.0;

        std::string hourPart = text.substr(0, c1);
        const int minutes = std::stoi(text.substr(c1 + 1, c2 - c1 - 1));
        const double seconds = std::stod(text.substr(c2 + 1));
        int days = 0;
        int hours = 0;
        const size_t dot = hourPart.find('.');
        if (dot != std::string::npos) {
            days = std::stoi(hourPart.substr(0, dot));
            hours = std::stoi(hourPart.substr(dot + 1));
        } else {
            hours = std::stoi(hourPart);
        }
        if (days < 0 || hours < 0 || minutes < 0 || minutes > 59 || seconds < 0.0 || seconds >= 60.0) return -1.0;
        return days * 86400.0 + hours * 3600.0 + minutes * 60.0 + seconds;
    } catch (...) {
        return -1.0;
    }
}

static const mbp::JsonValue* BridgeCommandValue(const mbp::PlaybackCommand& command, const char* key) {
    auto it = command.values.find(key);
    return it == command.values.end() ? nullptr : &it->second;
}

static void ApplyMovieBoxCommand(HWND hWnd, const mbp::PlaybackCommand& command) {
    if (command.action == "Close") {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
        return;
    }

    if (command.action == "Pause") {
        if (const mbp::JsonValue* value = BridgeCommandValue(command, "Pause"); value && value->isBool())
            BridgePauseEngine(value->boolean);
        else
            BridgePauseEngine(g_Settings.isPlaying);
        return;
    }

    if (command.action == "Time") {
        if (const mbp::JsonValue* value = BridgeCommandValue(command, "Time"); value && value->isString()) {
            const double seconds = ParseBridgeTimeSpan(value->string);
            if (seconds >= 0.0) BridgeSeekSeconds(seconds);
        } else if (const mbp::JsonValue* value = BridgeCommandValue(command, "Percent")) {
            const float duration = BridgeEngineDuration();
            if (std::isfinite(duration) && duration > 0.0f) BridgeSeekSeconds(duration * std::clamp(value->asNumber(0.0), 0.0, 100.0) / 100.0);
        } else if (BridgeCommandValue(command, "Forward")) {
            const float duration = BridgeEngineDuration();
            const float progress = BridgeEngineProgress();
            if (duration > 0.0f && progress >= 0.0f) BridgeSeekSeconds(progress * duration + 10.0);
        } else if (BridgeCommandValue(command, "Backward")) {
            const float duration = BridgeEngineDuration();
            const float progress = BridgeEngineProgress();
            if (duration > 0.0f && progress >= 0.0f) BridgeSeekSeconds(progress * duration - 10.0);
        }
        return;
    }

    if (command.action == "Volume") {
        if (const mbp::JsonValue* value = BridgeCommandValue(command, "Volume")) BridgeSetVolumePercent(value->asNumber(g_Settings.volume * 100.0));
        return;
    }

    if (command.action == "Mute") {
        if (const mbp::JsonValue* value = BridgeCommandValue(command, "Mute"); value && value->isBool()) BridgeSetMuted(value->boolean);
        else BridgeSetMuted(!g_Settings.muted && g_Settings.volume > 0.0001f);
        return;
    }

    if (command.action == "Play") {
        const mbp::JsonValue* rate = BridgeCommandValue(command, "rate");
        BridgeSetRate(rate ? rate->asNumber(1.0) : 1.0);
        BridgePauseEngine(false);
        return;
    }

    if (command.action == "AudioDelay") {
        if (const mbp::JsonValue* value = BridgeCommandValue(command, "AudioDelay"))
            g_Settings.bridgeAudioDelaySeconds = value->asNumber(0.0);
        return;
    }

    if (command.action == "Stop") {
        StopCPlayer();
        StopVLCPlayer();
        g_Settings.isPlaying = false;
    }
}

static void PumpMovieBoxBridge(HWND hWnd) {
    if (!g_Settings.bridgeMode) return;

    mbp::PlaybackReply reply;
    while (MovieBoxBridgePopReply(reply)) {
        if (!reply.error.empty()) s_bridgeError = reply.error;
        if (reply.closed) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
            continue;
        }

        bool settingsSourceChanged = false;
        if (reply.source && reply.source->revision != g_Settings.bridgeRevision) {
            const mbp::PlaybackSource& source = *reply.source;
            SaveCurrentEpisodeSettings();
            s_cockIdentityReady = false;
            s_cockSettingsLoaded = false;
            s_profileSource = playback_preferences::ProfileSource::Unknown;
            s_preferenceResolver.ResetUnknown();
            s_activePreferences = {};
            g_Settings.bridgeRevision = source.revision;
            g_Settings.currentUrl = source.uri;
            g_Settings.videoTitle = source.title;
            g_Settings.bridgeSettingsTitle = source.settingsTitle;
            g_Settings.bridgeSeason = source.season;
            g_Settings.bridgeEpisode = source.episode;
            g_Settings.bridgeBoxType = source.boxType;
            g_Settings.bridgeResumeSeconds = std::max(0.0, source.seconds);
            g_Settings.bridgePlaybackRate = std::clamp(source.rate, 0.25, 4.0);
            g_Settings.progress = 0.0f;

            // Prime paused MovieBox sources through their first frame, then restore
            // the requested pause state after resume/open has been applied.
            s_bridgeRequestedPlaying = source.playing;
            g_Settings.isPlaying = true;
            s_bridgeResumePending = g_Settings.bridgeResumeSeconds > 0.001;
            s_pendingStartProgress = -1.0f;
            g_Settings.requestPlay = !g_Settings.currentUrl.empty();
            s_loadingOverlayAlpha = 1.0f;
            s_bridgeOpenedSent = false;
            s_bridgeFailedSent = false;
            s_bridgeEndedSent = false;
            s_bridgeMetadataValid = false;
            s_bridgeCues.clear();
            s_bridgeSeasonGroup.clear();
            s_bridgeError.clear();
            ConfigureSettingsIdentity(source);
            ResolveCurrentProfile();
            settingsSourceChanged = true;
        }

        if (reply.metadata) {
            s_bridgeMetadata = *reply.metadata;
            s_bridgeMetadataValid = true;
            SettlePendingBridgePreferences();
        }
        if (reply.cues) s_bridgeCues = *reply.cues;
        if (s_profileSource != playback_preferences::ProfileSource::EpisodeCock)
            s_bridgeSubtitleDelay = reply.subtitleDelay;

        for (const mbp::PlaybackCommand& command : reply.commands) ApplyMovieBoxCommand(hWnd, command);

        // Keep the original authority ordering: bridge commands in the same
        // source-change reply run first, then the episode .cock base is re-applied
        // so an existing episode profile wins completely. The earlier load was
        // still necessary to decide episode-vs-launcher defaults before engine init.
        if (settingsSourceChanged && s_profileSource == playback_preferences::ProfileSource::EpisodeCock) {
            LoadCurrentEpisodeSettings();
            MovieBoxBridgeQueueAction("subtitleDelay", "", s_bridgeSubtitleDelay);
        }
    }
}

static void SyncMovieBoxPlaybackState(float engineDuration, bool bridgeMediaReady) {
    if (!g_Settings.bridgeMode) return;

    const bool usingNative = (g_Settings.engineMode == 0 || g_Settings.engineMode == 2);
    if (!s_bridgeFailedSent && usingNative && CPlayerHasError()) {
        MovieBoxBridgeQueueAction("failed");
        s_bridgeFailedSent = true;
    }

    if (bridgeMediaReady && engineDuration > 0.0f) {
        if (s_bridgeResumePending) {
            g_Settings.progress = std::clamp(static_cast<float>(g_Settings.bridgeResumeSeconds / engineDuration), 0.0f, 1.0f);
            if (usingNative) SeekCPlayer(g_Settings.progress);
            else SeekVLCPlayer(g_Settings.progress);
            s_bridgeResumePending = false;
        }

        BridgeSetRate(g_Settings.bridgePlaybackRate);
        if (!s_bridgeOpenedSent) {
            if (!s_bridgeRequestedPlaying) BridgePauseEngine(true);
            MovieBoxBridgeQueueAction("opened");
            s_bridgeOpenedSent = true;
        }
    }

    float realProgress = BridgeEngineProgress();
    if (std::isfinite(realProgress) && realProgress >= 0.0f)
        g_Settings.progress = std::clamp(realProgress, 0.0f, 1.0f);

    const bool engineEnded = usingNative ? CPlayerHasEnded() : VLCPlayerHasEnded();
    if (s_bridgeOpenedSent && engineEnded && !s_bridgeEndedSent) {
        MovieBoxBridgeQueueAction("ended");
        s_bridgeEndedSent = true;
        g_Settings.isPlaying = false;
    }

    MovieBoxPlaybackState state;
    state.revision = g_Settings.bridgeRevision;
    state.duration = (std::isfinite(engineDuration) && engineDuration > 0.0f) ? engineDuration : 0.0;
    state.position = state.duration > 0.0 ? std::clamp(static_cast<double>(g_Settings.progress) * state.duration, 0.0, state.duration) : 0.0;
    state.ready = bridgeMediaReady && s_bridgeOpenedSent;
    state.playing = g_Settings.isPlaying && !engineEnded;
    state.volume = std::clamp(static_cast<int>(std::lround(g_Settings.volume * 100.0f)), 0, 100);
    state.muted = g_Settings.muted || g_Settings.volume <= 0.0001f;
    MovieBoxBridgeSetPlaybackState(state);
}

static void RenderMovieBoxSubtitles(ImDrawList* drawList, const ImVec2& screenSize, double playbackSeconds) {
    if (!g_Settings.bridgeMode || !drawList || s_bridgeCues.empty() || !std::isfinite(playbackSeconds)) return;

    const double cueTime = playbackSeconds - s_bridgeSubtitleDelay;
    std::vector<std::string> lines;
    for (const mbp::SubtitleCue& cue : s_bridgeCues) {
        if (cueTime < cue.start || cueTime > cue.end || cue.text.empty()) continue;
        std::stringstream stream(cue.text);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) lines.push_back(line);
        }
    }
    if (lines.empty()) return;

    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize() * (g_IsPIPMode ? 0.95f : 1.18f);
    const float lineGap = 4.0f;
    float widest = 0.0f;
    float lineHeight = fontSize;
    for (const std::string& line : lines) {
        const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, line.c_str());
        widest = std::max(widest, size.x);
        lineHeight = std::max(lineHeight, size.y);
    }

    const float padX = 14.0f;
    const float padY = 9.0f;
    const float totalHeight = lineHeight * static_cast<float>(lines.size()) +
        lineGap * static_cast<float>(lines.size() > 1 ? lines.size() - 1 : 0);
    const float bottomMargin = g_IsPIPMode ? 28.0f : 86.0f;
    const ImVec2 boxMin(
        std::max(10.0f, (screenSize.x - widest) * 0.5f - padX),
        std::max(10.0f, screenSize.y - bottomMargin - totalHeight - padY * 2.0f));
    const ImVec2 boxMax(
        std::min(screenSize.x - 10.0f, (screenSize.x + widest) * 0.5f + padX),
        boxMin.y + totalHeight + padY * 2.0f);

    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 176), 9.0f);
    drawList->AddRect(boxMin, boxMax, IM_COL32(255, 255, 255, 30), 9.0f);

    float y = boxMin.y + padY;
    for (const std::string& line : lines) {
        const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, line.c_str());
        const ImVec2 textPos((screenSize.x - size.x) * 0.5f, y);
        // A subtle shadow keeps captions readable against bright video without
        // turning the subtitle card into an opaque block.
        drawList->AddText(font, fontSize, ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 220), line.c_str());
        drawList->AddText(font, fontSize, textPos, IM_COL32(255, 255, 255, 255), line.c_str());
        y += lineHeight + lineGap;
    }
}

static void RenderPlayerLoadingScreen(const char* statusText, float opacity)
{
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size = io.DisplaySize;

    if (!dl || size.x <= 0.0f || size.y <= 0.0f || opacity <= 0.001f)
        return;

    DrawLoadingShader(
        dl,
        ImVec2(0.0f, 0.0f),
        size,
        static_cast<float>(ImGui::GetTime()),
        opacity);

    // Keep typography intentionally minimal so the shader is the visual focus.
    ImFont* font = ImGui::GetFont();
    const float base = ImGui::GetFontSize();
    const ImVec2 center(size.x * 0.5f, size.y * 0.5f);

    const char* title = "CUSTOMPLAYER";
    const float titleSizePx = base * 0.78f;
    const ImVec2 titleSize = font->CalcTextSizeA(titleSizePx, FLT_MAX, 0.0f, title);

    dl->AddText(
        font,
        titleSizePx,
        ImVec2(center.x - titleSize.x * 0.5f, center.y + 82.0f),
        IM_COL32(226, 229, 244, static_cast<int>(215.0f * opacity)),
        title);

    const char* safeStatus =
        (statusText && statusText[0] != '\0') ? statusText : "Preparing media...";

    const float statusSizePx = base * 0.72f;
    const ImVec2 statusSize = font->CalcTextSizeA(statusSizePx, FLT_MAX, 0.0f, safeStatus);

    dl->AddText(
        font,
        statusSizePx,
        ImVec2(center.x - statusSize.x * 0.5f, center.y + 105.0f),
        IM_COL32(150, 157, 181, static_cast<int>(190.0f * opacity)),
        safeStatus);
}

static void RenderBridgeStartupLoadingFrame(const char* statusText)
{
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    if (size.x <= 0.0f || size.y <= 0.0f) return;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::Begin("BridgeStartupLoading", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    RenderPlayerLoadingScreen(statusText, 1.0f);
    ImGui::End();
}

void RenderVideoEngine(HWND hWnd)
{
    // Bridge replies are consumed on the render thread so all engine/UI mutations
    // stay serialized with the player frame.
    PumpMovieBoxBridge(hWnd);

    // FIX: Queue the exit to the start of the frame to prevent the silent nvwgf2umx.dll crash!
    // Destroying the player COM textures while they are actively queued in ImGui's DrawList 
    // causes the graphics driver to instantly access violate.
    static bool s_pendingExitToDashboard = false;
    if (s_pendingExitToDashboard && g_Settings.bridgeMode) {
        // A bridge session has no dashboard destination. If legacy UI ever
        // queues that transition, close through the normal bridge-aware
        // shutdown path instead of stopping media and revealing standalone UI.
        s_pendingExitToDashboard = false;
        PostMessage(hWnd, WM_CLOSE, 0, 0);
        return;
    }
    if (s_pendingExitToDashboard) {
        StopCPlayer();
        StopVLCPlayer();
        g_AppMode = MODE_DASHBOARD;
        s_pendingExitToDashboard = false;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screenSize = io.DisplaySize;

    // Bridge startup is intentionally lightweight until MovieBox has supplied a
    // source. More importantly, the first *visible* frame is always the normal
    // player loading shader. The old path did all expensive initialization in
    // the hidden swap-chain prime, so the loader was effectively consumed before
    // the user could ever see it.
    if (g_Settings.bridgeMode) {
        const bool waitingForSource = g_Settings.currentUrl.empty();
        if (!s_bridgeVisibleStartupFrameShown || waitingForSource) {
            std::string status = MovieBoxBridgeLastError();
            if (status.empty()) {
                if (waitingForSource) {
                    status = MovieBoxBridgeConnected()
                        ? "Waiting for MovieBoxPro..."
                        : "Connecting to MovieBoxPro...";
                }
                else {
                    status = "Preparing media...";
                }
            }

            RenderBridgeStartupLoadingFrame(status.c_str());
            if (IsWindowVisible(hWnd)) s_bridgeVisibleStartupFrameShown = true;
            return;
        }
    }

    // --- GLASSMORPHIC SCROLLBAR STYLER ---
    auto PushScrollbarStyles = []() {
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 25));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(139, 92, 246, 150));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(139, 92, 246, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f);
        };
    auto PopScrollbarStyles = []() {
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        };

    // 1. GLOBAL INPUT WAKERS (Guarantees the UI is completely responsive)
    auto WakeUpUI = [&]() {
        s_sleepTimer = 3.0f;
        s_isAwake = true;
        };

    // Using native cross-version mouse and wheel checks instead of ImGui::IsAnyKeyPressed()
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        io.MouseWheel != 0.0f)
    {
        WakeUpUI();
    }

    // Compare ONLY this frame's physical cursor movement. The previous version
    // left lastMousePos untouched for sub-2px motion, so tiny sensor jitter
    // accumulated against an old baseline until it falsely woke the controls.
    static ImVec2 lastMousePos = io.MousePos;
    const ImVec2 mouseDelta(
        io.MousePos.x - lastMousePos.x,
        io.MousePos.y - lastMousePos.y);
    lastMousePos = io.MousePos; // Always advance the baseline every frame.

    const float mouseDeltaSq =
        mouseDelta.x * mouseDelta.x +
        mouseDelta.y * mouseDelta.y;

    // A >3px single-frame move is genuine interaction while normal high-DPI
    // sensor noise no longer stacks up across frames.
    if (mouseDeltaSq > 9.0f) {
        WakeUpUI();
    }

    // 2. LIFECYCLE & ENGINE SELECTION
    if (!isGlobalEnginesInit) {
        char buf[64];
        GetPrivateProfileStringA("Profile", "Volume", "1.0", buf, sizeof(buf), ".\\tvanime_settings.ini");
        try { g_ActiveProfile.defaultVolume = std::stof(buf); }
        catch (...) { g_ActiveProfile.defaultVolume = 1.0f; }

        GetPrivateProfileStringA("Profile", "Quality", "Auto", buf, sizeof(buf), ".\\tvanime_settings.ini");
        g_ActiveProfile.defaultQuality = buf;

        // Initialize only the engine that will actually play this source. The
        // previous bridge startup paid for both Media Foundation and libVLC even
        // though only one could be active, which added avoidable launch latency.
        if (g_Settings.engineMode == 1) InitVLCPlayer();
        else InitCPlayer(hWnd);

        if (!s_cockSettingsLoaded &&
            !(g_Settings.bridgeMode && s_profileSource == playback_preferences::ProfileSource::LauncherDefaults))
            g_Settings.volume = g_ActiveProfile.defaultVolume;
        s_lastVolume = g_Settings.volume > 0.0001f ? g_Settings.volume : s_lastVolume;
        isGlobalEnginesInit = true;
        s_lastIsPlaying = g_Settings.isPlaying;
        s_targetIsPlaying = g_Settings.isPlaying;
    }

    if (g_Settings.requestPlay) {
        // Engine/media swaps should cover the old surface immediately rather
        // than fading the loader in over a stale frame.
        s_loadingOverlayAlpha = 1.0f;

        // A pending start position is only for a real resume/engine-swap seek.
        // Fresh playback begins at 0 naturally; queueing a zero seek here causes
        // a visible restart once duration/metadata becomes available.
        if (s_pendingStartProgress < 0.0f) {
            if (std::isfinite(g_Settings.progress) && g_Settings.progress > 0.001f) {
                s_pendingStartProgress = std::clamp(g_Settings.progress, 0.0f, 1.0f);
            }
        }
        else if (s_pendingStartProgress <= 0.001f) {
            // Normalize explicit "start from beginning" requests to no pending seek.
            s_pendingStartProgress = -1.0f;
        }
        StopCPlayer();
        StopVLCPlayer();

        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) {
            // Idempotent and also handles a later switch from VLC to the native engine.
            InitCPlayer(hWnd);
            LoadCPlayer(g_Settings.currentUrl);
        }
        else {
            // Idempotent and also handles a later switch from the native engine to VLC.
            InitVLCPlayer();
            LoadVLCPlayer(g_Settings.currentUrl);
        }

        if (g_Settings.bridgeMode) {
            BridgeSetRate(g_Settings.bridgePlaybackRate);
            const float effectiveVolume = g_Settings.muted ? 0.0f : g_Settings.volume;
            if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(effectiveVolume);
            else SetVLCVolume(effectiveVolume);
        }

        g_Settings.requestPlay = false;
    }

    // IMPORTANT: do not return here while loading. Both CPlayer and VLC need
    // their Render* function to keep running so decoded frames can reach D3D11.
    const bool usingNative =
        (g_Settings.engineMode == 0 || g_Settings.engineMode == 2);

    float realEngineDuration = (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) ? GetCPlayerDuration() : GetVLCDuration();
    const bool hasValidDuration = std::isfinite(realEngineDuration) && realEngineDuration > 0.0f;
    float engineDuration = hasValidDuration ? realEngineDuration : 0.0f;

    auto TogglePlay = [&]() {
        g_Settings.isPlaying = !g_Settings.isPlaying;
        TriggerIndicator(g_Settings.isPlaying ? 1 : 2, "");
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) PauseCPlayer(!g_Settings.isPlaying);
        else PauseVLCPlayer(!g_Settings.isPlaying);
        };

    auto ToggleMute = [&]() {
        if (!g_Settings.muted && g_Settings.volume > 0.0f) {
            s_lastVolume = g_Settings.volume;
            g_Settings.volume = 0.0f;
            g_Settings.muted = true;
            TriggerIndicator(5, "Muted");
        }
        else {
            g_Settings.volume = s_lastVolume > 0.0f ? s_lastVolume : 1.0f;
            g_Settings.muted = false;
            TriggerIndicator(5, std::to_string((int)(std::round(g_Settings.volume * 100))) + "%");
        }
        g_ActiveProfile.defaultVolume = g_Settings.volume;
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(g_Settings.volume);
        else SetVLCVolume(g_Settings.volume);
        };

    auto HardwareSeek = [&]() {
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SeekCPlayer(g_Settings.progress);
        else SeekVLCPlayer(g_Settings.progress);
        };

    // 3. GLOBAL CLICK DISPATCHER (Evaluated at the TOP so it perfectly catches all clicks even when hidden)
    float headerPaddingTop = g_IsPIPMode ? 16.0f : 36.0f;
    float headerPaddingLeft = g_IsPIPMode ? 16.0f : 32.0f;
    float headerPaddingRight = g_IsPIPMode ? 16.0f : 34.0f;
    float headerHeight = headerPaddingTop + 40.0f + 15.0f;

    bool clickedMenu = false;
    ImVec2 mPos = io.MousePos;

    const float serverMenuHitHeight = g_Settings.bridgeMode
        ? std::min(680.0f, std::max(430.0f, screenSize.y - headerHeight - 18.0f))
        : 430.0f;
    const float subMenuHitHeight = g_Settings.bridgeMode ? 390.0f : 320.0f;
    if (s_showServerMenu && mPos.x >= screenSize.x - headerPaddingRight - 340.0f && mPos.x <= screenSize.x - headerPaddingRight && mPos.y >= headerHeight + 5.0f && mPos.y <= headerHeight + 5.0f + serverMenuHitHeight) clickedMenu = true;
    if (s_showSubMenu && mPos.x >= screenSize.x - headerPaddingRight - 280.0f && mPos.x <= screenSize.x - headerPaddingRight && mPos.y >= headerHeight + 5.0f && mPos.y <= headerHeight + 5.0f + subMenuHitHeight) clickedMenu = true;
    if (s_showEpPanel && mPos.x >= screenSize.x - 380.0f && mPos.x <= screenSize.x && mPos.y >= 0 && mPos.y <= screenSize.y) clickedMenu = true;
    if (s_showContextMenu && mPos.x >= s_contextMenuPos.x && mPos.x <= s_contextMenuPos.x + 320.0f && mPos.y >= s_contextMenuPos.y && mPos.y <= s_contextMenuPos.y + 300.0f) clickedMenu = true;

    if (s_controlsAlpha > 0.01f) {
        if (mPos.y >= 0 && mPos.y <= headerHeight) clickedMenu = true;
        float bottomMargin = g_IsPIPMode ? 50.0f : 120.0f;
        if (mPos.y >= screenSize.y - bottomMargin) clickedMenu = true;
    }

    static POINT s_mouseDownPosAbs = { 0, 0 };
    static bool s_wasAsleepOnMouseDown = false;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_wasAsleepOnMouseDown = !s_isAwake;
        GetCursorPos(&s_mouseDownPosAbs); // Track drag distances securely
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_showContextMenu = true;
        s_contextMenuPos = io.MousePos;
        if (s_contextMenuPos.x + 320.0f > screenSize.x) s_contextMenuPos.x = screenSize.x - 320.0f;
        if (s_contextMenuPos.y + 300.0f > screenSize.y) s_contextMenuPos.y = screenSize.y - 300.0f;
        s_showServerMenu = s_showSubMenu = s_showEpPanel = s_seasonDropdownOpen = false;
    }
    else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        POINT pt; GetCursorPos(&pt);
        float dx = (float)(pt.x - s_mouseDownPosAbs.x);
        float dy = (float)(pt.y - s_mouseDownPosAbs.y);

        if (std::abs(dx) < 5.0f && std::abs(dy) < 5.0f) {
            if (!clickedMenu) {
                if (s_showServerMenu || s_showSubMenu || s_showEpPanel || s_seasonDropdownOpen || s_showContextMenu) {
                    // Close menus, but ensure WakeUpUI keeps the timer full so it doesn't instantly fade out!
                    s_showServerMenu = s_showSubMenu = s_showEpPanel = s_seasonDropdownOpen = s_showContextMenu = false;
                }
                else if (!s_wasAsleepOnMouseDown) {
                    TogglePlay(); // Only pause if the UI was already visible during the click
                }
            }
        }
    }

    // 4. MAIN VIDEO RENDERER
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(screenSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) ? IM_COL32(0, 0, 0, 0) : IM_COL32(0, 0, 0, 255));

    ImGui::Begin("VideoEngineOverlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    HandleAppDrag(hWnd);

    ImVec2 renderPos(0, 0), renderSize = screenSize;

    // UNIFIED LETTERBOX CALCULATION
    float vW = 1920.0f, vH = 1080.0f;
    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) {
        vW = (float)GetCPlayerVideoWidth();
        vH = (float)GetCPlayerVideoHeight();
    }
    else {
        vW = (float)GetVLCVideoWidth();
        vH = (float)GetVLCVideoHeight();
    }

    if (vW <= 0.0f) vW = 1920.0f;
    if (vH <= 0.0f) vH = 1080.0f;

    float screenAspect = screenSize.x / screenSize.y;
    float videoAspect = vW / vH;

    if (videoAspect > screenAspect) {
        renderSize.y = screenSize.x / videoAspect;
        renderPos.y = (screenSize.y - renderSize.y) * 0.5f;
    }
    else {
        renderSize.x = screenSize.y * videoAspect;
        renderPos.x = (screenSize.x - renderSize.x) * 0.5f;
    }

    // Draw the letterbox background regardless of engine
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), screenSize, IM_COL32(0, 0, 0, 255));

    if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) {
        RenderCPlayer(renderPos, renderSize);
    }
    else {
        // This call MUST run while loading: it uploads libVLC's decoded back
        // buffer into the D3D11 texture and creates the first SRV.
        RenderVLCPlayer(renderPos, renderSize);
    }

    // ------------------------------------------------------------
    // PREWARM PLAYER UI WHILE THE LOADING SHADER IS STILL COVERING IT
    // ------------------------------------------------------------
    // Previously these renderers were created only after the loading overlay
    // returned. That guaranteed one or more visible "video-only" frames before
    // the header/glass UI appeared. Build and render the top-card texture now so
    // loader completion means BOTH media and the player UI are actually ready.
    if (!s_TopCardRenderer) {
        dynhtml::SurfaceConfig cfg;
        cfg.useHardwareAcceleration = true;
        s_TopCardRenderer = new dynhtml::HtmlRenderer(cfg);
        s_TopCardRenderer->InitializeDX11(g_pd3dDevice, g_pd3dDeviceContext);
        s_TopCardRenderer->SetHTML(R"(<div style="width: 100%; height: 100%; background: rgba(17, 24, 39, 0.8); backdrop-filter: blur(12px); border-bottom: 1px solid rgba(255, 255, 255, 0.08);"></div>)");
    }

    ID3D11ShaderResourceView* bgSrv =
        (g_Settings.engineMode == 1)
        ? GetVLCShaderResourceView()
        : GetCPlayerShaderResourceView();

    bool playerUiReady = false;
    if (s_TopCardRenderer) {
        s_TopCardRenderer->Tick(io.DeltaTime);
        s_TopCardRenderer->SetViewport((int)screenSize.x, (int)headerHeight);
        s_TopCardRenderer->SetBackdropTexture(
            bgSrv,
            (int)screenSize.x,
            (int)screenSize.y);
        s_TopCardRenderer->SetBackdropVideoRect(
            renderPos.x,
            renderPos.y,
            renderSize.x,
            renderSize.y);
        s_TopCardRenderer->SetBackdropAmbientEffect(
            static_cast<float>(ImGui::GetTime()),
            0.62f * std::max(s_controlsAlpha, 0.35f));

        const bool rendered = s_TopCardRenderer->Render();
        playerUiReady =
            rendered &&
            s_TopCardRenderer->TextureHandle() != nullptr;
    }

    // Decide readiness only AFTER both the engine and the glass UI had a chance
    // to produce their current-frame GPU textures.
    const bool mediaStillLoading = usingNative
        ? (!CPlayerHasError() && !CPlayerHasUsableMedia())
        : (GetVLCShaderResourceView() == nullptr);

    const bool bridgeMediaReady = hasValidDuration &&
        (usingNative
            ? (!CPlayerHasError() && CPlayerHasUsableMedia())
            : (GetVLCShaderResourceView() != nullptr));
    SyncMovieBoxPlaybackState(engineDuration, bridgeMediaReady);

    const bool holdLoadingOverlay =
        mediaStillLoading || !playerUiReady;

    if (holdLoadingOverlay) {
        s_loadingOverlayAlpha = 1.0f;
    }
    else {
        // The first frame and UI texture are already prepared behind the loader.
        LerpFade(s_loadingOverlayAlpha, 0.0f, 8.5f);
    }

    if (s_loadingOverlayAlpha > 0.01f) {
        std::string bridgeLoadingText;
        const char* loadingText = nullptr;
        if (g_Settings.bridgeMode && g_Settings.currentUrl.empty()) {
            bridgeLoadingText = MovieBoxBridgeLastError();
            if (bridgeLoadingText.empty()) {
                bridgeLoadingText = MovieBoxBridgeConnected()
                    ? "Waiting for MovieBoxPro..."
                    : "Connecting to MovieBoxPro...";
            }
            loadingText = bridgeLoadingText.c_str();
        }
        else {
            loadingText = usingNative
                ? GetCPlayerStatusText()
                : "Starting VLC playback...";
        }

        RenderPlayerLoadingScreen(loadingText, s_loadingOverlayAlpha);

        // The active engine has already been ticked above. Suppress the rest of
        // the controls until the overlay has faded so nothing draws over it.
        ImGui::End();
        return;
    }

    bool anyKeyActivity = false;

    // Keyboard Shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        TogglePlay();
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !g_IsPIPMode) {
        SetTimer(hWnd, 1002, 10, MaximizeTimerProc);
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        SetTimer(hWnd, 1001, 10, PipTimerProc);
        s_showSubMenu = s_showServerMenu = s_showEpPanel = s_showContextMenu = false;
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        ToggleMute();
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && hasValidDuration) {
        g_Settings.progress = std::clamp(g_Settings.progress + (10.0f / engineDuration), 0.0f, 1.0f);
        HardwareSeek(); TriggerIndicator(3, "10s");
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && hasValidDuration) {
        g_Settings.progress = std::clamp(g_Settings.progress - (10.0f / engineDuration), 0.0f, 1.0f);
        HardwareSeek(); TriggerIndicator(4, "10s");
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        g_Settings.volume = std::clamp(g_Settings.volume + 0.05f, 0.0f, 2.0f);
        if (g_Settings.volume > 0.0f) s_lastVolume = g_Settings.volume;
        g_Settings.muted = g_Settings.volume <= 0.0001f;
        g_ActiveProfile.defaultVolume = g_Settings.volume;
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(g_Settings.volume);
        else SetVLCVolume(g_Settings.volume);
        TriggerIndicator(5, std::to_string((int)(std::round(g_Settings.volume * 100))) + "%");
        anyKeyActivity = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        g_Settings.volume = std::clamp(g_Settings.volume - 0.05f, 0.0f, 2.0f);
        g_Settings.muted = g_Settings.volume <= 0.0001f;
        g_ActiveProfile.defaultVolume = g_Settings.volume;
        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(g_Settings.volume);
        else SetVLCVolume(g_Settings.volume);
        TriggerIndicator(5, g_Settings.volume <= 0.0f ? "Muted" : std::to_string((int)(std::round(g_Settings.volume * 100))) + "%");
        anyKeyActivity = true;
    }

    if (anyKeyActivity) WakeUpUI();

    // 5. DETERMINISTIC FADE & TICK MACHINE
    bool anyMenuOpen = s_showServerMenu || s_showSubMenu || s_showEpPanel || s_showContextMenu || s_seasonDropdownOpen;

    if (anyMenuOpen || !g_Settings.isPlaying) {
        WakeUpUI(); // Hard lock the timer to max if a menu is open or paused!
    }

    if (s_isAwake) {
        s_sleepTimer -= io.DeltaTime;
        if (s_sleepTimer <= 0.0f) {
            s_sleepTimer = 0.0f;
            s_isAwake = false;
        }
    }

    float targetAlpha = s_isAwake ? 1.0f : 0.0f;
    float safeDt = std::min(io.DeltaTime, 0.05f);

    if (s_controlsAlpha < targetAlpha) {
        s_controlsAlpha = std::min(s_controlsAlpha + (safeDt * 6.0f), 1.0f);
    }
    else if (s_controlsAlpha > targetAlpha) {
        s_controlsAlpha = std::max(s_controlsAlpha - (safeDt * 4.0f), 0.0f);
    }

    // MovieBox subtitle cues belong to the media, not the chrome. Draw them
    // before the controls-hidden early return so captions remain visible while
    // the player UI fades away.
    if (g_Settings.bridgeMode && hasValidDuration) {
        RenderMovieBoxSubtitles(drawList, screenSize, static_cast<double>(g_Settings.progress) * engineDuration);
    }

    // Optimized Render Skip (Hitboxes still function because our global click dispatcher is above!)
    if (s_controlsAlpha <= 0.001f && !anyMenuOpen && s_indicatorTimer <= 0.0f) {
        ImGui::End();
        return;
    }

    // Keep a restrained version of the procedural loading field alive underneath
    // the floating UI. Because this is drawn after the video frame and before the
    // glass/header/footer controls, it reads through those elements without
    // obscuring the movie.
    const float ambientShaderAlpha = 0.045f * s_controlsAlpha;
    DrawLoadingShader(
        drawList,
        ImVec2(0.0f, 0.0f),
        screenSize,
        static_cast<float>(ImGui::GetTime()),
        ambientShaderAlpha);

    ImU32 textCol = IM_COL32(255, 255, 255, (int)(255 * s_controlsAlpha));
    ImU32 textDimCol = IM_COL32(255, 255, 255, (int)(180 * s_controlsAlpha));
    ImU32 purpleAccent = IM_COL32(139, 92, 246, (int)(255 * s_controlsAlpha));
    ImU32 lavenderAccent = IM_COL32(180, 150, 255, (int)(255 * s_controlsAlpha));
    ImU32 iconCol = IM_COL32(255, 255, 255, (int)(180 * s_controlsAlpha));


    const float mistTime = static_cast<float>(ImGui::GetTime());
    auto DrawMistCapsule = [&](ImVec2 coreStart, ImVec2 coreEnd, float coreRadius, float feather, float intensity, float alphaScale = 1.0f) {
        if (intensity <= 0.001f) return;

        // This is intentionally NOT a rectangle. The carrier geometry is a thick
        // stroked line plus circular end caps, and the HLSL capsule SDF reaches
        // zero alpha before the carrier boundary.
        const float length = std::max(coreEnd.x - coreStart.x, 0.0f);
        const ImVec2 center(
            (coreStart.x + coreEnd.x) * 0.5f,
            (coreStart.y + coreEnd.y) * 0.5f);

        const float capsuleHalfLength =
            std::max(length * 0.5f - coreRadius, 0.0f);

        const ImVec2 axisStart(center.x - capsuleHalfLength, center.y);
        const ImVec2 axisEnd(center.x + capsuleHalfLength, center.y);

        PushMistGlowShape(
            drawList,
            IM_COL32(139, 92, 246, 255),
            intensity,
            mistTime,
            center,
            ImVec2(capsuleHalfLength, coreRadius),
            coreRadius,
            feather,
            2.0f);

        const float carrierRadius = coreRadius + feather + 4.0f;
        const ImU32 carrierColor =
            IM_COL32(255, 255, 255, (int)(126 * alphaScale));

        if (capsuleHalfLength > 0.5f) {
            drawList->PathClear();
            drawList->PathLineTo(axisStart);
            drawList->PathLineTo(axisEnd);
            drawList->PathStroke(
                carrierColor,
                0,
                carrierRadius * 2.0f);
        }

        drawList->AddCircleFilled(
            axisStart,
            carrierRadius,
            carrierColor,
            32);
        drawList->AddCircleFilled(
            axisEnd,
            carrierRadius,
            carrierColor,
            32);

        PopMistGlow(drawList);
    };

    auto DrawMistCircle = [&](ImVec2 center, float coreRadius, float feather, float intensity, float alphaScale = 1.0f) {
        if (intensity <= 0.001f) return;
        PushMistGlowShape(drawList, IM_COL32(139, 92, 246, 255), intensity, mistTime, center, ImVec2(coreRadius, coreRadius), coreRadius, feather, 1.0f);
        // Leave a transparent margin outside the SDF feather so the carrier
        // circle itself can never become the visible cutoff.
        drawList->AddCircleFilled(center, coreRadius + feather + 4.0f, IM_COL32(255, 255, 255, (int)(96 * alphaScale)));
        drawList->AddCircleFilled(center, coreRadius + feather * 0.35f, IM_COL32(255, 255, 255, (int)(170 * alphaScale)));
        PopMistGlow(drawList);
    };

    auto DrawMistIcon = [&](const char* iconName, ImVec2 iconPos, float iconSize, float thickness, float intensity, float alphaScale = 1.0f) {
        if (intensity <= 0.001f) return;

        const ImVec2 center(
            iconPos.x + iconSize * 0.5f,
            iconPos.y + iconSize * 0.5f);
        const float coreRadius = iconSize * 0.48f;
        const float feather = iconSize * 0.72f;

        PushMistGlowShape(
            drawList,
            IM_COL32(139, 92, 246, 255),
            intensity,
            mistTime,
            center,
            ImVec2(coreRadius, coreRadius),
            coreRadius,
            feather,
            1.0f);

        // Use the ACTUAL Lucide vector paths as the carrier. Multiple expanded
        // strokes create the plume around the icon silhouette; no background
        // rectangle/circle is involved.
        const float plumeWidths[4] = {
            thickness + feather * 1.45f,
            thickness + feather * 0.92f,
            thickness + feather * 0.52f,
            thickness + feather * 0.20f
        };
        const float plumeAlpha[4] = { 22.0f, 52.0f, 98.0f, 168.0f };

        for (int layer = 0; layer < 4; ++layer) {
            Lucide::IconProps maskProps;
            maskProps.size = iconSize;
            maskProps.color = IM_COL32(
                255, 255, 255,
                (int)(plumeAlpha[layer] * alphaScale));
            maskProps.thickness = plumeWidths[layer];
            maskProps.hoverThickness = plumeWidths[layer];
            maskProps.animated = false;

            ImGui::SetCursorScreenPos(iconPos);
            Lucide::Icon(iconName, maskProps);
        }

        PopMistGlow(drawList);
    };

    // 6. HTML RENDERED HEADER BACKGROUND
    // The texture was already prepared above, underneath the loading overlay.
    // From here onward we only composite that ready texture into the visible UI.
    if (s_controlsAlpha > 0.001f) {
        if (void* htmlTex = s_TopCardRenderer ? s_TopCardRenderer->TextureHandle() : nullptr) {
            drawList->AddImage((ImTextureID)htmlTex, ImVec2(0, 0), ImVec2(screenSize.x, headerHeight), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, (int)(255 * s_controlsAlpha)));
        }
        else {
            drawList->AddRectFilled(ImVec2(0, 0), ImVec2(screenSize.x, headerHeight), IM_COL32(17, 24, 39, (int)(204 * s_controlsAlpha)));
        }

        drawList->AddRectFilledMultiColor(
            ImVec2(0, headerHeight), ImVec2(screenSize.x, headerHeight + 16.0f),
            IM_COL32(0, 0, 0, (int)(50.0f * s_controlsAlpha)), IM_COL32(0, 0, 0, (int)(50.0f * s_controlsAlpha)),
            IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0)
        );
    }

    // --- LEFT SIDE HEADER: TYPOGRAPHY ---
    ImGui::SetCursorScreenPos(ImVec2(headerPaddingLeft, headerPaddingTop));
    if (ImGui::InvisibleButton("##Back", ImVec2(40.0f, 40.0f))) {
        if (g_Settings.videoPlayerQuits) {
            // VPQ = Video Player Quits. Post WM_CLOSE so shutdown happens through
            // the normal message-loop cleanup path after this frame is finished.
            if (g_ActiveDetails) {
                MockMedia historySnapshot = *g_ActiveDetails;
                historySnapshot.historyStats.progress = g_Settings.progress;
                historySnapshot.historyStats.season = g_Settings.activeSeason;
                historySnapshot.historyStats.url = g_Settings.currentUrl;
                auto& historyVec = g_ActiveProfile.history;
                historyVec.erase(std::remove_if(historyVec.begin(), historyVec.end(), [&](const MockMedia& m) { return m.title == historySnapshot.title; }), historyVec.end());
                historyVec.insert(historyVec.begin(), historySnapshot);
            }
            g_ActiveDetails = nullptr;
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
        else if (g_IsPIPMode) {
            SetTimer(hWnd, 1001, 10, PipTimerProc);
        }
        else {
            if (g_ActiveDetails) {
                MockMedia historySnapshot = *g_ActiveDetails;
                historySnapshot.historyStats.progress = g_Settings.progress;
                historySnapshot.historyStats.season = g_Settings.activeSeason;
                historySnapshot.historyStats.url = g_Settings.currentUrl;
                auto& historyVec = g_ActiveProfile.history;
                historyVec.erase(std::remove_if(historyVec.begin(), historyVec.end(), [&](const MockMedia& m) { return m.title == historySnapshot.title; }), historyVec.end());
                historyVec.insert(historyVec.begin(), historySnapshot);
            }
            g_ActiveDetails = nullptr;
            s_pendingExitToDashboard = true; // Queue the safe exit!
        }
    }

    ImVec2 backCenter(headerPaddingLeft + 20.0f, headerPaddingTop + 20.0f);
    drawList->AddCircleFilled(backCenter, 20.0f, ImGui::IsItemHovered() ? IM_COL32(255, 255, 255, (int)(64 * s_controlsAlpha)) : IM_COL32(255, 255, 255, (int)(25 * s_controlsAlpha)));
    drawList->AddCircle(backCenter, 20.0f, IM_COL32(255, 255, 255, (int)(38 * s_controlsAlpha)), 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(backCenter.x - 12.0f, backCenter.y - 12.0f));
    Lucide::IconProps backProps;
    backProps.size = 24.0f; backProps.color = textCol; backProps.thickness = 2.0f; backProps.hoverThickness = 2.5f; backProps.animated = true;
    Lucide::Icon("X", backProps);

    float textX = headerPaddingLeft + 40.0f + 16.0f;
    float textY = headerPaddingTop + 4.0f;

    ImGui::SetWindowFontScale(g_IsPIPMode ? 0.65f : 0.85f);
    drawList->AddText(ImVec2(textX, textY), purpleAccent, g_Settings.currentUrl.find("file://") == 0 ? "LOCAL MEDIA" : "CURRENTLY PLAYING");
    ImGui::SetWindowFontScale(g_IsPIPMode ? 0.85f : 1.15f);

    if (g_IsPIPMode) drawList->PushClipRect(ImVec2(textX, textY + 12.0f), ImVec2(screenSize.x - 40.0f, headerHeight), true);
    drawList->AddText(ImVec2(textX, textY + (g_IsPIPMode ? 12.0f : 14.0f)), textCol, g_Settings.videoTitle.c_str());
    if (g_IsPIPMode) drawList->PopClipRect();

    ImGui::SetWindowFontScale(1.0f);

    std::vector<SubTrack> engineSubs;
    if (g_Settings.engineMode == 1) engineSubs = GetVLCSpuTracks();
    else engineSubs.push_back({ -1, "Disable" });

    const bool bridgeEpisodePanelAvailable =
        g_Settings.bridgeMode && s_bridgeMetadataValid && s_bridgeMetadata.isSeries;
    const bool bridgeSubtitleMenuAvailable =
        g_Settings.bridgeMode && s_bridgeMetadataValid && !s_bridgeMetadata.subtitles.empty();
    const bool hasSubs = g_Settings.bridgeMode
        ? bridgeSubtitleMenuAvailable
        : engineSubs.size() > 1;

    // --- RIGHT SIDE HEADER: ICONS ---
    float rightBtnW = 36.0f, rightGap = 4.0f;
    float curRX = screenSize.x - headerPaddingRight;

    auto DrawHeaderIcon = [&](const char* id, const char* iconName, bool& menuState, bool forceActive = false) {
        curRX -= rightBtnW;
        ImGui::SetCursorScreenPos(ImVec2(curRX, headerPaddingTop + 2.0f));
        if (ImGui::InvisibleButton(id, ImVec2(rightBtnW, rightBtnW))) menuState = !menuState;

        const bool iconHot = ImGui::IsItemHovered() || menuState || forceActive;
        if (iconHot) {
            DrawMistIcon(iconName, ImVec2(curRX + 8.0f, headerPaddingTop + 10.0f), 20.0f, 2.0f, 1.08f * s_controlsAlpha, s_controlsAlpha);
        }

        Lucide::IconProps rProps;
        rProps.size = 20.0f;
        rProps.color = iconHot ? lavenderAccent : iconCol;
        rProps.thickness = 2.0f;
        rProps.hoverThickness = 2.5f;
        rProps.animated = true;

        ImGui::SetCursorScreenPos(ImVec2(curRX + 8.0f, headerPaddingTop + 10.0f));
        Lucide::Icon(iconName, rProps);
        curRX -= rightGap;
        };

    bool pipDummy = false;
    DrawHeaderIcon("##PIP", "MonitorPlay", pipDummy, g_IsPIPMode);
    if (pipDummy) {
        SetTimer(hWnd, 1001, 10, PipTimerProc);
        s_showSubMenu = s_showServerMenu = s_showEpPanel = s_showContextMenu = false;
    }

    if (!g_IsPIPMode) {
        if (bridgeEpisodePanelAvailable ||
            (!g_Settings.bridgeMode && g_ActiveDetails && (g_ActiveDetails->type == "Series" || !g_ActiveDetails->seasons.empty()))) {
            bool oldEp = s_showEpPanel;
            DrawHeaderIcon("##Episodes", "ListVideo", s_showEpPanel);
            if (s_showEpPanel && !oldEp) { s_showSubMenu = s_showServerMenu = s_showContextMenu = false; }
        }
        if (hasSubs) {
            bool oldSub = s_showSubMenu;
            DrawHeaderIcon("##Subtitles", "Captions", s_showSubMenu);
            if (s_showSubMenu && !oldSub) { s_showEpPanel = s_showServerMenu = s_showContextMenu = false; }
        }
        else {
            s_showSubMenu = false;
        }

        bool oldSet = s_showServerMenu;
        DrawHeaderIcon("##Settings", "Settings", s_showServerMenu);
        if (s_showServerMenu && !oldSet) { s_showSubMenu = s_showEpPanel = s_showContextMenu = false; }
    }

    // 7. GLASSMORPHIC FOOTER CONTROLS
    ImU32 botGradTop = IM_COL32(0, 0, 0, 0);
    ImU32 botGradBot1 = IM_COL32(0, 0, 0, (int)((g_IsPIPMode ? 100 : 180) * s_controlsAlpha));
    ImU32 botGradBot2 = IM_COL32(0, 0, 0, (int)((g_IsPIPMode ? 150 : 240) * s_controlsAlpha));

    drawList->AddRectFilledMultiColor(ImVec2(0, screenSize.y - 150), ImVec2(screenSize.x, screenSize.y - 60), botGradTop, botGradTop, botGradBot1, botGradBot1);
    drawList->AddRectFilledMultiColor(ImVec2(0, screenSize.y - 60), ImVec2(screenSize.x, screenSize.y), botGradBot1, botGradBot1, botGradBot2, botGradBot2);

    float controlY = screenSize.y - (g_IsPIPMode ? 50.0f : 70.0f);
    float trackY = controlY - 25.0f;
    float paddingX = g_IsPIPMode ? 16.0f : 32.0f;
    float trackWidth = screenSize.x - (paddingX * 2);

    ImRect progressBB(ImVec2(paddingX, trackY), ImVec2(paddingX + trackWidth, trackY + 10));
    ImGui::SetCursorScreenPos(progressBB.Min);

    ImGui::InvisibleButton("##Scrubber", progressBB.GetSize());
    bool scrubHovered = ImGui::IsItemHovered();
    bool scrubHeld = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (scrubHeld && hasValidDuration) {
        g_Settings.progress = std::clamp((io.MousePos.x - progressBB.Min.x) / trackWidth, 0.0f, 1.0f);
        s_pendingStartProgress = -1.0f;
        HardwareSeek();
    }
    else if (s_pendingStartProgress > 0.001f) {
        float realProg = (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) ? GetCPlayerProgress() : GetVLCPlayerProgress();
        if (realEngineDuration > 0.0f && realProg >= 0.0f) {
            g_Settings.progress = s_pendingStartProgress;
            HardwareSeek();
            s_pendingStartProgress = -1.0f;
        }
        else {
            g_Settings.progress = s_pendingStartProgress;
        }
    }
    else if (g_Settings.isPlaying) {
        float realProg = (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) ? GetCPlayerProgress() : GetVLCPlayerProgress();
        if (realProg >= 0.0f) g_Settings.progress = realProg;
        // No synthetic clock: if the engine has not reported a real duration/progress
        // yet, hold the scrubber still instead of inventing playback state.
    }

    static float s_scrubBarH = 4.0f;
    LerpFade(s_scrubBarH, scrubHovered || scrubHeld ? 6.0f : 4.0f, 15.0f);
    float curTrackY = trackY + (10.0f - s_scrubBarH) / 2.0f;

    drawList->AddRectFilled(ImVec2(paddingX, curTrackY), ImVec2(paddingX + trackWidth, curTrackY + s_scrubBarH), IM_COL32(255, 255, 255, (int)(50 * s_controlsAlpha)), 2.0f);

    if (g_Settings.progress > 0.0001f) {
        DrawMistCapsule(
            ImVec2(paddingX, curTrackY + s_scrubBarH * 0.5f),
            ImVec2(
                paddingX + g_Settings.progress * trackWidth,
                curTrackY + s_scrubBarH * 0.5f),
            s_scrubBarH * 0.5f,
            18.0f,
            0.95f * s_controlsAlpha,
            s_controlsAlpha);
    }
    drawList->AddRectFilled(ImVec2(paddingX, curTrackY), ImVec2(paddingX + g_Settings.progress * trackWidth, curTrackY + s_scrubBarH), purpleAccent, 2.0f);

    static float s_scrubRadius = 5.0f;
    static float s_scrubGlowAlpha = 0.0f;

    LerpFade(s_scrubRadius, scrubHovered || scrubHeld ? 7.0f : 5.0f, 20.0f);
    LerpFade(s_scrubGlowAlpha, scrubHovered || scrubHeld ? 1.0f : 0.0f, 15.0f);

    ImVec2 handlePos(paddingX + g_Settings.progress * trackWidth, trackY + 5.0f);

    if (s_scrubGlowAlpha > 0.01f) {
        DrawMistCircle(
            handlePos,
            s_scrubRadius,
            15.0f,
            1.35f * s_scrubGlowAlpha * s_controlsAlpha,
            s_scrubGlowAlpha * s_controlsAlpha);
    }

    drawList->AddCircleFilled(ImVec2(handlePos.x, handlePos.y + 1.0f), s_scrubRadius, IM_COL32(0, 0, 0, (int)(100 * s_controlsAlpha)));
    drawList->AddCircleFilled(handlePos, s_scrubRadius, purpleAccent);

    if (scrubHovered || scrubHeld) {
        float hoverProg = std::clamp((io.MousePos.x - progressBB.Min.x) / trackWidth, 0.0f, 1.0f);
        int hoverSecs = hasValidDuration ? (int)(hoverProg * engineDuration) : 0;
        char ttStr[32];
        if (hasValidDuration) snprintf(ttStr, sizeof(ttStr), "%d:%02d", hoverSecs / 60, hoverSecs % 60);
        else snprintf(ttStr, sizeof(ttStr), "--:--");
        ImVec2 ttSize = ImGui::CalcTextSize(ttStr);
        ImVec2 ttPos(progressBB.Min.x + hoverProg * trackWidth, trackY - 25.0f);

        drawList->AddRectFilled(ImVec2(ttPos.x - ttSize.x / 2.0f - 8.0f, ttPos.y - 4.0f), ImVec2(ttPos.x + ttSize.x / 2.0f + 8.0f, ttPos.y + ttSize.y + 4.0f), IM_COL32(15, 18, 26, (int)(245 * s_controlsAlpha)), 4.0f);
        drawList->AddRect(ImVec2(ttPos.x - ttSize.x / 2.0f - 8.0f, ttPos.y - 4.0f), ImVec2(ttPos.x + ttSize.x / 2.0f + 8.0f, ttPos.y + ttSize.y + 4.0f), IM_COL32(139, 92, 246, (int)(100 * s_controlsAlpha)), 4.0f);
        drawList->AddText(ImVec2(ttPos.x - ttSize.x / 2.0f, ttPos.y), IM_COL32(255, 255, 255, (int)(255 * s_controlsAlpha)), ttStr);
    }

    ImGui::SetCursorScreenPos(ImVec2(paddingX, controlY));
    if (ImGui::InvisibleButton("##Play", ImVec2(40, 40))) TogglePlay();
    const bool playHovered = ImGui::IsItemHovered();

    if (s_targetIsPlaying != g_Settings.isPlaying) {
        s_targetIsPlaying = g_Settings.isPlaying;
        s_isSwappingIcon = true;
    }

    if (s_isSwappingIcon) {
        s_playPauseAlpha = std::max(s_playPauseAlpha - (io.DeltaTime / 0.15f), 0.0f);
        if (s_playPauseAlpha <= 0.0f) {
            s_lastIsPlaying = s_targetIsPlaying;
            s_isSwappingIcon = false;
        }
    }
    else {
        s_playPauseAlpha = std::min(s_playPauseAlpha + (io.DeltaTime / 0.15f), 1.0f);
    }

    Lucide::IconProps playProps;
    playProps.size = 24.0f;
    playProps.thickness = 2.0f;
    playProps.hoverThickness = 2.5f;
    playProps.animated = true;
    playProps.color = IM_COL32(255, 255, 255, (int)(255 * s_controlsAlpha * s_playPauseAlpha));

    DrawMistIcon(
        s_lastIsPlaying ? "Pause" : "Play",
        ImVec2(paddingX + 8.0f, controlY + 8.0f),
        24.0f,
        2.0f,
        (playHovered ? 1.30f : 0.62f) * s_controlsAlpha * s_playPauseAlpha,
        s_controlsAlpha * s_playPauseAlpha);

    ImGui::SetCursorScreenPos(ImVec2(paddingX + 8.0f, controlY + 8.0f));
    Lucide::Icon(s_lastIsPlaying ? "Pause" : "Play", playProps);

    int curSecs = hasValidDuration ? (int)(g_Settings.progress * engineDuration) : 0;
    int totSecs = hasValidDuration ? (int)engineDuration : 0;
    int remSecs = hasValidDuration ? std::max(0, totSecs - curSecs) : 0;

    static int s_timeFormat = 0; // 0: Standard, 1: T-minus, 2: Natural
    char timeStr[64];

    if (!hasValidDuration) {
        snprintf(timeStr, sizeof(timeStr), "--:-- / --:--");
    }
    else if (s_timeFormat == 0) {
        snprintf(timeStr, sizeof(timeStr), "%d:%02d / %d:%02d", curSecs / 60, curSecs % 60, totSecs / 60, totSecs % 60);
    }
    else if (s_timeFormat == 1) {
        snprintf(timeStr, sizeof(timeStr), "-%d:%02d / %d:%02d", remSecs / 60, remSecs % 60, remSecs / 60, remSecs % 60);
    }
    else {
        snprintf(timeStr, sizeof(timeStr), "%d:%02d / %d:%02d", curSecs / 60, curSecs % 60, remSecs / 60, remSecs % 60);
    }

    ImVec2 tSize = ImGui::CalcTextSize(timeStr);
    ImVec2 tPos(paddingX + (g_IsPIPMode ? 50.0f : 60.0f), controlY + 12);

    // Create bounding box for our invisible pill button
    ImRect tRect(tPos.x - 8, tPos.y - 4, tPos.x + tSize.x + 8, tPos.y + tSize.y + 4);

    ImGui::SetCursorScreenPos(tRect.Min);
    if (ImGui::InvisibleButton("##TimeFormatToggle", tRect.GetSize())) {
        s_timeFormat = (s_timeFormat + 1) % 3;
    }

    static float s_timeHov = 0.0f;
    LerpFade(s_timeHov, ImGui::IsItemHovered() ? 1.0f : 0.0f, 15.0f);

    if (s_timeHov > 0.01f) {
        drawList->AddRectFilled(tRect.Min, tRect.Max, IM_COL32(255, 255, 255, (int)(15 * s_timeHov * s_controlsAlpha)), 6.0f);
    }

    // Lerp text color from dim (180) to bright white (255) based on hover state
    ImU32 tCol = IM_COL32(
        180 + (int)(75 * s_timeHov),
        180 + (int)(75 * s_timeHov),
        180 + (int)(75 * s_timeHov),
        (int)(255 * s_controlsAlpha)
    );

    drawList->AddText(tPos, tCol, timeStr);

    float volSliderW = g_IsPIPMode ? 90.0f : 180.0f;
    float rightGrpWidth = 40.0f + 5.0f + volSliderW + 15.0f + (g_IsPIPMode ? 0.0f : 40.0f);
    float curVolX = screenSize.x - paddingX - rightGrpWidth;

    ImGui::SetCursorScreenPos(ImVec2(curVolX, controlY));
    if (ImGui::InvisibleButton("##Vol", ImVec2(40, 40))) ToggleMute();
    const bool volumeButtonHovered = ImGui::IsItemHovered();

    if (volumeButtonHovered) {
        DrawMistIcon(g_Settings.volume <= 0.0f ? "VolumeX" : "Volume2", ImVec2(curVolX + 10.0f, controlY + 10.0f), 20.0f, 2.0f, 1.08f * s_controlsAlpha, s_controlsAlpha);
    }

    ImGui::SetCursorScreenPos(ImVec2(curVolX + 10.0f, controlY + 10.0f));
    Lucide::IconProps volProps;
    volProps.size = 20.0f;
    volProps.color = volumeButtonHovered ? lavenderAccent : textCol;
    volProps.thickness = 2.0f;
    volProps.hoverThickness = 2.5f;
    volProps.animated = true;
    Lucide::Icon(g_Settings.volume <= 0.0f ? "VolumeX" : "Volume2", volProps);

    curVolX += 45.0f;
    ImVec2 sliderStart(curVolX, controlY + 14.0f);
    ImGui::SetCursorScreenPos(sliderStart);
    ImGui::InvisibleButton("##VolSlider", ImVec2(volSliderW, 12));
    bool volHovered = ImGui::IsItemHovered();
    bool volHeld = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);

    if (volHeld) {
        g_Settings.volume = std::clamp(((io.MousePos.x - sliderStart.x) / volSliderW) * 2.0f, 0.0f, 2.0f);
        if (g_Settings.volume > 0.0f) s_lastVolume = g_Settings.volume;
        g_ActiveProfile.defaultVolume = g_Settings.volume;

        if (g_Settings.engineMode == 0 || g_Settings.engineMode == 2) SetCPlayerVolume(g_Settings.volume);
        else SetVLCVolume(g_Settings.volume);
    }

    static float s_volBarH = 4.0f;
    LerpFade(s_volBarH, volHovered || volHeld ? 6.0f : 4.0f, 15.0f);
    float curVolTrackY = sliderStart.y + (12.0f - s_volBarH) / 2.0f;

    drawList->AddRectFilled(ImVec2(sliderStart.x, curVolTrackY), ImVec2(sliderStart.x + volSliderW, curVolTrackY + s_volBarH), IM_COL32(255, 255, 255, (int)(50 * s_controlsAlpha)), 2.0f);
    if (g_Settings.volume > 0.0001f) {
        DrawMistCapsule(
            ImVec2(sliderStart.x, curVolTrackY + s_volBarH * 0.5f),
            ImVec2(
                sliderStart.x + (g_Settings.volume / 2.0f) * volSliderW,
                curVolTrackY + s_volBarH * 0.5f),
            s_volBarH * 0.5f,
            18.0f,
            0.88f * s_controlsAlpha,
            s_controlsAlpha);
    }
    drawList->AddRectFilled(ImVec2(sliderStart.x, curVolTrackY), ImVec2(sliderStart.x + (g_Settings.volume / 2.0f) * volSliderW, curVolTrackY + s_volBarH), purpleAccent, 2.0f);

    static float s_volRadius = 5.0f;
    static float s_volGlowAlpha = 0.0f;

    LerpFade(s_volRadius, volHovered || volHeld ? 7.0f : 5.0f, 20.0f);
    LerpFade(s_volGlowAlpha, volHovered || volHeld ? 1.0f : 0.0f, 15.0f);

    ImVec2 volHandlePos(sliderStart.x + (g_Settings.volume / 2.0f) * volSliderW, sliderStart.y + 6.0f);

    if (s_volGlowAlpha > 0.01f) {
        DrawMistCircle(
            volHandlePos,
            s_volRadius,
            15.0f,
            1.25f * s_volGlowAlpha * s_controlsAlpha,
            s_volGlowAlpha * s_controlsAlpha);
    }

    drawList->AddCircleFilled(ImVec2(volHandlePos.x, volHandlePos.y + 1.0f), s_volRadius, IM_COL32(0, 0, 0, (int)(100 * s_controlsAlpha)));
    drawList->AddCircleFilled(volHandlePos, s_volRadius, purpleAccent);

    if (volHovered || volHeld) {
        float hoverVol = volHeld ? g_Settings.volume : std::clamp(((io.MousePos.x - sliderStart.x) / volSliderW) * 2.0f, 0.0f, 2.0f);
        char ttStr[16];
        snprintf(ttStr, sizeof(ttStr), "%d%%", (int)(hoverVol * 100));
        ImVec2 ttSize = ImGui::CalcTextSize(ttStr);
        ImVec2 ttPos(sliderStart.x + (hoverVol / 2.0f) * volSliderW, sliderStart.y - 25.0f);

        drawList->AddRectFilled(ImVec2(ttPos.x - ttSize.x / 2.0f - 8.0f, ttPos.y - 4.0f), ImVec2(ttPos.x + ttSize.x / 2.0f + 8.0f, ttPos.y + ttSize.y + 4.0f), IM_COL32(15, 18, 26, (int)(245 * s_controlsAlpha)), 4.0f);
        drawList->AddRect(ImVec2(ttPos.x - ttSize.x / 2.0f - 8.0f, ttPos.y - 4.0f), ImVec2(ttPos.x + ttSize.x / 2.0f + 8.0f, ttPos.y + ttSize.y + 4.0f), IM_COL32(139, 92, 246, (int)(100 * s_controlsAlpha)), 4.0f);
        drawList->AddText(ImVec2(ttPos.x - ttSize.x / 2.0f, ttPos.y), IM_COL32(255, 255, 255, (int)(255 * s_controlsAlpha)), ttStr);
    }
    curVolX += volSliderW + 15.0f;

    if (!g_IsPIPMode) {
        ImGui::SetCursorScreenPos(ImVec2(curVolX, controlY));
        if (ImGui::InvisibleButton("##Fullscreen", ImVec2(40, 40))) SetTimer(hWnd, 1002, 10, MaximizeTimerProc);
        const bool maximizeHovered = ImGui::IsItemHovered();

        if (maximizeHovered) {
            DrawMistIcon("Maximize", ImVec2(curVolX + 10.0f, controlY + 10.0f), 20.0f, 2.0f, 1.08f * s_controlsAlpha, s_controlsAlpha);
        }

        ImGui::SetCursorScreenPos(ImVec2(curVolX + 10.0f, controlY + 10.0f));
        Lucide::IconProps maxProps;
        maxProps.size = 20.0f;
        maxProps.color = maximizeHovered ? lavenderAccent : textCol;
        maxProps.thickness = 2.0f;
        maxProps.hoverThickness = 2.5f;
        maxProps.animated = true;
        Lucide::Icon("Maximize", maxProps);
    }

    // --- TOAST INDICATOR DRAW LOGIC ---
    if (s_indicatorTimer > 0.0f) {
        s_indicatorTimer -= io.DeltaTime;
        float aPulse = std::clamp(s_indicatorTimer / 0.2f, 0.0f, 1.0f);
        if (s_indicatorTimer > 0.7f) aPulse = std::clamp((0.8f - s_indicatorTimer) / 0.1f, 0.0f, 1.0f);

        ImVec2 center(screenSize.x * 0.5f, screenSize.y * 0.5f);
        drawList->AddCircleFilled(center, 40.0f, IM_COL32(0, 0, 0, (int)(160 * aPulse)));

        bool hasText = !s_indicatorText.empty();
        float iconY = hasText ? center.y - 20.0f : center.y - 16.0f;

        ImGui::SetCursorScreenPos(ImVec2(center.x - 16.0f, iconY));
        const char* iconNm = "";

        if (s_indicatorType == 1) iconNm = "Play";
        else if (s_indicatorType == 2) iconNm = "Pause";
        else if (s_indicatorType == 3) iconNm = "RotateCw";
        else if (s_indicatorType == 4) iconNm = "RotateCcw";
        else if (s_indicatorType == 5) iconNm = (s_indicatorText == "Muted" ? "VolumeX" : "Volume2");

        if (iconNm[0] != '\0') {
            Lucide::IconProps indProps;
            indProps.size = 32.0f;
            indProps.color = IM_COL32(255, 255, 255, (int)(255 * aPulse));
            indProps.thickness = 2.0f;
            indProps.hoverThickness = 2.0f;
            indProps.animated = false;
            Lucide::Icon(iconNm, indProps);
        }

        if (hasText) {
            ImGui::SetWindowFontScale(1.0f);
            ImVec2 tSize = ImGui::CalcTextSize(s_indicatorText.c_str());
            drawList->AddText(ImVec2(center.x - tSize.x * 0.5f, center.y + 8.0f), IM_COL32(255, 255, 255, (int)(255 * aPulse)), s_indicatorText.c_str());
        }
    }

    ImGui::End(); // End main overlay to natively allow overlay popup blocks

    // 8. MENU RENDERERS & GLASSMORPHISM SURFACES
    static int s_lastMenuState = 0;

    LerpFade(s_serverMenuAlpha, (s_showServerMenu && !g_IsPIPMode) ? 1.0f : 0.0f, 15.0f);
    LerpFade(s_subMenuAlpha, (s_showSubMenu && hasSubs && !g_IsPIPMode) ? 1.0f : 0.0f, 15.0f);
    LerpFade(s_epPanelAlpha, (s_showEpPanel && !g_IsPIPMode &&
        (bridgeEpisodePanelAvailable ||
         (!g_Settings.bridgeMode && g_ActiveDetails && !g_ActiveDetails->seasons.empty()))) ? 1.0f : 0.0f, 15.0f);
    LerpFade(s_seasonPopupAlpha, s_seasonDropdownOpen ? 1.0f : 0.0f, 15.0f);
    LerpFade(s_contextMenuAlpha, s_showContextMenu ? 1.0f : 0.0f, 15.0f);

    bool anyPanelFading = s_serverMenuAlpha > 0.01f || s_subMenuAlpha > 0.01f || s_epPanelAlpha > 0.01f || s_seasonPopupAlpha > 0.01f || s_contextMenuAlpha > 0.01f;
    int curMenuState = s_showServerMenu ? 1 : (s_showSubMenu ? 2 : (s_showEpPanel ? 3 : (s_showContextMenu ? 4 : 0)));

    // The full-screen glass panel renderer is only needed when a menu exists.
    // Creating it during startup made every bridge launch pay for a surface that
    // most playback sessions never open.
    if ((curMenuState != 0 || anyPanelFading) && !s_PanelRenderer) {
        dynhtml::SurfaceConfig cfg;
        cfg.useHardwareAcceleration = true;
        s_PanelRenderer = new dynhtml::HtmlRenderer(cfg);
        s_PanelRenderer->InitializeDX11(g_pd3dDevice, g_pd3dDeviceContext);
    }

    if (curMenuState != 0 && s_PanelRenderer) {
        if (curMenuState != s_lastMenuState) {
            std::string htmlPayload = "";
            if (curMenuState == 1 || curMenuState == 2 || curMenuState == 4) {
                htmlPayload = "<div style='position:absolute; left:0; top:0; width:100%; height:100%; border-radius:0px; background:linear-gradient(145deg, rgba(20,24,34,0.65), rgba(10,12,18,0.85)); backdrop-filter:blur(30px); border:1px solid rgba(255,255,255,0.06); box-shadow:0 10px 40px rgba(0,0,0,0.5);'></div>";
            }
            else if (curMenuState == 3) {
                htmlPayload = "<div style='position:absolute; left:0; top:0; width:100%; height:100%; background:linear-gradient(180deg, rgba(15,18,26,0.75), rgba(8,10,14,0.9)); backdrop-filter:blur(35px); border-left:1px solid rgba(255,255,255,0.08); box-shadow:-10px 0 50px rgba(0,0,0,0.5);'></div>";
            }
            s_PanelRenderer->SetHTML(htmlPayload.c_str());
            s_lastMenuState = curMenuState;
        }
    }
    else {
        s_lastMenuState = 0;
    }

    if (anyPanelFading && s_PanelRenderer) {
        s_PanelRenderer->Tick(io.DeltaTime);
        s_PanelRenderer->SetViewport((int)screenSize.x, (int)screenSize.y);
        s_PanelRenderer->SetBackdropTexture(bgSrv, (int)screenSize.x, (int)screenSize.y);
        s_PanelRenderer->SetBackdropVideoRect(renderPos.x, renderPos.y, renderSize.x, renderSize.y);
        s_PanelRenderer->SetBackdropAmbientEffect(
            static_cast<float>(ImGui::GetTime()),
            0.55f * std::max({
                s_serverMenuAlpha,
                s_subMenuAlpha,
                s_epPanelAlpha,
                s_contextMenuAlpha }));
        s_PanelRenderer->Render();
    }

    auto DrawSleekRadio = [&](const char* id, const char* label, bool active, float w, float parentAlpha = 1.0f) -> bool {
        ImGuiID itemID = ImGui::GetID(id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, 34));
        bool hov = ImGui::IsItemHovered();

        float& hovVal = s_radioHoverMap[itemID];
        LerpFade(hovVal, hov ? 1.0f : 0.0f, 15.0f);

        float& actVal = s_radioActiveMap[itemID];
        LerpFade(actVal, active ? 1.0f : 0.0f, 15.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 34), IM_COL32(255, 255, 255, (int)(5 * parentAlpha)), 8.0f);

        if (hovVal > 0.01f) {
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 34), IM_COL32(255, 255, 255, (int)(10 * hovVal * parentAlpha)), 8.0f);
        }
        if (actVal > 0.01f) {
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 34), IM_COL32(139, 92, 246, (int)(70 * actVal * parentAlpha)), 8.0f);
        }

        dl->AddRect(pos, ImVec2(pos.x + w, pos.y + 34), IM_COL32(255, 255, 255, (int)(20 * parentAlpha)), 8.0f);

        if (actVal > 0.01f) {
            dl->AddRect(pos, ImVec2(pos.x + w, pos.y + 34), IM_COL32(139, 92, 246, (int)(255 * actVal * parentAlpha)), 8.0f);
        }

        dl->AddCircle(ImVec2(pos.x + 18, pos.y + 17), 6.0f, IM_COL32(255, 255, 255, (int)(20 * parentAlpha)), 0, 2.0f);

        if (actVal > 0.01f) {
            dl->AddCircle(ImVec2(pos.x + 18, pos.y + 17), 6.0f, IM_COL32(139, 92, 246, (int)(255 * actVal * parentAlpha)), 0, 2.0f);
            dl->AddCircleFilled(ImVec2(pos.x + 18, pos.y + 17), 3.0f * actVal, IM_COL32(139, 92, 246, (int)(255 * parentAlpha)));
        }

        // --- NEW TEXT TRUNCATION & MARQUEE LOGIC ---
        static std::map<ImGuiID, float> s_scrollTimeMap;
        float& itemTime = s_scrollTimeMap[itemID];

        // Track per-item hover time instead of using global GetTime() to prevent jumping
        if (hov) {
            itemTime += ImGui::GetIO().DeltaTime;
        }
        else {
            // Smoothly rewind text back to the start when mouse leaves
            itemTime = std::max(0.0f, itemTime - ImGui::GetIO().DeltaTime * 2.0f);
        }

        float clipW = w - 46.0f; // 36px left start + 10px right padding
        ImVec2 tSize = ImGui::CalcTextSize(label);
        float scrollOffset = 0.0f;

        if (tSize.x > clipW) {
            float overflow = tSize.x - clipW;

            // Wait 0.5s before starting the marquee
            float activeTime = std::max(0.0f, itemTime - 0.5f);

            // Phase-shift the sine wave by -PI/2 so it starts perfectly at -1.0
            float wave = std::clamp(std::sin(activeTime * 1.5f - 1.570796f) * 1.5f, -1.0f, 1.0f);
            float normalized = (wave + 1.0f) * 0.5f; // Maps cleanly from 0.0 to 1.0

            scrollOffset = normalized * overflow;
        }

        // Apply Native Hardware Clipping Rect for a clean cut-off
        ImVec2 clipMin(pos.x + 36, pos.y);
        ImVec2 clipMax(pos.x + w - 10, pos.y + 34);
        dl->PushClipRect(clipMin, clipMax, true);

        float textX = pos.x + 36 - scrollOffset;
        float textY = pos.y + 9;

        dl->AddText(ImVec2(textX, textY), IM_COL32(160, 160, 160, (int)(255 * parentAlpha)), label);

        if (hovVal > 0.01f) {
            dl->AddText(ImVec2(textX, textY), IM_COL32(220, 220, 220, (int)(255 * hovVal * parentAlpha)), label);
        }
        if (actVal > 0.01f) {
            dl->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, (int)(255 * actVal * parentAlpha)), label);
        }

        dl->PopClipRect();
        // -------------------------------------------

        return clicked;
        };

    auto DrawSleekToggle = [&](const char* id, const char* label, bool* active, float w, float parentAlpha = 1.0f) {
        ImGuiID itemID = ImGui::GetID(id);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, 36));

        if (clicked) *active = !*active;

        float& actVal = s_radioActiveMap[itemID];
        LerpFade(actVal, *active ? 1.0f : 0.0f, 15.0f);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(pos.x + 10, pos.y + 10), IM_COL32(210, 210, 210, (int)(255 * parentAlpha)), label);

        float switchW = 40.0f, switchH = 22.0f;
        ImVec2 switchPos(pos.x + w - switchW - 10, pos.y + 7);
        dl->AddRectFilled(switchPos, ImVec2(switchPos.x + switchW, switchPos.y + switchH), IM_COL32(255, 255, 255, (int)(30 * parentAlpha)), switchH / 2);

        if (actVal > 0.01f) {
            const ImVec2 switchCenter(
                switchPos.x + switchW * 0.5f,
                switchPos.y + switchH * 0.5f);
            const float coreRadius = switchH * 0.5f;
            const float capsuleHalfLength =
                std::max(switchW * 0.5f - coreRadius, 0.0f);
            const float feather = 10.0f;
            const float carrierRadius = coreRadius + feather + 3.0f;
            const ImVec2 axisStart(
                switchCenter.x - capsuleHalfLength,
                switchCenter.y);
            const ImVec2 axisEnd(
                switchCenter.x + capsuleHalfLength,
                switchCenter.y);

            PushMistGlowShape(
                dl,
                IM_COL32(139, 92, 246, 255),
                1.05f * actVal * parentAlpha,
                mistTime,
                switchCenter,
                ImVec2(capsuleHalfLength, coreRadius),
                coreRadius,
                feather,
                2.0f);

            const ImU32 smokeCarrier = IM_COL32(
                255, 255, 255,
                (int)(120 * actVal * parentAlpha));

            dl->PathClear();
            dl->PathLineTo(axisStart);
            dl->PathLineTo(axisEnd);
            dl->PathStroke(
                smokeCarrier,
                0,
                carrierRadius * 2.0f);
            dl->AddCircleFilled(
                axisStart,
                carrierRadius,
                smokeCarrier,
                24);
            dl->AddCircleFilled(
                axisEnd,
                carrierRadius,
                smokeCarrier,
                24);
            PopMistGlow(dl);

            dl->AddRectFilled(switchPos, ImVec2(switchPos.x + switchW, switchPos.y + switchH), IM_COL32(139, 92, 246, (int)(255 * actVal * parentAlpha)), switchH / 2);
        }

        float thumbOffX = switchPos.x + switchH / 2;
        float thumbOnX = switchPos.x + switchW - switchH / 2;

        dl->AddCircleFilled(
            ImVec2(thumbOffX + (thumbOnX - thumbOffX) * actVal, switchPos.y + switchH / 2),
            switchH / 2 - 3.0f,
            IM_COL32(255, 255, 255, (int)(255 * parentAlpha))
        );
        };

    auto DrawSleekGlassBackground = [&](float parentAlpha = 1.0f) {
        ImVec2 wPos = ImGui::GetWindowPos();
        ImVec2 wSize = ImGui::GetWindowSize();
        ImVec2 uv0 = ImVec2(wPos.x / screenSize.x, wPos.y / screenSize.y);
        ImVec2 uv1 = ImVec2((wPos.x + wSize.x) / screenSize.x, (wPos.y + wSize.y) / screenSize.y);

        void* pt = s_PanelRenderer ? s_PanelRenderer->TextureHandle() : nullptr;
        if (pt) {
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)pt, wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y), uv0, uv1, IM_COL32(255, 255, 255, (int)(255 * parentAlpha)));
        }
        else {
            ImGui::GetWindowDrawList()->AddRectFilled(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y), IM_COL32(20, 24, 34, (int)(250 * parentAlpha)), 0.0f);
        }
        };

    if (s_contextMenuAlpha > 0.01f) {
        ImGui::SetNextWindowPos(s_contextMenuPos);
        ImGui::SetNextWindowSize(ImVec2(320.0f, 300.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 40));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_contextMenuAlpha);

        // FIX: Adding NoMouseInputs strictly when fading out guarantees the window becomes fully physically transparent
        // to inputs, curing the deadzone bug!
        ImGuiWindowFlags ctxFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
        if (!s_showContextMenu) ctxFlags |= ImGuiWindowFlags_NoMouseInputs;

        ImGui::Begin("PlayerContextMenuAnim", nullptr, ctxFlags);
        DrawSleekGlassBackground(s_contextMenuAlpha);

        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 8.0f);
        ImGui::TextColored(ImVec4(0.54f, 0.36f, 0.96f, s_contextMenuAlpha), "Player Engine settings");
        ImGui::Dummy(ImVec2(0, 5));

        float ctxW = 280.0f;
        if (DrawSleekRadio("##CtxDS", "Custom DirectShow Engine", g_Settings.engineMode == 0, ctxW, s_contextMenuAlpha)) {
            g_Settings.engineMode = 0; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (DrawSleekRadio("##CtxDSUp", "DirectShow Upscaled (SSAA)", g_Settings.engineMode == 2, ctxW, s_contextMenuAlpha)) {
            g_Settings.engineMode = 2; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (DrawSleekRadio("##CtxVLC", "VLC Native Engine", g_Settings.engineMode == 1, ctxW, s_contextMenuAlpha)) {
            g_Settings.engineMode = 1; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextColored(ImVec4(0.54f, 0.36f, 0.96f, s_contextMenuAlpha), "Encoding Hardware");
        ImGui::Dummy(ImVec2(0, 5));

        if (DrawSleekRadio("##CtxSoft", "Software Decoding", g_Settings.hwDecoding == 0, ctxW, s_contextMenuAlpha)) {
            g_Settings.hwDecoding = 0; g_Settings.requestPlay = true;
        }
        ImGui::Dummy(ImVec2(0, 2));
        if (DrawSleekRadio("##CtxHard", "Hardware Decoding", g_Settings.hwDecoding == 1, ctxW, s_contextMenuAlpha)) {
            g_Settings.hwDecoding = 1; g_Settings.requestPlay = true;
        }
        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
    }

    if (s_serverMenuAlpha > 0.01f) {
        float setWinW = 340.0f;
        float setWinH = g_Settings.bridgeMode
            ? std::min(680.0f, std::max(430.0f, screenSize.y - headerHeight - 18.0f))
            : 430.0f;
        ImGui::SetNextWindowPos(ImVec2(screenSize.x - headerPaddingRight - setWinW, headerHeight + 5.0f));
        ImGui::SetNextWindowSize(ImVec2(setWinW, setWinH));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_serverMenuAlpha);

        ImGuiWindowFlags srvFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
        if (!g_Settings.bridgeMode) srvFlags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (!s_showServerMenu) srvFlags |= ImGuiWindowFlags_NoMouseInputs;

        if (g_Settings.bridgeMode) PushScrollbarStyles();
        ImGui::Begin("ServerMenu", nullptr, srvFlags);
        DrawSleekGlassBackground(s_serverMenuAlpha);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* renderQualityTitle = g_Settings.bridgeMode ? "RENDER SCALE" : "VIDEO QUALITY";
        ImVec2 ts = ImGui::CalcTextSize(renderQualityTitle);
        ImGui::SetCursorPosX((setWinW - ts.x) * 0.5f);
        dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_serverMenuAlpha)), renderQualityTitle);
        ImGui::Dummy(ImVec2(0, 16));

        int currentSrcHeight = (g_Settings.engineMode == 1) ? GetVLCVideoHeight() : 1080;
        if (currentSrcHeight <= 0) currentSrcHeight = 1080;

        std::vector<std::string> qOpts;
        if (currentSrcHeight >= 2160) qOpts.push_back("4k");
        if (currentSrcHeight >= 1080) qOpts.push_back("1080p");
        if (currentSrcHeight >= 720) qOpts.push_back("720p");
        qOpts.push_back("360p");

        auto ParseRes = [](const std::string& r) {
            if (r == "4k") return 2160;
            if (r == "1080p") return 1080;
            if (r == "720p") return 720;
            if (r == "360p") return 360;
            return 9999;
            };
        std::string activeQ = qOpts.front();
        int prefRes = ParseRes(g_ActiveProfile.defaultQuality);
        if (prefRes != 9999) {
            for (const auto& q : qOpts) {
                if (ParseRes(q) <= prefRes) { activeQ = q; break; }
            }
        }

        float qW = 300.0f;
        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        ImVec2 qPos = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(qPos, ImVec2(qPos.x + qW, qPos.y + 36), IM_COL32(0, 0, 0, (int)(80 * s_serverMenuAlpha)), 8.0f);

        float segW = qW / qOpts.size();
        float targetX = -1.0f;
        for (int i = 0; i < qOpts.size(); i++) {
            if (qOpts[i] == activeQ) { targetX = qPos.x + i * segW; break; }
        }

        static float s_qAnimX = -1.0f;
        if (s_qAnimX < 0.0f || std::abs(s_qAnimX - qPos.x) > qW) {
            s_qAnimX = targetX;
        }

        if (targetX >= 0.0f) LerpFade(s_qAnimX, targetX, 15.0f);

        if (s_qAnimX >= 0.0f) {
            dl->AddRectFilled(ImVec2(s_qAnimX + 2, qPos.y + 2), ImVec2(s_qAnimX + segW - 2, qPos.y + 36 - 2), IM_COL32(139, 92, 246, (int)(255 * s_serverMenuAlpha)), 8.0f);
        }

        for (int i = 0; i < qOpts.size(); i++) {
            ImVec2 sMin(qPos.x + i * segW, qPos.y);
            ImGuiID qId = ImGui::GetID(qOpts[i].c_str());
            float& qAct = s_radioActiveMap[qId];
            LerpFade(qAct, (qOpts[i] == activeQ) ? 1.0f : 0.0f, 15.0f);

            ImVec2 tSize = ImGui::CalcTextSize(qOpts[i].c_str());
            dl->AddText(ImVec2(sMin.x + (segW - tSize.x) / 2, sMin.y + (36 - tSize.y) / 2), IM_COL32(180, 180, 180, (int)(255 * s_serverMenuAlpha)), qOpts[i].c_str());

            if (qAct > 0.01f) {
                dl->AddText(ImVec2(sMin.x + (segW - tSize.x) / 2, sMin.y + (36 - tSize.y) / 2), IM_COL32(255, 255, 255, (int)(255 * qAct * s_serverMenuAlpha)), qOpts[i].c_str());
            }

            ImGui::SetCursorScreenPos(sMin);
            if (ImGui::InvisibleButton(qOpts[i].c_str(), ImVec2(segW, 36))) {
                if (g_ActiveProfile.defaultQuality != qOpts[i]) {
                    g_ActiveProfile.defaultQuality = qOpts[i];
                    WritePrivateProfileStringA("Profile", "Quality", g_ActiveProfile.defaultQuality.c_str(), ".\\tvanime_settings.ini");
                    s_pendingStartProgress = g_Settings.progress;
                    g_Settings.requestPlay = true;
                }
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(qPos.x, qPos.y + 50));
        ts = ImGui::CalcTextSize("ENGINE BACKEND");
        ImGui::SetCursorPosX((setWinW - ts.x) * 0.5f);
        dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_serverMenuAlpha)), "ENGINE BACKEND");
        ImGui::Dummy(ImVec2(0, 16));

        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        if (DrawSleekRadio("##EngDS", "Custom DirectShow", g_Settings.engineMode == 0, qW, s_serverMenuAlpha)) {
            g_Settings.engineMode = 0; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        if (DrawSleekRadio("##EngDSUp", "DirectShow Upscaled", g_Settings.engineMode == 2, qW, s_serverMenuAlpha)) {
            g_Settings.engineMode = 2; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        if (DrawSleekRadio("##EngVLC", "VLC Texture Map", g_Settings.engineMode == 1, qW, s_serverMenuAlpha)) {
            g_Settings.engineMode = 1; s_pendingStartProgress = g_Settings.progress; g_Settings.requestPlay = true;
        }

        ImGui::Dummy(ImVec2(0, 10));
        ts = ImGui::CalcTextSize("HARDWARE ACCELERATION");
        ImGui::SetCursorPosX((setWinW - ts.x) * 0.5f);
        dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_serverMenuAlpha)), "HARDWARE ACCELERATION");
        ImGui::Dummy(ImVec2(0, 16));

        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        if (DrawSleekRadio("##HwSoft", "Software Decoding", g_Settings.hwDecoding == 0, qW, s_serverMenuAlpha)) {
            g_Settings.hwDecoding = 0; g_Settings.requestPlay = true;
        }

        ImGui::Dummy(ImVec2(0, 2));
        ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
        if (DrawSleekRadio("##HwHard", "Hardware Decoding", g_Settings.hwDecoding == 1, qW, s_serverMenuAlpha)) {
            g_Settings.hwDecoding = 1; g_Settings.requestPlay = true;
        }

        if (g_Settings.bridgeMode && s_bridgeMetadataValid) {
            ImGui::Dummy(ImVec2(0, 14));
            ts = ImGui::CalcTextSize("MOVIEBOX QUALITY");
            ImGui::SetCursorPosX((setWinW - ts.x) * 0.5f);
            dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_serverMenuAlpha)), "MOVIEBOX QUALITY");
            ImGui::Dummy(ImVec2(0, 12));

            if (s_bridgeMetadata.qualities.empty()) {
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, s_serverMenuAlpha), "No quality choices available.");
            }
            for (size_t i = 0; i < s_bridgeMetadata.qualities.size(); ++i) {
                const mbp::PlaybackChoice& choice = s_bridgeMetadata.qualities[i];
                ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
                const std::string id = "##BridgeQuality" + std::to_string(i);
                if (DrawSleekRadio(id.c_str(), choice.label.c_str(), choice.selected, qW, s_serverMenuAlpha)) {
                    CaptureBridgePreference("quality", choice);
                    MovieBoxBridgeQueueAction("quality", choice.id);
                }
                ImGui::Dummy(ImVec2(0, 2));
            }

            ImGui::Dummy(ImVec2(0, 10));
            ts = ImGui::CalcTextSize("MOVIEBOX SERVER");
            ImGui::SetCursorPosX((setWinW - ts.x) * 0.5f);
            dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_serverMenuAlpha)), "MOVIEBOX SERVER");
            ImGui::Dummy(ImVec2(0, 12));

            if (s_bridgeMetadata.servers.empty()) {
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, s_serverMenuAlpha), "No server choices available.");
            }
            for (size_t i = 0; i < s_bridgeMetadata.servers.size(); ++i) {
                const mbp::PlaybackChoice& choice = s_bridgeMetadata.servers[i];
                ImGui::SetCursorPosX((setWinW - qW) * 0.5f);
                const std::string id = "##BridgeServer" + std::to_string(i);
                if (DrawSleekRadio(id.c_str(), choice.label.c_str(), choice.selected, qW, s_serverMenuAlpha)) {
                    CaptureBridgePreference("server", choice);
                    MovieBoxBridgeQueueAction("server", choice.id);
                }
                ImGui::Dummy(ImVec2(0, 2));
            }

            if (s_bridgeMetadata.loading) {
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::SetCursorPosX(20.0f);
                ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, s_serverMenuAlpha), "MovieBox metadata is loading...");
            }
            if (!s_bridgeMetadata.error.empty()) {
                ImGui::Dummy(ImVec2(0, 6));
                ImGui::SetCursorPosX(20.0f);
                ImGui::PushTextWrapPos(setWinW - 20.0f);
                ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, s_serverMenuAlpha), "%s", s_bridgeMetadata.error.c_str());
                ImGui::PopTextWrapPos();
            }
        }

        ImGui::End();
        if (g_Settings.bridgeMode) PopScrollbarStyles();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    if (s_subMenuAlpha > 0.01f) {
        float subWinW = 280.0f;
        ImGui::SetNextWindowPos(ImVec2(screenSize.x - headerPaddingRight - subWinW, headerHeight + 5.0f));
        ImGui::SetNextWindowSize(ImVec2(subWinW, g_Settings.bridgeMode ? 390.0f : 320.0f));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_subMenuAlpha);

        ImGuiWindowFlags subFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav;
        if (!s_showSubMenu) subFlags |= ImGuiWindowFlags_NoMouseInputs;

        ImGui::Begin("SubMenu", nullptr, subFlags);
        DrawSleekGlassBackground(s_subMenuAlpha);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 st = ImGui::CalcTextSize("SUBTITLES");
        ImGui::SetCursorPosX((subWinW - st.x) * 0.5f);
        dl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(140, 140, 140, (int)(255 * s_subMenuAlpha)), "SUBTITLES");
        ImGui::Dummy(ImVec2(0, 16));

        ImGui::SetCursorPosX((subWinW - 240.0f) * 0.5f);
        PushScrollbarStyles(); // APPLY SLEEK SCROLLBARS
        ImGui::BeginChild("SubScrollList", ImVec2(240, g_Settings.bridgeMode ? 310.0f : 240.0f), false, ImGuiWindowFlags_NoBackground);

        if (g_Settings.bridgeMode) {
            bool anySelected = false;
            for (const mbp::PlaybackChoice& choice : s_bridgeMetadata.subtitles) {
                anySelected = anySelected || choice.selected;
            }

            ImGui::SetCursorPosX((240.0f - 230.0f) * 0.5f);
            if (DrawSleekRadio("##BridgeSubOff", "Off", !anySelected, 225.0f, s_subMenuAlpha)) {
                CaptureSubtitleOffPreference();
                MovieBoxBridgeQueueAction("subtitle", "off");
                s_bridgeCues.clear();
                if (g_Settings.engineMode == 1) SetVLCSpuTrack(-1);
            }
            ImGui::Dummy(ImVec2(0, 6));

            std::string currentGroup;
            for (size_t i = 0; i < s_bridgeMetadata.subtitles.size(); ++i) {
                const mbp::PlaybackChoice& choice = s_bridgeMetadata.subtitles[i];
                if (choice.group != currentGroup) {
                    currentGroup = choice.group;
                    if (!currentGroup.empty()) {
                        ImGui::TextColored(ImVec4(0.55f, 0.36f, 0.96f, s_subMenuAlpha), "%s", currentGroup.c_str());
                        ImGui::Dummy(ImVec2(0, 3));
                    }
                }
                ImGui::SetCursorPosX((240.0f - 230.0f) * 0.5f);
                const std::string id = "##BridgeSub" + std::to_string(i);
                if (DrawSleekRadio(id.c_str(), choice.label.c_str(), choice.selected, 225.0f, s_subMenuAlpha)) {
                    CaptureBridgePreference("subtitle", choice);
                    MovieBoxBridgeQueueAction("subtitle", choice.id);
                    if (g_Settings.engineMode == 1) SetVLCSpuTrack(-1);
                }
                ImGui::Dummy(ImVec2(0, 2));
            }

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::TextColored(ImVec4(0.55f, 0.36f, 0.96f, s_subMenuAlpha), "TIMING");
            ImGui::Dummy(ImVec2(0, 3));
            const std::string delayLabel = "Delay " + (s_bridgeSubtitleDelay >= 0.0 ? std::string("+") : std::string()) +
                std::to_string(s_bridgeSubtitleDelay).substr(0, std::to_string(s_bridgeSubtitleDelay).find('.') + 2) + "s";
            ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.80f, s_subMenuAlpha), "%s", delayLabel.c_str());
            if (ImGui::Button("-0.5s##BridgeSubDelay", ImVec2(70, 30))) {
                s_bridgeSubtitleDelay -= 0.5;
                MovieBoxBridgeQueueAction("subtitleDelay", "", s_bridgeSubtitleDelay);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset##BridgeSubDelay", ImVec2(75, 30))) {
                s_bridgeSubtitleDelay = 0.0;
                MovieBoxBridgeQueueAction("subtitleDelay", "", s_bridgeSubtitleDelay);
            }
            ImGui::SameLine();
            if (ImGui::Button("+0.5s##BridgeSubDelay", ImVec2(70, 30))) {
                s_bridgeSubtitleDelay += 0.5;
                MovieBoxBridgeQueueAction("subtitleDelay", "", s_bridgeSubtitleDelay);
            }
        }
        else {
            for (int i = 0; i < engineSubs.size(); i++) {
                ImGui::SetCursorPosX((240.0f - 230.0f) * 0.5f);

                bool isSubActive = (g_Settings.activeSub == engineSubs[i].name || (g_Settings.activeSub.empty() && engineSubs[i].id == -1));

                if (DrawSleekRadio((std::string("##Sub") + std::to_string(i)).c_str(), engineSubs[i].name.c_str(), isSubActive, 225.0f, s_subMenuAlpha)) {
                    g_Settings.activeSub = engineSubs[i].name;
                    if (g_Settings.engineMode == 1) SetVLCSpuTrack(engineSubs[i].id);
                }
                ImGui::Dummy(ImVec2(0, 2));
            }
        }
        ImGui::EndChild();
        PopScrollbarStyles();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

    auto RenderBridgeEpisodePanel = [&]() {
        const float epW = 380.0f;
        const float comboW = epW - 48.0f;

        // MovieBox supplies one flat episode list with a Group string (normally
        // "Season N"). Build a stable season list from that metadata so bridge
        // mode keeps the same season -> episode interaction as standalone mode.
        std::vector<std::string> bridgeSeasonGroups;
        std::string selectedEpisodeGroup;
        for (const mbp::PlaybackChoice& choice : s_bridgeMetadata.episodes) {
            if (choice.selected && !choice.group.empty()) selectedEpisodeGroup = choice.group;
            if (choice.group.empty()) continue;
            if (std::find(bridgeSeasonGroups.begin(), bridgeSeasonGroups.end(), choice.group) == bridgeSeasonGroups.end()) {
                bridgeSeasonGroups.push_back(choice.group);
            }
        }

        if (!bridgeSeasonGroups.empty()) {
            const bool savedGroupStillExists =
                std::find(bridgeSeasonGroups.begin(), bridgeSeasonGroups.end(), s_bridgeSeasonGroup) != bridgeSeasonGroups.end();
            if (!savedGroupStillExists) {
                s_bridgeSeasonGroup = !selectedEpisodeGroup.empty() ? selectedEpisodeGroup : bridgeSeasonGroups.front();
            }
        }
        else {
            s_bridgeSeasonGroup.clear();
            s_seasonDropdownOpen = false;
        }

        if (!s_showEpPanel) s_seasonDropdownOpen = false;

        ImGui::SetNextWindowPos(ImVec2(screenSize.x - epW, 0));
        ImGui::SetNextWindowSize(ImVec2(epW, screenSize.y));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_epPanelAlpha);

        ImGuiWindowFlags epFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        if (!s_showEpPanel) epFlags |= ImGuiWindowFlags_NoMouseInputs;

        ImGui::Begin("EpPanelBridge", nullptr, epFlags);
        DrawSleekGlassBackground(s_epPanelAlpha);
        ImDrawList* wDl = ImGui::GetWindowDrawList();
        const ImVec2 winPos = ImGui::GetWindowPos();

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, winPos.y + 24));
        ImGui::SetWindowFontScale(1.2f);
        wDl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha)), "Episodes");
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + epW - 44, winPos.y + 20));
        if (ImGui::InvisibleButton("##CloseBridgeEp", ImVec2(30, 30))) {
            s_showEpPanel = false;
            s_seasonDropdownOpen = false;
        }
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + epW - 39, winPos.y + 25));
        Lucide::IconProps closeProps;
        closeProps.size = 20.0f;
        closeProps.color = ImGui::IsItemHovered() ? IM_COL32(255, 100, 100, (int)(255 * s_epPanelAlpha)) : IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha));
        closeProps.thickness = 2.0f;
        closeProps.hoverThickness = 2.5f;
        closeProps.animated = true;
        Lucide::Icon("X", closeProps);
        wDl->AddLine(ImVec2(winPos.x, winPos.y + 65), ImVec2(winPos.x + epW, winPos.y + 65), IM_COL32(255, 255, 255, (int)(20 * s_epPanelAlpha)));

        float curY = winPos.y + 82.0f;

        if (!bridgeSeasonGroups.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, curY));
            const bool comboClicked = ImGui::InvisibleButton("##BridgeSeasonCombo", ImVec2(comboW, 44));
            const bool comboHovered = ImGui::IsItemHovered();

            if (comboClicked) {
                s_seasonDropdownOpen = !s_seasonDropdownOpen;
            }
            else if (s_seasonDropdownOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const float popupHeight = std::min(250.0f, (float)(bridgeSeasonGroups.size() * 36.0f) + 32.0f);
                const bool inCombo = mPos.x >= winPos.x + 24 && mPos.x <= winPos.x + 24 + comboW &&
                    mPos.y >= curY && mPos.y <= curY + 44.0f;
                const bool inPopup = mPos.x >= winPos.x + 24 && mPos.x <= winPos.x + 24 + comboW &&
                    mPos.y >= winPos.y + 132.0f && mPos.y <= winPos.y + 132.0f + popupHeight;
                if (!inCombo && !inPopup) s_seasonDropdownOpen = false;
            }

            ImGuiID comboId = ImGui::GetID("##BridgeSeasonCombo");
            float& comboActive = s_radioActiveMap[comboId];
            float& comboHover = s_radioHoverMap[comboId];
            LerpFade(comboActive, s_seasonDropdownOpen ? 1.0f : 0.0f, 15.0f);
            LerpFade(comboHover, comboHovered ? 1.0f : 0.0f, 15.0f);

            wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(10 * s_epPanelAlpha)));
            if (comboHover > 0.01f) {
                wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(15 * comboHover * s_epPanelAlpha)));
            }
            if (comboActive > 0.01f) {
                wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(139, 92, 246, (int)(28 * comboActive * s_epPanelAlpha)));
            }
            wDl->AddRect(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(30 * s_epPanelAlpha)));
            wDl->AddText(ImVec2(winPos.x + 40, curY + 14), IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha)), s_bridgeSeasonGroup.c_str());

            ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24 + comboW - 32, curY + 12));
            Lucide::IconProps chevronProps;
            chevronProps.size = 20.0f;
            chevronProps.color = IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha));
            chevronProps.thickness = 2.0f;
            chevronProps.hoverThickness = 2.0f;
            chevronProps.animated = false;
            Lucide::Icon("ChevronDown", chevronProps);

            curY += 60.0f;
        }

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, curY));
        const float itemW = epW - 48.0f;
        if (DrawSleekRadio("##BridgeNextEpisode", "Play next episode", false, itemW, s_epPanelAlpha)) {
            MovieBoxBridgeQueueAction("next");
            s_showEpPanel = false;
            s_seasonDropdownOpen = false;
        }
        curY += 50.0f;

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 16, curY));
        PushScrollbarStyles();
        ImGui::BeginChild("##BridgeEpisodeList", ImVec2(epW - 32, std::max(80.0f, screenSize.y - curY - 10.0f)), false, ImGuiWindowFlags_NoBackground);

        if (!s_bridgeMetadata.error.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + epW - 64.0f);
            ImGui::TextColored(ImVec4(1.0f, 0.48f, 0.48f, s_epPanelAlpha), "%s", s_bridgeMetadata.error.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8));
        }
        if (s_bridgeMetadata.loading) {
            ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, s_epPanelAlpha), "Loading episode metadata...");
            ImGui::Dummy(ImVec2(0, 8));
        }
        if (s_bridgeMetadata.episodes.empty() && !s_bridgeMetadata.loading) {
            ImGui::TextColored(ImVec4(0.62f, 0.62f, 0.68f, s_epPanelAlpha), "No episode metadata available.");
        }

        for (size_t i = 0; i < s_bridgeMetadata.episodes.size(); ++i) {
            const mbp::PlaybackChoice& choice = s_bridgeMetadata.episodes[i];
            if (!s_bridgeSeasonGroup.empty() && choice.group != s_bridgeSeasonGroup) continue;

            const std::string id = "##BridgeEpisode" + std::to_string(i);
            if (DrawSleekRadio(id.c_str(), choice.label.c_str(), choice.selected, epW - 48.0f, s_epPanelAlpha)) {
                MovieBoxBridgeQueueAction("episode", choice.id);
                s_showEpPanel = false;
                s_seasonDropdownOpen = false;
            }
            ImGui::Dummy(ImVec2(0, 2));
        }

        ImGui::EndChild();
        PopScrollbarStyles();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        // Render the season list as its own foreground window so it stays above
        // the full-height episode panel and remains clickable during animation.
        if (s_seasonPopupAlpha > 0.01f && !bridgeSeasonGroups.empty()) {
            const float listHeight = (float)bridgeSeasonGroups.size() * 36.0f;
            const float popupHeight = std::min(250.0f, listHeight + 32.0f);

            ImGui::SetNextWindowPos(ImVec2(winPos.x + 24, winPos.y + 132.0f));
            ImGui::SetNextWindowSize(ImVec2(comboW, popupHeight));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 40));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_seasonPopupAlpha);

            ImGuiWindowFlags popupFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            if (!s_seasonDropdownOpen) popupFlags |= ImGuiWindowFlags_NoMouseInputs;

            ImGui::Begin("BridgeSeasonListPopup", nullptr, popupFlags);
            DrawSleekGlassBackground(s_seasonPopupAlpha);
            const float startY = std::max(0.0f, (popupHeight - listHeight) * 0.3f);
            if (startY > 0.0f) ImGui::Dummy(ImVec2(0, startY));

            PushScrollbarStyles();
            ImGui::BeginChild("##BridgeSeasonScroll", ImVec2(comboW, popupHeight - startY), false, ImGuiWindowFlags_NoBackground);
            for (size_t i = 0; i < bridgeSeasonGroups.size(); ++i) {
                const float seasonItemW = comboW - 32.0f;
                ImGui::SetCursorPosX((comboW - seasonItemW) * 0.5f);
                const std::string id = "##BridgeSeason" + std::to_string(i);
                if (DrawSleekRadio(id.c_str(), bridgeSeasonGroups[i].c_str(), s_bridgeSeasonGroup == bridgeSeasonGroups[i], seasonItemW, s_seasonPopupAlpha)) {
                    s_bridgeSeasonGroup = bridgeSeasonGroups[i];
                    s_seasonDropdownOpen = false;
                }
                ImGui::Dummy(ImVec2(0, 2));
            }
            ImGui::EndChild();
            PopScrollbarStyles();
            ImGui::End();
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(2);
        }
    };

    static bool s_autoPlayNext = true;

    if (s_epPanelAlpha > 0.01f && bridgeEpisodePanelAvailable && !g_IsPIPMode) {
        RenderBridgeEpisodePanel();
    }
    else if (s_epPanelAlpha > 0.01f && !g_Settings.bridgeMode && g_ActiveDetails && !g_ActiveDetails->seasons.empty() && !g_IsPIPMode) {
        if (!s_showEpPanel) s_seasonDropdownOpen = false;

        float epW = 380.0f;
        ImGui::SetNextWindowPos(ImVec2(screenSize.x - epW, 0));
        ImGui::SetNextWindowSize(ImVec2(epW, screenSize.y));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_epPanelAlpha);

        // FIX 1: Add ImGuiWindowFlags_NoNav to prevent controller focus stealing, 
        // but ensure the panel can actually receive mouse events properly.
        ImGuiWindowFlags epFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        if (!s_showEpPanel) epFlags |= ImGuiWindowFlags_NoMouseInputs;

        ImGui::Begin("EpPanel", nullptr, epFlags);
        DrawSleekGlassBackground(s_epPanelAlpha);

        ImDrawList* wDl = ImGui::GetWindowDrawList();
        ImVec2 winPos = ImGui::GetWindowPos();

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, winPos.y + 24));
        ImGui::SetWindowFontScale(1.2f);
        wDl->AddText(ImGui::GetCursorScreenPos(), IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha)), "Episodes");
        ImGui::SetWindowFontScale(1.0f);

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + epW - 44, winPos.y + 20));
        if (ImGui::InvisibleButton("##CloseEp", ImVec2(30, 30))) {
            s_showEpPanel = false;
            s_seasonDropdownOpen = false;
        }

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + epW - 39, winPos.y + 25));
        Lucide::IconProps exProps;
        exProps.size = 20.0f;
        exProps.color = ImGui::IsItemHovered() ? IM_COL32(255, 100, 100, (int)(255 * s_epPanelAlpha)) : IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha));
        exProps.thickness = 2.0f;
        exProps.hoverThickness = 2.5f;
        exProps.animated = true;
        Lucide::Icon("X", exProps);

        wDl->AddLine(ImVec2(winPos.x, winPos.y + 65), ImVec2(winPos.x + epW, winPos.y + 65), IM_COL32(255, 255, 255, (int)(20 * s_epPanelAlpha)));

        float curY = winPos.y + 85.0f;
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, curY));
        float comboW = epW - 48;
        bool comboClicked = ImGui::InvisibleButton("SeasonCombo", ImVec2(comboW, 44));
        bool comboHov = ImGui::IsItemHovered();

        if (comboClicked) {
            s_seasonDropdownOpen = !s_seasonDropdownOpen;
        }
        else if (s_seasonDropdownOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            float pWinH = std::min(250.0f, (float)(g_ActiveDetails->seasons.size() * 36.0f) + 32.0f);

            bool inCombo = (mPos.x >= winPos.x + 24 && mPos.x <= winPos.x + 24 + comboW && mPos.y >= curY && mPos.y <= curY + 44.0f);
            bool inPopup = (mPos.x >= winPos.x + 24 && mPos.x <= winPos.x + 24 + comboW && mPos.y >= winPos.y + 135.0f && mPos.y <= winPos.y + 135.0f + pWinH);

            if (!inCombo && !inPopup) {
                s_seasonDropdownOpen = false;
            }
        }

        ImGuiID comboId = ImGui::GetID("SeasonCombo");
        float& cAct = s_radioActiveMap[comboId];
        LerpFade(cAct, s_seasonDropdownOpen ? 1.0f : 0.0f, 15.0f);

        float& cHov = s_radioHoverMap[comboId];
        LerpFade(cHov, comboHov ? 1.0f : 0.0f, 15.0f);

        wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(10 * s_epPanelAlpha)), 0.0f);

        if (cHov > 0.01f) {
            wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(15 * cHov * s_epPanelAlpha)), 0.0f);
        }

        if (cAct > 0.01f) {
            wDl->AddRectFilled(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(25 * cAct * s_epPanelAlpha)), 0.0f);
        }

        wDl->AddRect(ImVec2(winPos.x + 24, curY), ImVec2(winPos.x + 24 + comboW, curY + 44), IM_COL32(255, 255, 255, (int)(30 * s_epPanelAlpha)), 0.0f);
        wDl->AddText(ImVec2(winPos.x + 40, curY + 14), IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha)), g_ActiveDetails->seasons[g_SelectedSeasonIdx].name.c_str());

        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24 + comboW - 32, curY + 12));
        Lucide::IconProps chvProps;
        chvProps.size = 20.0f;
        chvProps.color = IM_COL32(255, 255, 255, (int)(255 * s_epPanelAlpha));
        chvProps.thickness = 2.0f;
        chvProps.hoverThickness = 2.0f;
        chvProps.animated = false;
        Lucide::Icon("ChevronDown", chvProps);

        curY += 60.0f;
        ImGui::SetCursorScreenPos(ImVec2(winPos.x + 24, curY));
        DrawSleekToggle("##AutoPlayTg", "Auto-Play Next", &s_autoPlayNext, comboW, s_epPanelAlpha);

        curY += 45.0f;
        wDl->AddLine(ImVec2(winPos.x, curY), ImVec2(winPos.x + epW, curY), IM_COL32(255, 255, 255, (int)(10 * s_epPanelAlpha)));
        curY += 15.0f;

        if (g_SelectedSeasonIdx < g_ActiveDetails->seasons.size()) {
            ImGui::SetCursorScreenPos(ImVec2(winPos.x + 16, curY));
            PushScrollbarStyles(); // APPLY SLEEK SCROLLBARS
            ImGui::BeginChild("##EpList", ImVec2(epW - 32, screenSize.y - curY - 10), false, ImGuiWindowFlags_NoBackground);

            for (size_t i = 0; i < g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps.size(); ++i) {
                MockEpisode& ep = g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps[i];
                bool isPlayingEp = (g_Settings.currentUrl == ep.url);
                ImVec2 btnPos = ImGui::GetCursorScreenPos();

                if (ImGui::InvisibleButton((std::string("EpItem") + std::to_string(i)).c_str(), ImVec2(epW - 32, 50))) {
                    g_SelectedEpisodeIdx = (int)i;
                    g_Settings.currentUrl = ep.url;
                    g_Settings.activeSeason = g_ActiveDetails->seasons[g_SelectedSeasonIdx].name;
                    g_Settings.videoTitle = g_ActiveDetails->title + " - " + ep.title;
                    g_Settings.progress = 0.0f;
                    s_pendingStartProgress = -1.0f;
                    g_Settings.requestPlay = true;
                    s_showEpPanel = false;
                }

                ImGuiID epId = ImGui::GetID((std::string("EpItem") + std::to_string(i)).c_str());
                float& epHovF = s_radioHoverMap[epId];
                LerpFade(epHovF, ImGui::IsItemHovered() ? 1.0f : 0.0f, 15.0f);

                float& epActF = s_radioActiveMap[epId];
                LerpFade(epActF, isPlayingEp ? 1.0f : 0.0f, 15.0f);

                ImDrawList* cDl = ImGui::GetWindowDrawList();

                if (epHovF > 0.01f) {
                    cDl->AddRectFilled(btnPos, ImVec2(btnPos.x + epW - 32, btnPos.y + 50), IM_COL32(255, 255, 255, (int)(15 * epHovF * s_epPanelAlpha)), 0.0f);
                }
                if (epActF > 0.01f) {
                    cDl->AddRectFilled(btnPos, ImVec2(btnPos.x + epW - 32, btnPos.y + 50), IM_COL32(139, 92, 246, (int)(50 * epActF * s_epPanelAlpha)), 0.0f);
                    cDl->AddRectFilled(ImVec2(btnPos.x + 2, btnPos.y + 12), ImVec2(btnPos.x + 6, btnPos.y + 38), IM_COL32(139, 92, 246, (int)(255 * epActF * s_epPanelAlpha)), 0.0f);
                }

                cDl->AddText(ImVec2(btnPos.x + 18, btnPos.y + 16), IM_COL32(204, 204, 204, (int)(255 * s_epPanelAlpha)), ep.title.c_str());

                if (epActF > 0.01f) {
                    cDl->AddText(ImVec2(btnPos.x + 18, btnPos.y + 16), IM_COL32(255, 255, 255, (int)(255 * epActF * s_epPanelAlpha)), ep.title.c_str());
                }
                ImGui::Dummy(ImVec2(0, 2));
            }
            ImGui::EndChild();
            PopScrollbarStyles();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        if (s_seasonPopupAlpha > 0.01f) {
            float listHeight = (float)g_ActiveDetails->seasons.size() * 36.0f;
            float winH = std::min(250.0f, listHeight + 32.0f);

            ImGui::SetNextWindowPos(ImVec2(winPos.x + 24, winPos.y + 135));
            ImGui::SetNextWindowSize(ImVec2(comboW, winH));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, 0);
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 40));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f); // Match the sleek look
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, s_seasonPopupAlpha);

            // FIX 2: Use Popup/Tooltip flags which force the window to the absolute front of the stack
            // and bypass the "NoBringToFront" constraints of the background overlay.
            ImGuiWindowFlags popFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;

            if (!s_seasonDropdownOpen) popFlags |= ImGuiWindowFlags_NoMouseInputs;

            // FIX 3: Change the window name to be unique per-session to prevent layout caching
            ImGui::Begin("SeasonListPopup_Live", nullptr, popFlags);
            DrawSleekGlassBackground(s_seasonPopupAlpha);

            float startY = std::max(0.0f, (winH - listHeight) * 0.3f);
            if (startY > 0.0f) ImGui::Dummy(ImVec2(0, startY));

            PushScrollbarStyles(); // APPLY SLEEK SCROLLBARS
            ImGui::BeginChild("SeasonScroll", ImVec2(comboW, winH - startY), false, ImGuiWindowFlags_NoBackground);
            for (int s = 0; s < g_ActiveDetails->seasons.size(); s++) {
                float itemW = comboW - 32.0f;
                ImGui::SetCursorPosX((comboW - itemW) * 0.5f);

                if (DrawSleekRadio((std::string("Sel") + std::to_string(s)).c_str(), g_ActiveDetails->seasons[s].name.c_str(), (g_SelectedSeasonIdx == s), itemW, s_seasonPopupAlpha)) {
                    g_SelectedSeasonIdx = s;
                    g_SelectedEpisodeIdx = 0;
                    s_seasonDropdownOpen = false;
                }
                ImGui::Dummy(ImVec2(0, 2));
            }
            ImGui::EndChild();
            PopScrollbarStyles();
            ImGui::End();
            ImGui::PopStyleVar(4);
            ImGui::PopStyleColor(2);
        }
    }
    else {
        s_seasonDropdownOpen = false;
    }

    static float s_lastSavedVolume = g_Settings.volume;
    if (std::abs(s_lastSavedVolume - g_Settings.volume) > 0.001f && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        WritePrivateProfileStringA("Profile", "Volume", std::to_string(g_Settings.volume).c_str(), ".\\tvanime_settings.ini");
        s_lastSavedVolume = g_Settings.volume;
    }
}