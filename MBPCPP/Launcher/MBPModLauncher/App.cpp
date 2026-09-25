#include "App.hpp"

#include "lucide_imgui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <commdlg.h>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    constexpr ImU32 kText = IM_COL32(245, 247, 255, 255);
    constexpr ImU32 kMuted = IM_COL32(154, 161, 183, 255);
    constexpr ImU32 kPurple = IM_COL32(139, 92, 246, 255);
    constexpr ImU32 kGreenDark = IM_COL32(78, 152, 73, 255);
    constexpr ImU32 kBlue = IM_COL32(0, 89, 157, 255);
    constexpr ImU32 kGreen = IM_COL32(74, 222, 128, 255);
    constexpr ImU32 kRed = IM_COL32(248, 113, 113, 255);

    ImU32 Alpha(ImU32 color, float alpha)
    {
        const int a = std::clamp(static_cast<int>(((color >> 24) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f)), 0, 255);
        return (color & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24);
    }

    ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
}

App::App()
{
    html_.SetHTML(R"HTML(
        <panel id="shell" background="#090B15D9" border="#FFFFFF18" radius="28" border-width="1" shadow="22" />
        <panel id="card" background="#111522C8" border="#FFFFFF18" radius="20" border-width="1" shadow="12" />
        <panel id="status" background="#0B0F19CC" border="#FFFFFF12" radius="14" border-width="1" shadow="8" />
    )HTML");
}

float App::Approach(float current, float target, float speed, float dt)
{
    const float t = 1.0f - std::exp(-speed * std::max(dt, 0.0f));
    return current + (target - current) * t;
}

std::string App::WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string out(static_cast<size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr) != size)
        return {};
    return out;
}

void App::DrawTitleBar(HWND hwnd, ImVec2 origin, ImVec2 size)
{
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImVec2 brandPos(origin.x + 24.0f, origin.y + 18.0f);
    ImGui::SetCursorScreenPos(brandPos);
    Lucide::Sparkles({ .size = 20.0f, .color = kPurple, .thickness = 2.2f, .glowRadius = 5.0f, .glowColor = Alpha(kPurple, 0.45f) });
    draw->AddText(ImVec2(brandPos.x + 30.0f, brandPos.y + 1.0f), kText, "MovieBox Player Mod");

    const float controlY = origin.y + 12.0f;
    const ImVec2 minPos(origin.x + size.x - 86.0f, controlY);
    ImGui::SetCursorScreenPos(minPos);
    ImGui::InvisibleButton("##minimize", ImVec2(34.0f, 34.0f));
    const bool minHover = ImGui::IsItemHovered();
    interactiveHovered_ = interactiveHovered_ || minHover;
    if (ImGui::IsItemClicked()) ShowWindow(hwnd, SW_MINIMIZE);
    if (minHover) draw->AddRectFilled(minPos, Add(minPos, ImVec2(34, 34)), IM_COL32(255, 255, 255, 14), 10.0f);
    ImGui::SetCursorScreenPos(ImVec2(minPos.x + 9.0f, minPos.y + 9.0f));
    Lucide::Minus({ .size = 16.0f, .color = minHover ? kText : kMuted, .thickness = 2.0f });

    const ImVec2 closePos(origin.x + size.x - 46.0f, controlY);
    ImGui::SetCursorScreenPos(closePos);
    ImGui::InvisibleButton("##close", ImVec2(34.0f, 34.0f));
    const bool closeHover = ImGui::IsItemHovered();
    interactiveHovered_ = interactiveHovered_ || closeHover;
    if (ImGui::IsItemClicked()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    if (closeHover) draw->AddRectFilled(closePos, Add(closePos, ImVec2(34, 34)), IM_COL32(248, 113, 113, 34), 10.0f);
    ImGui::SetCursorScreenPos(ImVec2(closePos.x + 9.0f, closePos.y + 9.0f));
    Lucide::X({ .size = 16.0f, .color = closeHover ? kRed : kMuted, .thickness = 2.0f });
}

bool App::DrawPlayerCard(
    const char* id,
    PlayerKind kind,
    const char* title,
    const char* subtitle,
    ImVec2 position,
    ImVec2 size,
    float& hoverAnimation,
    CardIcon icon)
{
    ImGui::SetCursorScreenPos(position);
    ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    interactiveHovered_ = interactiveHovered_ || hovered;
    const bool pressed = ImGui::IsItemClicked();
    const bool selected = selected_ == kind;
    hoverAnimation = Approach(hoverAnimation, hovered ? 1.0f : 0.0f, 15.0f, ImGui::GetIO().DeltaTime);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    html_.DrawPanel(draw, "card", position, Add(position, size));

    const float lift = hoverAnimation * 2.0f;
    if (hoverAnimation > 0.01f)
        draw->AddRect(position, Add(position, size), Alpha(IM_COL32(160, 170, 255, 255), hoverAnimation * 0.22f), 20.0f, 0, 1.0f);

    if (selected)
    {
        const float pulse = 0.55f + 0.12f * std::sin(selectionPulse_);
        draw->AddRectFilled(position, Add(position, size), Alpha(kind == PlayerKind::Cpp ? kBlue : kGreenDark, 0.065f), 20.0f);
        draw->AddRect(position, Add(position, size), Alpha(kind == PlayerKind::Cpp ? kBlue : kGreenDark, pulse), 20.0f, 0, 1.6f);
    }

    const ImVec2 iconTile(position.x + 24.0f, position.y + 25.0f - lift);
    draw->AddRectFilled(iconTile, Add(iconTile, ImVec2(50, 50)), IM_COL32(255, 255, 255, selected ? 18 : 10), 15.0f);
    draw->AddRect(iconTile, Add(iconTile, ImVec2(50, 50)), IM_COL32(255, 255, 255, 18), 15.0f);
    ImGui::SetCursorScreenPos(ImVec2(iconTile.x + 13.0f, iconTile.y + 13.0f));
    Lucide::IconProps iconProps{};
    iconProps.size = 24.0f;
    iconProps.color = selected ? (kind == PlayerKind::Cpp ? kBlue : kGreenDark) : IM_COL32(205, 210, 230, 255);
    iconProps.thickness = 2.0f;
    iconProps.glowRadius = selected ? 6.0f : 0.0f;
    iconProps.glowColor = Alpha(iconProps.color, 0.36f);
    if (icon == CardIcon::Cpu) Lucide::Cpu(iconProps); else Lucide::Braces(iconProps);

    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f, ImVec2(position.x + 24.0f, position.y + 89.0f - lift), kText, title);
    draw->AddText(ImVec2(position.x + 24.0f, position.y + 116.0f - lift), kMuted, subtitle);

    const ImVec2 radio(position.x + size.x - 34.0f, position.y + 28.0f);
    draw->AddCircle(radio, 8.0f, selected ? (kind == PlayerKind::Cpp ? kBlue : kGreenDark) : IM_COL32(255, 255, 255, 45), 24, 1.5f);
    if (selected) draw->AddCircleFilled(radio, 4.0f, kind == PlayerKind::Cpp ? kBlue : kGreenDark, 20);

    if (pressed)
    {
        selected_ = kind;
        errorMessage_.clear();
    }
    return pressed;
}

void App::ToggleContentView()
{
    if (contentTransition_ != ContentTransition::Idle)
        return;

    targetContentView_ = contentView_ == MainContentView::PlayerSelector
        ? MainContentView::Settings
        : MainContentView::PlayerSelector;
    contentTransition_ = ContentTransition::FadingOut;
}

void App::UpdateContentTransition(float dt)
{
    constexpr float kContentFadeSeconds = 0.16f;
    const float step = std::max(0.0f, dt) / kContentFadeSeconds;

    switch (contentTransition_)
    {
    case ContentTransition::FadingOut:
        contentOpacity_ = std::max(0.0f, contentOpacity_ - step);
        if (contentOpacity_ <= 0.0f)
        {
            contentOpacity_ = 0.0f;
            contentView_ = targetContentView_;
            contentTransition_ = ContentTransition::FadingIn;
        }
        break;
    case ContentTransition::FadingIn:
        contentOpacity_ = std::min(1.0f, contentOpacity_ + step);
        if (contentOpacity_ >= 1.0f)
        {
            contentOpacity_ = 1.0f;
            contentTransition_ = ContentTransition::Idle;
        }
        break;
    case ContentTransition::Idle:
        contentOpacity_ = 1.0f;
        break;
    }
}

void App::MultiplyDrawVertexAlpha(ImDrawList* draw, int firstVertex, float opacity)
{
    if (!draw)
        return;
    const float clamped = std::clamp(opacity, 0.0f, 1.0f);
    for (int i = std::max(firstVertex, 0); i < draw->VtxBuffer.Size; ++i)
    {
        ImU32& color = draw->VtxBuffer[i].col;
        const ImU32 alpha = (color >> IM_COL32_A_SHIFT) & 0xFFu;
        const ImU32 scaled = static_cast<ImU32>(std::clamp(static_cast<int>(alpha * clamped + 0.5f), 0, 255));
        color = (color & ~(0xFFu << IM_COL32_A_SHIFT)) | (scaled << IM_COL32_A_SHIFT);
    }
}

void App::DrawPlayerSelector(ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth)
{
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.55f, ImVec2(shellMin.x + 34.0f, shellMin.y + 24.0f), kText, "Choose your player");
    draw->AddText(ImVec2(shellMin.x + 34.0f, shellMin.y + 56.0f), kMuted, "The hook will route MovieBox playback through the implementation you select.");

    const float gap = 18.0f;
    const ImVec2 cardSize((shellWidth - 68.0f - gap) * 0.5f, 154.0f);
    const ImVec2 cppPos(shellMin.x + 34.0f, shellMin.y + 98.0f);
    const ImVec2 csPos(cppPos.x + cardSize.x + gap, cppPos.y);
    DrawPlayerCard("##cpp_player", PlayerKind::Cpp, "C++ Player", "Uses enhanced effects, custom player", cppPos, cardSize, cppHover_, CardIcon::Cpu);
    DrawPlayerCard("##csharp_player", PlayerKind::CSharp, "C# Player", ".NET player highly optimized", csPos, cardSize, csharpHover_, CardIcon::Braces);

    const char* selectionText = selected_ == PlayerKind::Cpp ? "CPP" : "CSHARP";
    draw->AddText(ImVec2(shellMin.x + 34.0f, shellMax.y - 43.0f), kMuted, "MOVIEBOX_PLAYER");
    draw->AddText(ImVec2(shellMin.x + 178.0f, shellMax.y - 43.0f), selected_ == PlayerKind::Cpp ? kBlue : kGreenDark, selectionText);
}

void App::DrawMainContent(HWND hwnd, ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth)
{
    if (contentView_ == MainContentView::PlayerSelector)
    {
        DrawPlayerSelector(draw, shellMin, shellMax, shellWidth);
        return;
    }

    EnsureSettingsEditorInitialized();
    DrawSettings(hwnd, draw, shellMin, shellMax, shellWidth);
}

bool App::Utf8ToWide(std::string_view value, std::wstring& out)
{
    out.clear();
    if (value.empty())
        return true;
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0)
        return false;
    out.assign(static_cast<std::size_t>(count), L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), count) == count;
}

bool App::InputTextString(const char* id, std::string& value, float width)
{
    if (value.capacity() < 256)
        value.reserve(256);

    ImGui::SetNextItemWidth(width);
    const auto callback = [](ImGuiInputTextCallbackData* data) -> int
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto* text = static_cast<std::string*>(data->UserData);
            text->resize(static_cast<std::size_t>(data->BufTextLen));
            data->Buf = text->data();
        }
        return 0;
    };

    return ImGui::InputText(
        id,
        value.data(),
        value.capacity() + 1,
        ImGuiInputTextFlags_CallbackResize,
        callback,
        &value);
}

void App::EnsureSettingsEditorInitialized()
{
    if (settingsEditorInitialized_)
        return;

    const auto& state = settings_.GetMovieBoxPathState();
    const std::filesystem::path initial = !state.savedExecutable.empty()
        ? state.savedExecutable
        : state.executable;
    movieBoxPathEdit_ = initial.empty() ? std::string{} : WideToUtf8(initial.wstring());
    playerDefaultsEdit_ = settings_.GetPlayerDefaults();
    settingsScrollOffset_ = settingsScrollTarget_ = 0.0f;
    settingsDirty_ = false;
    if (!state.persistenceWarning.empty())
        settingsMessage_ = WideToUtf8(state.persistenceWarning);
    else if (state.savedSettingsPresent && !state.savedExecutableExists && !state.savedExecutable.empty())
        settingsMessage_ = "Saved MovieBox location no longer exists. Choose a new install location.";
    settingsEditorInitialized_ = true;
}

bool App::BrowseForMovieBox(HWND hwnd, std::filesystem::path& selected, std::wstring* error)
{
    if (error)
        error->clear();

    std::array<wchar_t, 32768> fileBuffer{};
    std::wstring currentWide;
    if (Utf8ToWide(movieBoxPathEdit_, currentWide) && currentWide.size() < fileBuffer.size())
    {
        std::copy(currentWide.begin(), currentWide.end(), fileBuffer.begin());
        fileBuffer[currentWide.size()] = L'\0';
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileBuffer.data();
    ofn.nMaxFile = static_cast<DWORD>(fileBuffer.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    ofn.lpstrDefExt = L"exe";

    if (GetOpenFileNameW(&ofn))
    {
        selected = std::filesystem::path(fileBuffer.data());
        return true;
    }

    const DWORD dialogError = CommDlgExtendedError();
    if (dialogError != 0 && error)
        *error = L"The MovieBox file picker failed. Common dialog error: " + std::to_wstring(dialogError);
    return false;
}

void App::DrawSettings(HWND hwnd, ImDrawList* draw, ImVec2 shellMin, ImVec2 shellMax, float shellWidth)
{
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.55f, ImVec2(shellMin.x + 34.0f, shellMin.y + 24.0f), kText, "MovieBox Settings");
    draw->AddText(ImVec2(shellMin.x + 34.0f, shellMin.y + 56.0f), kMuted, "Launcher installation and defaults for new MovieBox episodes.");

    const ImVec2 cardMin(shellMin.x + 34.0f, shellMin.y + 92.0f);
    const ImVec2 cardMax(shellMax.x - 34.0f, shellMax.y - 24.0f);
    html_.DrawPanel(draw, "card", cardMin, cardMax, 0.93f);

    const float innerLeft = cardMin.x + 20.0f;
    const float innerRight = cardMax.x - 20.0f;
    const float footerTop = cardMax.y - 66.0f;
    const ImVec2 viewportMin(innerLeft, cardMin.y + 18.0f);
    const ImVec2 viewportMax(innerRight - 13.0f, footerTop - 10.0f);
    const float viewportHeight = std::max(1.0f, viewportMax.y - viewportMin.y);

    const float previousMaxScroll = std::max(0.0f, settingsContentHeight_ - viewportHeight);
    settingsScrollTarget_ = std::clamp(settingsScrollTarget_, 0.0f, previousMaxScroll);
    settingsScrollOffset_ = std::clamp(settingsScrollOffset_, 0.0f, previousMaxScroll);
    const ImGuiIO& io = ImGui::GetIO();
    const bool viewportHovered = ImGui::IsMouseHoveringRect(viewportMin, viewportMax, false);
    if (viewportHovered && std::abs(io.MouseWheel) > 0.001f)
    {
        settingsScrollTarget_ = std::clamp(settingsScrollTarget_ - io.MouseWheel * 72.0f, 0.0f, previousMaxScroll);
        interactiveHovered_ = true;
    }
    const float scrollAlpha = 1.0f - std::exp(-std::max(io.DeltaTime, 0.0f) / 0.09f);
    settingsScrollOffset_ += (settingsScrollTarget_ - settingsScrollOffset_) * scrollAlpha;

    auto markItemInteractive = [&]() { interactiveHovered_ = interactiveHovered_ || ImGui::IsItemHovered() || ImGui::IsItemActive(); };
    auto dirty = [&]() { settingsDirty_ = true; settingsMessage_.clear(); };
    auto drawAction = [&](const char* id, const char* label, ImVec2 pos, ImVec2 actionSize, ImU32 accent) -> bool
    {
        ImGui::SetCursorScreenPos(pos); ImGui::InvisibleButton(id, actionSize);
        const bool hovered = ImGui::IsItemHovered(), clicked = ImGui::IsItemClicked(); markItemInteractive();
        draw->AddRectFilled(pos, Add(pos, actionSize), hovered ? Alpha(accent, 0.22f) : IM_COL32(255,255,255,9), 10.0f);
        draw->AddRect(pos, Add(pos, actionSize), hovered ? Alpha(accent, 0.62f) : IM_COL32(255,255,255,22), 10.0f, 0, 1.0f);
        const ImVec2 ts = ImGui::CalcTextSize(label); draw->AddText(ImVec2(pos.x+(actionSize.x-ts.x)*0.5f,pos.y+(actionSize.y-ts.y)*0.5f), hovered?kText:IM_COL32(211,216,233,255), label);
        return clicked;
    };

    float y = viewportMin.y - settingsScrollOffset_;
    const float contentStartY = viewportMin.y - settingsScrollOffset_;
    const float controlWidth = std::max(180.0f, viewportMax.x - viewportMin.x);
    ImGui::PushClipRect(viewportMin, viewportMax, true);
    draw->PushClipRect(viewportMin, viewportMax, true);

    draw->AddText(ImVec2(innerLeft, y), kText, "MovieBox Installation"); y += 24.0f;
    draw->AddText(ImVec2(innerLeft, y), kMuted, "MovieBox executable"); y += 20.0f;
    ImGui::SetCursorScreenPos(ImVec2(innerLeft, y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f); ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f,9.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(8,11,20,220)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(12,16,28,235)); ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(12,17,30,245));
    if (InputTextString("##moviebox_path", movieBoxPathEdit_, controlWidth)) dirty(); markItemInteractive();
    ImGui::PopStyleColor(3); ImGui::PopStyleVar(2); y += 46.0f;
    if (drawAction("##browse_moviebox", "Browse", ImVec2(innerLeft,y), ImVec2(112,36), kBlue))
    {
        std::filesystem::path selected; std::wstring pickerError;
        if (BrowseForMovieBox(hwnd, selected, &pickerError)) { movieBoxPathEdit_ = WideToUtf8(selected.wstring()); dirty(); settingsMessage_ = "MovieBox executable selected. Save Settings to apply it."; }
        else if (!pickerError.empty()) settingsMessage_ = WideToUtf8(pickerError);
    }
    if (drawAction("##auto_find_moviebox", "Auto Find", ImVec2(innerLeft+124.0f,y), ImVec2(112,36), kPurple))
    {
        std::filesystem::path found;
        if (settings_.AutoFindMovieBox(&found)) { movieBoxPathEdit_=WideToUtf8(found.wstring()); dirty(); settingsMessage_="MovieBox detected."; }
        else settingsMessage_="MovieBox not detected. Please choose an install location.";
    }
    y += 58.0f;
    draw->AddLine(ImVec2(innerLeft,y),ImVec2(viewportMax.x,y),IM_COL32(255,255,255,20)); y += 18.0f;
    draw->AddText(ImVec2(innerLeft,y),kText,"Player Defaults"); y += 25.0f;

    struct Lang { const char* label; const char* code; };
    static constexpr Lang languages[]={{"Off",""},{"English","en"},{"Spanish","es"},{"French","fr"},{"German","de"},{"Italian","it"},{"Portuguese","pt"},{"Japanese","ja"},{"Korean","ko"},{"Chinese","zh"},{"Arabic","ar"},{"Russian","ru"},{"Hindi","hi"}};
    const char* subtitlePreview="Off";
    if(playerDefaultsEdit_.subtitleMode==launcher::SubtitleDefaultMode::Language) for(const auto& l:languages) if(playerDefaultsEdit_.subtitleLanguage==l.code){subtitlePreview=l.label;break;}
    draw->AddText(ImVec2(innerLeft,y),kMuted,"Subtitle default"); y+=19.0f; ImGui::SetCursorScreenPos(ImVec2(innerLeft,y)); ImGui::SetNextItemWidth(controlWidth);
    if(ImGui::BeginCombo("##subtitle_default",subtitlePreview)) { for(const auto& l:languages){const bool off=l.code[0]=='\0'; const bool sel=off?playerDefaultsEdit_.subtitleMode==launcher::SubtitleDefaultMode::Off:(playerDefaultsEdit_.subtitleMode==launcher::SubtitleDefaultMode::Language&&playerDefaultsEdit_.subtitleLanguage==l.code); if(ImGui::Selectable(l.label,sel)){playerDefaultsEdit_.subtitleMode=off?launcher::SubtitleDefaultMode::Off:launcher::SubtitleDefaultMode::Language;playerDefaultsEdit_.subtitleLanguage=off?"":l.code;dirty();} } ImGui::EndCombo(); } markItemInteractive(); y+=38.0f;

    static constexpr int qualityValues[]={0,2160,1440,1080,720,480,360}; static constexpr const char* qualityLabels[]={"Auto","2160p","1440p","1080p","720p","480p","360p"};
    int qi=0; if(playerDefaultsEdit_.qualityMode==launcher::QualityDefaultMode::TargetHeight) for(int i=1;i<7;++i) if(playerDefaultsEdit_.targetQualityHeight==qualityValues[i]) qi=i;
    draw->AddText(ImVec2(innerLeft,y),kMuted,"Target quality"); y+=19.0f; ImGui::SetCursorScreenPos(ImVec2(innerLeft,y)); ImGui::SetNextItemWidth(controlWidth);
    if(ImGui::BeginCombo("##quality_default",qualityLabels[qi])) { for(int i=0;i<7;++i){if(ImGui::Selectable(qualityLabels[i],i==qi)){playerDefaultsEdit_.qualityMode=i==0?launcher::QualityDefaultMode::Auto:launcher::QualityDefaultMode::TargetHeight;playerDefaultsEdit_.targetQualityHeight=(std::uint16_t)qualityValues[i];dirty();}} ImGui::EndCombo(); } markItemInteractive(); y+=38.0f;

    const char* serverPreview=playerDefaultsEdit_.serverMode==launcher::ServerDefaultMode::Auto?"Auto":"Preferred";
    draw->AddText(ImVec2(innerLeft,y),kMuted,"Default server"); y+=19.0f; ImGui::SetCursorScreenPos(ImVec2(innerLeft,y)); ImGui::SetNextItemWidth(controlWidth);
    if(ImGui::BeginCombo("##server_default_mode",serverPreview)) { if(ImGui::Selectable("Auto",playerDefaultsEdit_.serverMode==launcher::ServerDefaultMode::Auto)){playerDefaultsEdit_.serverMode=launcher::ServerDefaultMode::Auto;dirty();} if(ImGui::Selectable("Preferred",playerDefaultsEdit_.serverMode==launcher::ServerDefaultMode::Preferred)){playerDefaultsEdit_.serverMode=launcher::ServerDefaultMode::Preferred;dirty();} ImGui::EndCombo(); } markItemInteractive(); y+=38.0f;
    if(playerDefaultsEdit_.serverMode==launcher::ServerDefaultMode::Preferred){draw->AddText(ImVec2(innerLeft,y),kMuted,"Preferred server ID or label"); y+=19.0f; ImGui::SetCursorScreenPos(ImVec2(innerLeft,y)); if(InputTextString("##preferred_server",playerDefaultsEdit_.serverPreference,controlWidth))dirty();markItemInteractive();y+=38.0f;}

    draw->AddText(ImVec2(innerLeft,y),kMuted,"Default audio level"); y+=19.0f; int volume=(int)playerDefaultsEdit_.volume; ImGui::SetCursorScreenPos(ImVec2(innerLeft,y)); ImGui::SetNextItemWidth(controlWidth-54.0f);
    if(ImGui::SliderInt("##default_volume",&volume,0,100,"%d%%")){playerDefaultsEdit_.volume=(std::uint8_t)volume;dirty();} markItemInteractive(); y+=42.0f;
    draw->AddText(ImVec2(innerLeft,y),kMuted,"Defaults apply only when an episode has no saved .cock profile."); y+=28.0f;

    draw->PopClipRect(); ImGui::PopClipRect();
    settingsContentHeight_ = std::max(0.0f, y + settingsScrollOffset_ - viewportMin.y);
    const float maxScroll = std::max(0.0f, settingsContentHeight_ - viewportHeight);
    settingsScrollTarget_=std::clamp(settingsScrollTarget_,0.0f,maxScroll); settingsScrollOffset_=std::clamp(settingsScrollOffset_,0.0f,maxScroll);

    if(maxScroll>0.0f)
    {
        const ImVec2 trackMin(viewportMax.x+5.0f,viewportMin.y), trackMax(innerRight,viewportMax.y); const float trackH=trackMax.y-trackMin.y;
        const float thumbH=std::max(28.0f,trackH*(viewportHeight/settingsContentHeight_)); const float travel=std::max(1.0f,trackH-thumbH); const float frac=maxScroll>0?settingsScrollOffset_/maxScroll:0; const float thumbY=trackMin.y+travel*frac;
        ImGui::SetCursorScreenPos(trackMin); ImGui::InvisibleButton("##settings_scrollbar",ImVec2(trackMax.x-trackMin.x,trackH)); const bool sbHover=ImGui::IsItemHovered(); settingsScrollbarDragging_=ImGui::IsItemActive(); interactiveHovered_=interactiveHovered_||sbHover||settingsScrollbarDragging_;
        if(ImGui::IsItemClicked()||settingsScrollbarDragging_){const float mouseFrac=std::clamp((io.MousePos.y-trackMin.y-thumbH*0.5f)/travel,0.0f,1.0f); settingsScrollTarget_=settingsScrollOffset_=mouseFrac*maxScroll;}
        const float width=(sbHover||settingsScrollbarDragging_)?5.0f:3.0f; const float cx=(trackMin.x+trackMax.x)*0.5f;
        draw->AddRectFilled(ImVec2(cx-1.0f,trackMin.y),ImVec2(cx+1.0f,trackMax.y),IM_COL32(255,255,255,18),2.0f);
        draw->AddRectFilled(ImVec2(cx-width*0.5f,thumbY),ImVec2(cx+width*0.5f,thumbY+thumbH),(sbHover||settingsScrollbarDragging_)?IM_COL32(175,145,250,190):IM_COL32(160,170,205,110),4.0f);
    }

    if(settingsDirty_){const char* dirtyText="Unsaved"; const ImVec2 ds=ImGui::CalcTextSize(dirtyText); draw->AddText(ImVec2(innerRight-ds.x,footerTop+6.0f),IM_COL32(250,204,90,230),dirtyText);}
    if(!settingsMessage_.empty()) { const bool positive=settingsMessage_=="MovieBox detected."||settingsMessage_=="Settings saved."; draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*0.82f,ImVec2(innerLeft,footerTop+5.0f),positive?IM_COL32(158,235,180,255):IM_COL32(238,185,150,255),settingsMessage_.c_str(),nullptr,std::max(80.0f,innerRight-innerLeft-160.0f)); }
    const ImVec2 saveSize(140.0f,36.0f), savePos(innerRight-saveSize.x,cardMax.y-45.0f);
    if(drawAction("##save_moviebox_settings","Save Settings",savePos,saveSize,kGreenDark))
    {
        std::wstring candidateWide;
        if(!Utf8ToWide(movieBoxPathEdit_,candidateWide)) settingsMessage_="The selected path is not valid UTF-8.";
        else { std::wstring reason; if(!settings_.SaveAll(std::filesystem::path(candidateWide),playerDefaultsEdit_,&reason)) settingsMessage_=WideToUtf8(reason); else { settingsDirty_=false; settingsMessage_="Settings saved."; errorMessage_.clear(); playerDefaultsEdit_=settings_.GetPlayerDefaults(); const auto& state=settings_.GetMovieBoxPathState(); movieBoxPathEdit_=state.savedExecutable.empty()?std::string{}:WideToUtf8(state.savedExecutable.wstring()); } }
    }
}

void App::DrawStatus(ImDrawList* draw, ImVec2 origin, ImVec2 size)
{
    std::wstring reason;
    const bool ready = settings_.IsReady(selected_, &reason);
    const ImVec2 statusMin(origin.x + 48.0f, origin.y + size.y - 108.0f);
    const ImVec2 statusMax(origin.x + size.x - 48.0f, statusMin.y + 50.0f);
    html_.DrawPanel(draw, "status", statusMin, statusMax, 0.85f);

    const bool hasError = !errorMessage_.empty();

    std::string statusText;
    if (hasError)
        statusText = errorMessage_;
    else if (!ready)
        statusText = WideToUtf8(reason);
    else
        statusText = "Ready to launch";

    const char* backend = selected_ == PlayerKind::Cpp ? "PlayerCPP" : "PlayerC#";
    const ImVec2 backendSize = ImGui::CalcTextSize(backend);
    const float backendX = statusMax.x - backendSize.x - 18.0f;

    constexpr float dotRadius = 4.0f;
    constexpr float dotDiameter = dotRadius * 2.0f;
    constexpr float dotGap = 10.0f;
    constexpr float sidePadding = 18.0f;

    const float statusFontSize = ImGui::GetFontSize() * 0.88f;
    const float messageLeft = statusMin.x + sidePadding;
    const float messageRight = backendX - sidePadding;
    const float messageWidth = std::max(1.0f, messageRight - messageLeft);
    const float statusWrapWidth = std::max(1.0f, messageWidth - dotDiameter - dotGap);
    const ImVec2 statusTextSize = ImGui::GetFont()->CalcTextSizeA(
        statusFontSize,
        1000000.0f,
        statusWrapWidth,
        statusText.c_str());

    const float groupX = messageLeft;
    const float panelHeight = statusMax.y - statusMin.y;
    const float textY = statusMin.y + std::max(4.0f, (panelHeight - statusTextSize.y) * 0.5f);
    const float dotY = std::clamp(textY + statusTextSize.y * 0.5f, statusMin.y + dotRadius + 4.0f, statusMax.y - dotRadius - 4.0f);

    draw->AddCircleFilled(
        ImVec2(groupX + dotRadius, dotY),
        dotRadius,
        (ready && !hasError) ? kGreen : kRed,
        16);

    draw->AddText(
        ImGui::GetFont(),
        statusFontSize,
        ImVec2(groupX + dotDiameter + dotGap, textY),
        (ready && !hasError) ? IM_COL32(192, 245, 209, 255) : IM_COL32(255, 191, 191, 255),
        statusText.c_str(),
        nullptr,
        statusWrapWidth);

    draw->AddText(ImVec2(backendX, statusMin.y + 16.0f), kMuted, backend);
}

void App::AttemptLaunch(HWND hwnd)
{
    settings_.Refresh();
    const launcher::LaunchResult result = settings_.Launch(selected_);
    if (!result.success)
    {
        errorMessage_ = WideToUtf8(result.message);
        return;
    }

    errorMessage_.clear();
    PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void App::DrawLaunchButton(HWND hwnd, ImDrawList* draw, ImVec2 origin, ImVec2 size)
{
    const ImVec2 buttonSize(238.0f, 56.0f);
    const ImVec2 buttonPos(origin.x + (size.x - buttonSize.x) * 0.5f, origin.y + size.y - 174.0f);
    ImGui::SetCursorScreenPos(buttonPos);
    ImGui::InvisibleButton("##launch", buttonSize);
    const bool hovered = ImGui::IsItemHovered();
    interactiveHovered_ = interactiveHovered_ || hovered;
    const bool clicked = ImGui::IsItemClicked();
    launchHover_ = Approach(launchHover_, hovered ? 1.0f : 0.0f, 16.0f, ImGui::GetIO().DeltaTime);

    const ImU32 base = selected_ == PlayerKind::Cpp ? IM_COL32(40, 104, 232, 238) : IM_COL32(98, 172, 73, 255);
    if (launchHover_ > 0.01f)
    {
        for (int i = 4; i >= 1; --i)
        {
            const float ex = static_cast<float>(i) * 2.6f;
            draw->AddRectFilled(ImVec2(buttonPos.x - ex, buttonPos.y - ex), ImVec2(buttonPos.x + buttonSize.x + ex, buttonPos.y + buttonSize.y + ex), Alpha(base, launchHover_ * (0.025f * (5 - i))), 19.0f + ex);
        }
    }
    draw->AddRectFilled(buttonPos, Add(buttonPos, buttonSize), base, 17.0f);
    draw->AddRect(buttonPos, Add(buttonPos, buttonSize), IM_COL32(255, 255, 255, hovered ? 55 : 34), 17.0f);

    const ImVec2 iconPos(buttonPos.x + 54.0f, buttonPos.y + 18.0f);
    ImGui::SetCursorScreenPos(iconPos);
    Lucide::Play({ .size = 20.0f, .color = IM_COL32_WHITE, .thickness = 2.0f });
    draw->AddText(ImVec2(buttonPos.x + 86.0f, buttonPos.y + 18.0f), IM_COL32_WHITE, "Launch MovieBox");

    if (clicked) AttemptLaunch(hwnd);
}

void App::DrawSettingsGear(ImDrawList* draw, ImVec2 origin, ImVec2 size)
{
    const ImVec2 buttonSize(238.0f, 56.0f);
    const ImVec2 buttonPos(origin.x + (size.x - buttonSize.x) * 0.5f, origin.y + size.y - 174.0f);
    const ImVec2 gearSize(44.0f, 44.0f);
    const ImVec2 gearPos(
        buttonPos.x + buttonSize.x + 12.0f,
        buttonPos.y + (buttonSize.y - gearSize.y) * 0.5f);

    ImGui::SetCursorScreenPos(gearPos);
    ImGui::InvisibleButton("##settings_gear", gearSize);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    interactiveHovered_ = interactiveHovered_ || hovered;
    settingsGearHover_ = Approach(settingsGearHover_, hovered ? 1.0f : 0.0f, 16.0f, ImGui::GetIO().DeltaTime);

    const ImU32 fill = IM_COL32(17, 21, 34, static_cast<int>(210 + 20 * settingsGearHover_));
    draw->AddRectFilled(gearPos, Add(gearPos, gearSize), fill, 14.0f);
    draw->AddRect(gearPos, Add(gearPos, gearSize), IM_COL32(255, 255, 255, static_cast<int>(28 + 35 * settingsGearHover_)), 14.0f, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(gearPos.x + 12.0f, gearPos.y + 12.0f));
    Lucide::Settings({
        .size = 20.0f,
        .color = hovered || contentView_ == MainContentView::Settings ? kText : kMuted,
        .thickness = 2.0f,
        .glowRadius = hovered ? 4.0f : 0.0f,
        .glowColor = Alpha(kPurple, 0.30f)
    });

    if (clicked && contentTransition_ == ContentTransition::Idle)
        ToggleContentView();
}

void App::Render(HWND hwnd)
{
    ImGuiIO& io = ImGui::GetIO();
    selectionPulse_ += io.DeltaTime * 2.5f;
    UpdateContentTransition(io.DeltaTime);
    interactiveHovered_ = false;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::Begin("##launcher_root", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNav);

    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Dark veil keeps text and cards stable while the DX12 loader animates underneath.
    draw->AddRectFilled(origin, Add(origin, size), IM_COL32(3, 5, 13, 66));
    DrawTitleBar(hwnd, origin, size);

    const float shellWidth = std::min(760.0f, size.x - 72.0f);
    const float shellHeight = 330.0f;
    const ImVec2 shellMin(origin.x + (size.x - shellWidth) * 0.5f, origin.y + 104.0f);
    const ImVec2 shellMax(shellMin.x + shellWidth, shellMin.y + shellHeight);
    html_.DrawPanel(draw, "shell", shellMin, shellMax, 0.96f);

    const int firstContentVertex = draw->VtxBuffer.Size;
    if (contentTransition_ != ContentTransition::Idle)
        ImGui::BeginDisabled();
    DrawMainContent(hwnd, draw, shellMin, shellMax, shellWidth);
    if (contentTransition_ != ContentTransition::Idle)
        ImGui::EndDisabled();
    MultiplyDrawVertexAlpha(draw, firstContentVertex, contentOpacity_);

    DrawLaunchButton(hwnd, draw, origin, size);
    DrawSettingsGear(draw, origin, size);
    DrawStatus(draw, origin, size);

    const char* footer = "Enhanced Player Mod for Movie Box Pro!";
    const ImVec2 footerSize = ImGui::CalcTextSize(footer);
    draw->AddText(ImVec2(origin.x + (size.x - footerSize.x) * 0.5f, origin.y + size.y - 38.0f), IM_COL32(118, 126, 151, 210), footer);

    // Let ImGui decide whether the click belongs to an interactive item first.
    // Everything else inside the launcher acts as draggable window chrome.
    const bool mouseInsideWindow = ImGui::IsMouseHoveringRect(origin, Add(origin, size), false);
    if (mouseInsideWindow && !interactiveHovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);

        // Windows consumes the physical release while inside the non-client move loop,
        // so ImGui's Win32 backend never sees the matching client WM_LBUTTONUP.
        // Feed that release back through the normal window procedure to clear both
        // ImGui IO state and the backend's internal MouseButtonsDown bookkeeping.
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(hwnd, &cursor);
            SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(cursor.x, cursor.y));
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}
