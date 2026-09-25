#pragma once

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// ImGui
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "HtmlRenderer.hpp"

#include "lucide_imgui.hpp"
#include "fa_icons.h"
#include "poppins.h"

inline constexpr char ICON_FA_TIMES[] = "\xef\x80\x8d";

// Expose DirectX Globals so VLC can build textures
extern ID3D11Device* g_pd3dDevice;
extern ID3D11DeviceContext* g_pd3dDeviceContext;

// Application Global States
enum AppMode {
    MODE_PROFILES = 0,
    MODE_DASHBOARD,
    MODE_PLAYER
};

struct PlayerSettings {
    int engineMode = 1;      // 0 = Custom DirectShow, 1 = VLC Texture
    int hwDecoding = 1;
    float volume = 1.0f;
    float progress = 0.0f;
    bool isPlaying = true;
    std::string videoTitle = "";
    std::string sourceName = "";
    std::string activeSub = "";
    std::string activeSeason = "";

    // MovieBox bridge session state. These fields are only authoritative when
    // bridgeMode is true; standalone/direct-URI mode continues to use the
    // existing local player state above.
    bool bridgeMode = false;
    long long bridgeRevision = 0;
    double bridgeResumeSeconds = 0.0;
    double bridgePlaybackRate = 1.0;
    double bridgeAudioDelaySeconds = 0.0;
    bool muted = false;
    short bridgeSeason = 0;
    short bridgeEpisode = 0;
    unsigned char bridgeBoxType = 0;
    int bridgeAudioTrackIndex = -1; // Preserved in C#-compatible .cock files even when not applied by this UI.
    std::string bridgeSettingsTitle = "";
    std::string userAgent = "";

    // Core Engine Triggers
    std::string currentUrl = "";
    bool requestPlay = false;
    bool videoPlayerQuits = false; // --VPQ True: player X closes the full application
};

// Application State Tracking Snapshot
struct HistoryStats {
    float progress = 0.0f;
    std::string season = "";
    std::string episodeName = "";
    std::string url = "";
};

struct MockEpisode { std::string title; std::string url; };
struct MockSeason { std::string name; std::vector<MockEpisode> eps; };
struct MockMedia {
    std::string title;
    std::string type;
    std::string source;
    std::string poster;
    std::vector<MockSeason> seasons;
    HistoryStats historyStats;
};

struct UserProfile {
    std::string id;
    std::string name;
    std::string avatar;
    std::string defaultQuality = "Auto";
    bool autoSignIn = false;
    bool enableLocalMedia = true;
    std::string localDir = "C:\\Users\\Default\\Videos";
    float defaultVolume = 1.0f; // Restored here to preserve guimain.cpp aggregate initialization order

    // Persistent profile datasets
    std::vector<MockMedia> favorites;
    std::vector<MockMedia> history;
};

extern PlayerSettings g_Settings;
extern AppMode g_AppMode;
extern UserProfile g_ActiveProfile;
extern std::vector<UserProfile> g_Profiles;

extern bool g_IsPIPMode;

extern std::vector<MockMedia> g_LocalLibrary;
extern MockMedia* g_ActiveDetails;
extern int g_SelectedSeasonIdx;
extern int g_SelectedEpisodeIdx;

void ScanLocalMedia();

// HLSL Shader Exports
void InitShaders();
void ClearShaderState();
void PushGlow(ImDrawList* drawList, ImU32 glowColor, float intensity);
void PopGlow(ImDrawList* drawList);

// New animated mist/smoke glow. The legacy PushGlow/PopGlow path above is
// intentionally preserved as a fallback/reference.
void PushMistGlow(ImDrawList* drawList, ImU32 glowColor, float intensity, float timeSeconds);

// Shape-aware version used by the player controls. The shader emits smoke from
// the supplied core shape center rather than behaving like a fullscreen field.
// shapeMode: 0 = rounded rect, 1 = circle, 2 = horizontal capsule.
void PushMistGlowShape(
    ImDrawList* drawList,
    ImU32 glowColor,
    float intensity,
    float timeSeconds,
    ImVec2 center,
    ImVec2 halfSize,
    float roundness,
    float feather,
    float shapeMode);

void PopMistGlow(ImDrawList* drawList);

void DrawBlurRect(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImVec4 overlayColor, float blurRadius);
void DrawLoadingShader(ImDrawList* drawList, ImVec2 min, ImVec2 max, float timeSeconds, float opacity);

// Forward declarations
void RenderApp(HWND hWnd);
void RenderVideoEngine(HWND hWnd);
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void HandleAppDrag(HWND hWnd);
void TogglePIP(HWND hwnd);
void RenderFrame(HWND hwnd);