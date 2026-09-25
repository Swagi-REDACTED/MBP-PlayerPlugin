#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imgui.h>

#include "Settings.hpp"
#include "HtmlUi.hpp"

#include <filesystem>
#include <string>
#include <string_view>

class App
{
public:
    App();

    void Render(HWND hwnd);
    float BackgroundOpacity() const noexcept { return backgroundOpacity_; }
    bool IsCppSelected() const noexcept { return selected_ == PlayerKind::Cpp; }

private:
    using PlayerKind = launcher::PlayerKind;
    enum class CardIcon { Cpu, Braces };
    enum class MainContentView { PlayerSelector, Settings };
    enum class ContentTransition { Idle, FadingOut, FadingIn };

    bool DrawPlayerCard(
        const char* id,
        PlayerKind kind,
        const char* title,
        const char* subtitle,
        ImVec2 position,
        ImVec2 size,
        float& hoverAnimation,
        CardIcon icon);

    void DrawTitleBar(HWND hwnd, ImVec2 origin, ImVec2 size);
    void DrawStatus(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    void DrawLaunchButton(HWND hwnd, ImDrawList* draw, ImVec2 origin, ImVec2 size);
    void DrawSettingsGear(ImDrawList* draw, ImVec2 origin, ImVec2 size);
    void DrawMainContent(HWND hwnd, ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth);
    void DrawPlayerSelector(ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth);
    void ToggleContentView();
    void UpdateContentTransition(float dt);
    static void MultiplyDrawVertexAlpha(ImDrawList* draw, int firstVertex, float opacity);
    void AttemptLaunch(HWND hwnd);

    // Added in the Settings-content task; declarations live here so the content slot has one stable boundary.
    void EnsureSettingsEditorInitialized();
    void DrawSettings(HWND hwnd, ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth);
    bool BrowseForMovieBox(HWND hwnd, std::filesystem::path& selected, std::wstring* error = nullptr);
    static bool InputTextString(const char* id, std::string& value, float width);
    static bool Utf8ToWide(std::string_view value, std::wstring& out);

    static std::string WideToUtf8(std::wstring_view value);
    static float Approach(float current, float target, float speed, float dt);

    launcher::Settings settings_;
    htmlui::Renderer html_;
    PlayerKind selected_ = PlayerKind::Cpp;
    MainContentView contentView_ = MainContentView::PlayerSelector;
    MainContentView targetContentView_ = MainContentView::PlayerSelector;
    ContentTransition contentTransition_ = ContentTransition::Idle;
    float contentOpacity_ = 1.0f;
    float settingsGearHover_ = 0.0f;
    float cppHover_ = 0.0f;
    float csharpHover_ = 0.0f;
    float launchHover_ = 0.0f;
    float selectionPulse_ = 0.0f;
    float backgroundOpacity_ = 1.0f;
    bool interactiveHovered_ = false;
    std::string movieBoxPathEdit_;
    launcher::PlayerDefaults playerDefaultsEdit_{};
    float settingsScrollOffset_ = 0.0f;
    float settingsScrollTarget_ = 0.0f;
    float settingsContentHeight_ = 0.0f;
    bool settingsScrollbarDragging_ = false;
    bool settingsEditorInitialized_ = false;
    bool settingsDirty_ = false;
    std::string settingsMessage_;
    std::string errorMessage_;
};
