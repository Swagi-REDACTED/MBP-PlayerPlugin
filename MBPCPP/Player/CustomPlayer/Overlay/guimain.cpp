#include "main.hpp"
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
#include <cctype>

namespace fs = std::filesystem;

static VOID CALLBACK MaximizeTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    KillTimer(hwnd, idEvent);
    if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    else ShowWindow(hwnd, SW_MAXIMIZE);
}

static int s_ActiveNav = 0;
static char s_SearchBuf[256] = "";
static bool s_IsSearching = false;
static float s_SidebarW = 260.0f;

static bool s_IsAddingProfile = false;
static char s_NewProfileName[128] = "";

static ImU32 colBg = IM_COL32(11, 14, 20, 255);
static ImU32 colSidebar = IM_COL32(17, 24, 39, 204);
static ImU32 colAccentP = IM_COL32(139, 92, 246, 255);
static ImU32 colAccentB = IM_COL32(59, 130, 246, 255);
static ImU32 colCardBg = IM_COL32(255, 255, 255, 8);
static ImU32 colBorder = IM_COL32(255, 255, 255, 20);

std::vector<MockMedia> g_LocalLibrary;
static std::vector<MockMedia> s_FavoritesSnapshot; // Caches state for smooth un-favoriting visuals
MockMedia* g_ActiveDetails = nullptr;
int g_SelectedSeasonIdx = 0;
int g_SelectedEpisodeIdx = 0;
static char s_MediaDirBuf[512] = "";

std::string GetDefaultMediaPath() {
    char* userProfile = nullptr;
    size_t len;
    _dupenv_s(&userProfile, &len, "USERPROFILE");
    std::string path = "";
    if (userProfile) {
        path = std::string(userProfile) + "\\Videos\\MovieBoxPro";
        free(userProfile);
    }
    else {
        path = "C:\\Videos\\MovieBoxPro";
    }
    return path;
}

bool IsVideoFile(const fs::path& p) {
    if (!p.has_extension()) return false;
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return (ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".wmv");
}

void ScanLocalMedia() {
    // Prevent dangling pointer crashes when reloading memory underneath the modal
    g_ActiveDetails = nullptr;
    g_LocalLibrary.clear();

    fs::path rootPath(g_ActiveProfile.localDir);
    if (!fs::exists(rootPath) || !fs::is_directory(rootPath)) {
        std::error_code ec;
        fs::create_directories(rootPath, ec);
        return;
    }

    for (const auto& mediaEntry : fs::directory_iterator(rootPath)) {
        if (!mediaEntry.is_directory()) continue;

        MockMedia media;
        media.title = mediaEntry.path().filename().string();
        media.source = "Local Disk";
        media.type = "Movie";

        bool isSeries = false;

        for (const auto& subEntry : fs::directory_iterator(mediaEntry.path())) {
            if (subEntry.is_directory()) {
                isSeries = true;
                MockSeason season;

                std::string sName = subEntry.path().filename().string();
                if (sName.starts_with("Season") && sName.length() > 6 && isdigit(sName[6])) {
                    sName.insert(6, " "); // Convert "Season1" to "Season 1"
                }
                season.name = sName;

                for (const auto& epEntry : fs::directory_iterator(subEntry.path())) {
                    if (epEntry.is_directory()) {
                        for (const auto& fileEntry : fs::directory_iterator(epEntry.path())) {
                            if (fileEntry.is_regular_file() && IsVideoFile(fileEntry.path())) {
                                MockEpisode ep;
                                ep.title = fileEntry.path().stem().string();
                                ep.url = fileEntry.path().string();
                                season.eps.push_back(ep);
                                break;
                            }
                        }
                    }
                    else if (epEntry.is_regular_file() && IsVideoFile(epEntry.path())) {
                        MockEpisode ep;
                        ep.title = epEntry.path().stem().string();
                        ep.url = epEntry.path().string();
                        season.eps.push_back(ep);
                    }
                }
                if (!season.eps.empty()) {
                    media.seasons.push_back(season);
                }
            }
            else if (subEntry.is_regular_file() && IsVideoFile(subEntry.path())) {
                MockSeason s;
                s.name = "Feature";
                MockEpisode ep;
                ep.title = media.title;
                ep.url = subEntry.path().string();
                s.eps.push_back(ep);
                media.seasons.push_back(s);
            }
        }

        if (isSeries) media.type = "Series";
        if (!media.seasons.empty()) g_LocalLibrary.push_back(media);
    }
}

void RenderApp(HWND hWnd)
{
    if (g_Profiles.empty()) {
        g_Profiles.push_back({ "1", "John Clout", "", "Auto", false, true, GetDefaultMediaPath() });
        g_ActiveProfile = g_Profiles[0];
    }
    if (s_MediaDirBuf[0] == '\0') strncpy_s(s_MediaDirBuf, g_ActiveProfile.localDir.c_str(), sizeof(s_MediaDirBuf) - 1);
    if (g_LocalLibrary.empty()) ScanLocalMedia();

    // Re-scan dynamically on tab swapping for true local sync & Snapshot Favorites
    static int s_LastNav = 0;
    if (s_ActiveNav != s_LastNav) {
        if (s_ActiveNav <= 2) {
            ScanLocalMedia();
        }
        if (s_ActiveNav == 3) {
            s_FavoritesSnapshot = g_ActiveProfile.favorites; // Prevent visual popping
        }
        s_LastNav = s_ActiveNav;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screenSize = io.DisplaySize;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

    // ==========================================
    // PROFILE SELECTOR
    // ==========================================
    if (g_AppMode == MODE_PROFILES) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(screenSize);
        ImGui::Begin("ProfileOverlay", nullptr, ImGuiWindowFlags_NoDecoration);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        HandleAppDrag(hWnd);

        dl->AddRectFilled(ImVec2(0, 0), screenSize, colBg);
        ImVec2 center = ImVec2(screenSize.x * 0.5f, screenSize.y * 0.5f);
        dl->AddCircleFilled(center, screenSize.y * 0.8f, IM_COL32(139, 92, 246, 25), 64);

        // --- GLOBAL APP CLOSE BUTTON (PROFILES) ---
        ImVec2 pClosePos(screenSize.x - 35, 30);
        ImGui::SetCursorScreenPos(ImVec2(pClosePos.x - 5, pClosePos.y - 5));
        if (ImGui::InvisibleButton("AppCloseProfile", ImVec2(30, 30))) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
        bool pCloseHov = ImGui::IsItemHovered();

        Lucide::IconProps pCloseProps; pCloseProps.size = 20.0f; pCloseProps.color = pCloseHov ? IM_COL32(255, 100, 100, 255) : IM_COL32_WHITE; pCloseProps.thickness = 2.0f; pCloseProps.hoverThickness = 2.5f; pCloseProps.animated = true;
        ImGui::SetCursorScreenPos(pClosePos);
        Lucide::Icon("X", pCloseProps);

        if (!s_IsAddingProfile) {
            const char* title = "Who's watching?";
            ImGui::SetWindowFontScale(2.5f);
            ImVec2 tSize = ImGui::CalcTextSize(title);
            dl->AddText(ImVec2(center.x - tSize.x * 0.5f, center.y - 150), IM_COL32(255, 255, 255, 255), title);
            ImGui::SetWindowFontScale(1.0f);

            float cardW = 150.0f;
            float totalW = (g_Profiles.size() + 1) * cardW + (g_Profiles.size() * 40.0f);
            float startX = center.x - (totalW * 0.5f);

            for (size_t i = 0; i < g_Profiles.size(); i++) {
                ImVec2 pPos = ImVec2(startX, center.y - 50);
                ImGui::SetCursorScreenPos(pPos);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::InvisibleButton("ProfileBtn", ImVec2(cardW, cardW))) {
                    g_ActiveProfile = g_Profiles[i];
                    g_AppMode = MODE_DASHBOARD;
                }
                bool pHov = ImGui::IsItemHovered();
                ImGui::PopID();

                dl->AddRectFilled(pPos, ImVec2(pPos.x + cardW, pPos.y + cardW), IM_COL32(50, 50, 60, 255), 24.0f);
                if (pHov) dl->AddRect(pPos, ImVec2(pPos.x + cardW, pPos.y + cardW), colAccentP, 24.0f, 0, 4.0f);

                ImVec2 nSize = ImGui::CalcTextSize(g_Profiles[i].name.c_str());
                dl->AddText(ImVec2(pPos.x + (cardW - nSize.x) * 0.5f, pPos.y + cardW + 15), pHov ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 160, 170, 255), g_Profiles[i].name.c_str());

                startX += cardW + 40.0f;
            }

            ImVec2 aPos = ImVec2(startX, center.y - 50);
            ImGui::SetCursorScreenPos(aPos);
            if (ImGui::InvisibleButton("Profile_Add", ImVec2(cardW, cardW))) s_IsAddingProfile = true;
            bool aHov = ImGui::IsItemHovered();

            dl->AddRect(aPos, ImVec2(aPos.x + cardW, aPos.y + cardW), IM_COL32(255, 255, 255, 50), 24.0f, 0, 2.0f);

            ImGui::SetCursorScreenPos(ImVec2(aPos.x + cardW * 0.5f - 24.0f, aPos.y + cardW * 0.5f - 24.0f));
            Lucide::IconProps plusProps; plusProps.size = 48.0f; plusProps.color = aHov ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 100); plusProps.thickness = 2.0f; plusProps.hoverThickness = 2.5f; plusProps.animated = true;
            Lucide::Icon("Plus", plusProps);

            dl->AddText(ImVec2(aPos.x + 35, aPos.y + cardW + 15), aHov ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 160, 170, 255), "Add Profile");
        }
        else {
            ImVec2 modalSize(400, 250);
            ImVec2 mPos(center.x - modalSize.x * 0.5f, center.y - modalSize.y * 0.5f);
            dl->AddRectFilled(mPos, ImVec2(mPos.x + modalSize.x, mPos.y + modalSize.y), IM_COL32(20, 24, 34, 255), 16.0f);
            dl->AddRect(mPos, ImVec2(mPos.x + modalSize.x, mPos.y + modalSize.y), colBorder, 16.0f);

            dl->AddText(ImVec2(mPos.x + 30, mPos.y + 30), IM_COL32_WHITE, "Create New Profile");

            ImGui::SetCursorScreenPos(ImVec2(mPos.x + 30, mPos.y + 80));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 10));
            ImGui::InputText("##NameInput", s_NewProfileName, sizeof(s_NewProfileName));
            ImGui::PopStyleColor();

            ImGui::SetCursorScreenPos(ImVec2(mPos.x + 30, mPos.y + 180));
            if (ImGui::Button("Cancel", ImVec2(100, 35))) {
                s_IsAddingProfile = false;
            }
            ImGui::SameLine(mPos.x + modalSize.x - 130);
            if (ImGui::Button("Create", ImVec2(100, 35))) {
                if (strlen(s_NewProfileName) > 0) {
                    UserProfile np; np.name = s_NewProfileName;
                    g_Profiles.push_back(np);
                    s_NewProfileName[0] = '\0';
                }
                s_IsAddingProfile = false;
            }
        }

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        return;
    }

    // ==========================================
    // DASHBOARD MAIN
    // ==========================================
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(s_SidebarW, screenSize.y));
    ImGui::Begin("Sidebar", nullptr, ImGuiWindowFlags_NoDecoration);
    ImDrawList* sDl = ImGui::GetWindowDrawList();
    HandleAppDrag(hWnd);

    sDl->AddRectFilled(ImVec2(0, 0), ImVec2(s_SidebarW, screenSize.y), colSidebar);
    sDl->AddLine(ImVec2(s_SidebarW, 0), ImVec2(s_SidebarW, screenSize.y), colBorder);

    sDl->AddCircleFilled(ImVec2(30, 40), 6.0f, colAccentP);
    ImGui::SetWindowFontScale(1.3f);
    sDl->AddText(ImVec2(48, 28), IM_COL32_WHITE, "TvAnime");
    ImGui::SetWindowFontScale(1.0f);

    const char* navs[] = { "Home", "Movies", "TV Series", "Favorites", "History" };
    const char* icons[] = { "Home", "Film", "Tv", "Heart", "History" };

    float cY = 100.0f;
    for (int i = 0; i < 5; ++i) {
        ImVec2 iMin(20, cY);
        ImVec2 iMax(s_SidebarW - 20, cY + 44);

        ImGui::SetCursorScreenPos(iMin);
        if (ImGui::InvisibleButton(navs[i], ImVec2(iMax.x - iMin.x, 44))) { s_ActiveNav = i; g_ActiveDetails = nullptr; }
        bool isHov = ImGui::IsItemHovered();

        if (s_ActiveNav == i) sDl->AddRectFilled(iMin, iMax, colAccentP, 12.0f);
        else if (isHov) sDl->AddRectFilled(iMin, iMax, IM_COL32(255, 255, 255, 15), 12.0f);

        ImU32 tCol = (s_ActiveNav == i || isHov) ? IM_COL32_WHITE : IM_COL32(161, 161, 170, 255);

        ImGui::SetCursorScreenPos(ImVec2(iMin.x + 20 - 8.0f, cY + 22 - 8.0f));
        Lucide::IconProps navProps; navProps.size = 16.0f; navProps.color = tCol; navProps.thickness = 2.0f; navProps.hoverThickness = 2.5f; navProps.animated = true;
        Lucide::Icon(icons[i], navProps);

        sDl->AddText(ImVec2(55, cY + 12), tCol, navs[i]);
        cY += 52.0f;
    }

    ImVec2 uMin(20, screenSize.y - 80);
    ImVec2 uMax(s_SidebarW - 20, screenSize.y - 20);
    ImGui::SetCursorScreenPos(uMin);
    if (ImGui::InvisibleButton("SettingsNav", ImVec2(uMax.x - uMin.x, uMax.y - uMin.y))) { s_ActiveNav = 5; g_ActiveDetails = nullptr; }
    bool uHov = ImGui::IsItemHovered();

    sDl->AddRectFilled(uMin, uMax, uHov ? IM_COL32(255, 255, 255, 15) : IM_COL32(0, 0, 0, 0), 12.0f);
    sDl->AddCircleFilled(ImVec2(uMin.x + 24, uMin.y + 30), 16.0f, IM_COL32(80, 80, 90, 255));
    sDl->AddText(ImVec2(uMin.x + 50, uMin.y + 12), IM_COL32_WHITE, g_ActiveProfile.name.c_str());
    sDl->AddText(ImVec2(uMin.x + 50, uMin.y + 30), IM_COL32(161, 161, 170, 255), "Settings");

    ImGui::End();

    // --- MAIN CONTENT WINDOW ---
    ImGui::SetNextWindowPos(ImVec2(s_SidebarW, 0));
    ImGui::SetNextWindowSize(ImVec2(screenSize.x - s_SidebarW, screenSize.y));
    ImGui::Begin("MainContent", nullptr, ImGuiWindowFlags_NoDecoration);
    ImDrawList* mDl = ImGui::GetWindowDrawList();
    ImVec2 mP = ImGui::GetCursorScreenPos();
    HandleAppDrag(hWnd);

    // 1. Draw Fixed Background First
    mDl->AddRectFilled(mP, ImVec2(mP.x + screenSize.x - s_SidebarW, mP.y + screenSize.y), colBg);

    // 2. SCROLLABLE CHILD VIEW (Fixes Artifacting and Bounds cutoffs automatically)
    ImGui::SetCursorScreenPos(ImVec2(mP.x, mP.y + 80));
    ImGui::BeginChild("MainScroll", ImVec2(screenSize.x - s_SidebarW, screenSize.y - 80), false, ImGuiWindowFlags_NoBackground);
    ImDrawList* cDl = ImGui::GetWindowDrawList();
    ImVec2 cP = ImGui::GetCursorScreenPos();

    if (s_ActiveNav == 5) {
        cDl->AddText(ImVec2(cP.x + 40, cP.y + 40), IM_COL32_WHITE, "Settings & Preferences");
        ImVec2 setCardMin(cP.x + 40, cP.y + 80);
        ImVec2 setCardMax(cP.x + 600, cP.y + 270);
        cDl->AddRectFilled(setCardMin, setCardMax, IM_COL32(255, 255, 255, 10), 16.0f);
        cDl->AddRect(setCardMin, setCardMax, colBorder, 16.0f);
        cDl->AddText(ImVec2(setCardMin.x + 24, setCardMin.y + 24), IM_COL32_WHITE, "Local Media Setup");

        ImGui::SetCursorScreenPos(ImVec2(setCardMin.x + 24, setCardMin.y + 70));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 100));
        ImGui::PushStyleColor(ImGuiCol_Border, colBorder);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));

        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.65f, 1.0f), "Media Directory Path");
        ImGui::SetNextItemWidth(500.0f);
        ImGui::InputText("##MediaDir", s_MediaDirBuf, sizeof(s_MediaDirBuf));
        ImGui::Dummy(ImVec2(0, 15));

        ImGui::PushStyleColor(ImGuiCol_Button, colAccentB);
        if (ImGui::Button("Save & Rescan Directory", ImVec2(200, 40))) {
            g_ActiveProfile.localDir = s_MediaDirBuf;
            ScanLocalMedia();
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        ImGui::SetCursorScreenPos(ImVec2(cP.x, setCardMax.y + 40.0f));
        ImGui::Dummy(ImVec2(10, 10));
    }
    else {
        float gridY = cP.y + 40;

        // Point iteration source dynamically to core lists
        std::vector<MockMedia>* displayList = &g_LocalLibrary;
        if (s_ActiveNav == 3) displayList = &s_FavoritesSnapshot;
        else if (s_ActiveNav == 4) displayList = &g_ActiveProfile.history;

        if (displayList->empty() && (s_ActiveNav == 3 || s_ActiveNav == 4)) {
            cDl->AddText(ImVec2(cP.x + 40, gridY + 10), IM_COL32(160, 160, 170, 255), s_ActiveNav == 3 ? "No favorites yet." : "No watch history yet.");
        }

        if (s_ActiveNav == 0 && !g_LocalLibrary.empty()) {
            float heroY = gridY;
            float heroW = screenSize.x - s_SidebarW - 80;
            ImVec2 hMin(cP.x + 40, heroY);
            ImVec2 hMax(hMin.x + heroW, hMin.y + 300);

            cDl->AddRectFilled(hMin, hMax, IM_COL32(30, 30, 30, 255), 40.0f);
            cDl->AddRectFilledMultiColor(hMin, hMax, IM_COL32(10, 10, 11, 255), IM_COL32(10, 10, 11, 255), IM_COL32(10, 10, 11, 0), IM_COL32(10, 10, 11, 0));

            cDl->AddRectFilled(ImVec2(hMin.x + 40, hMax.y - 180), ImVec2(hMin.x + 150, hMax.y - 155), colAccentP, 20.0f);
            cDl->AddText(ImVec2(hMin.x + 55, hMax.y - 175), IM_COL32_WHITE, "MOST POPULAR");

            ImGui::SetWindowFontScale(2.5f);
            cDl->AddText(ImVec2(hMin.x + 40, hMax.y - 140), IM_COL32_WHITE, g_LocalLibrary[0].title.c_str());
            ImGui::SetWindowFontScale(1.0f);

            ImVec2 bMin(hMin.x + 40, hMax.y - 70);
            ImVec2 bMax(bMin.x + 180, bMin.y + 50);

            ImGui::SetCursorScreenPos(bMin);
            if (ImGui::InvisibleButton("HeroPlayBtn", ImVec2(bMax.x - bMin.x, bMax.y - bMin.y))) {
                g_ActiveDetails = &g_LocalLibrary[0];
                g_SelectedSeasonIdx = 0;
                g_SelectedEpisodeIdx = 0;
            }
            bool bHov = ImGui::IsItemHovered();

            cDl->AddRectFilled(bMin, bMax, bHov ? IM_COL32(230, 230, 230, 255) : IM_COL32_WHITE, 16.0f);

            ImGui::SetCursorScreenPos(ImVec2(bMin.x + 40 - 8.0f, bMin.y + 25 - 8.0f));
            Lucide::IconProps heroProps; heroProps.size = 16.0f; heroProps.color = IM_COL32_BLACK; heroProps.thickness = 2.0f;
            Lucide::Icon("Play", heroProps);

            cDl->AddText(ImVec2(bMin.x + 70, bMin.y + 16), IM_COL32_BLACK, "Play Now");

            gridY = hMax.y + 40;
        }

        if (s_ActiveNav != 0) {
            cDl->AddText(ImVec2(cP.x + 40, gridY), IM_COL32_WHITE, navs[s_ActiveNav]);
        }

        float cX = cP.x + 40;
        float cardW = 200.0f;
        float cardH = 280.0f;
        float currentY = gridY + 30;

        for (int i = 0; i < displayList->size(); ++i) {
            if (s_ActiveNav == 1 && (*displayList)[i].type != "Movie") continue;
            if (s_ActiveNav == 2 && (*displayList)[i].type != "Series") continue;

            if (cX + cardW > cP.x + screenSize.x - s_SidebarW - 40) {
                cX = cP.x + 40;
                currentY += cardH + 24.0f;
            }

            ImVec2 cMin(cX, currentY);
            ImVec2 cMax(cMin.x + cardW, cMin.y + cardH);

            ImGui::SetCursorScreenPos(cMin);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("MockCard", ImVec2(cardW, cardH))) {
                g_ActiveDetails = &(*displayList)[i];
                g_SelectedSeasonIdx = 0;
                g_SelectedEpisodeIdx = 0;

                // Seamless History Injection (pre-loads Dropdowns instantly)
                auto histIt = std::find_if(g_ActiveProfile.history.begin(), g_ActiveProfile.history.end(),
                    [&](const MockMedia& m) { return m.title == g_ActiveDetails->title; });

                if (histIt != g_ActiveProfile.history.end()) {
                    g_ActiveDetails->historyStats = histIt->historyStats;

                    // Preselect season based on state Snapshot
                    for (size_t s = 0; s < g_ActiveDetails->seasons.size(); s++) {
                        if (g_ActiveDetails->seasons[s].name == g_ActiveDetails->historyStats.season) {
                            g_SelectedSeasonIdx = (int)s;
                            // Preselect episode snapshot
                            for (size_t e = 0; e < g_ActiveDetails->seasons[s].eps.size(); e++) {
                                if (g_ActiveDetails->seasons[s].eps[e].url == g_ActiveDetails->historyStats.url) {
                                    g_SelectedEpisodeIdx = (int)e;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
                else {
                    g_ActiveDetails->historyStats = HistoryStats();
                }
            }
            bool cHov = ImGui::IsItemHovered();
            ImGui::PopID();

            float yOff = cHov ? -8.0f : 0.0f;
            cMin.y += yOff; cMax.y += yOff;

            cDl->AddRectFilled(cMin, cMax, colCardBg, 20.0f);
            cDl->AddRect(cMin, cMax, cHov ? colAccentP : colBorder, 20.0f, 0, 1.0f);

            // Mock Poster Visual Placeholder
            cDl->AddRectFilled(cMin, ImVec2(cMax.x, cMax.y - 60), IM_COL32(30, 32, 38, 255), 20.0f, ImDrawFlags_RoundCornersTop);

            ImGui::SetCursorScreenPos(ImVec2(cMin.x + cardW / 2.0f - 16.0f, cMin.y + (cardH - 60.0f) / 2.0f - 16.0f));
            Lucide::IconProps phProps; phProps.size = 32.0f; phProps.color = IM_COL32(255, 255, 255, 20); phProps.thickness = 2.0f;
            Lucide::Icon("Film", phProps);

            cDl->AddText(ImVec2(cMin.x + 15, cMax.y - 45), IM_COL32_WHITE, (*displayList)[i].title.c_str());
            cDl->AddText(ImVec2(cMin.x + 15, cMax.y - 25), IM_COL32(160, 160, 170, 255), (*displayList)[i].type.c_str());

            // History Playback Visual Progress Bar 
            if (s_ActiveNav == 4 && (*displayList)[i].historyStats.progress > 0.0f) {
                float prog = (*displayList)[i].historyStats.progress;
                cDl->AddRectFilled(ImVec2(cMin.x, cMax.y - 6), ImVec2(cMax.x, cMax.y), IM_COL32(255, 255, 255, 20), 20.0f, ImDrawFlags_RoundCornersBottom);
                cDl->AddRectFilled(ImVec2(cMin.x, cMax.y - 6), ImVec2(cMin.x + cardW * prog, cMax.y), colAccentP, 20.0f, ImDrawFlags_RoundCornersBottomLeft | (prog > 0.98f ? ImDrawFlags_RoundCornersBottomRight : 0));
            }

            cX += cardW + 24.0f;
        }

        // Force scroll-bar calculation bounding
        ImGui::SetCursorScreenPos(ImVec2(cP.x, currentY + cardH + 40.0f));
        ImGui::Dummy(ImVec2(10, 10));
    }
    ImGui::EndChild(); // MainScroll 

    // 3. Draw Fixed Header Layer ABOVE the scrolled content
    mDl->AddRectFilled(mP, ImVec2(mP.x + screenSize.x - s_SidebarW, mP.y + 80), IM_COL32(11, 14, 20, 245));
    mDl->AddLine(ImVec2(mP.x, mP.y + 80), ImVec2(screenSize.x, mP.y + 80), colBorder);

    // --- GLOBAL APP CLOSE BUTTON (MAIN VIEW) ---
    ImVec2 mcClosePos(screenSize.x - 35, mP.y + 30);
    ImGui::SetCursorScreenPos(ImVec2(mcClosePos.x - 5, mcClosePos.y - 5));
    if (ImGui::InvisibleButton("AppCloseMain", ImVec2(30, 30))) {
        PostMessage(hWnd, WM_CLOSE, 0, 0);
    }
    bool mcCloseHov = ImGui::IsItemHovered();

    Lucide::IconProps mcCloseProps; mcCloseProps.size = 20.0f; mcCloseProps.color = mcCloseHov ? IM_COL32(255, 100, 100, 255) : IM_COL32_WHITE; mcCloseProps.thickness = 2.0f; mcCloseProps.hoverThickness = 2.5f; mcCloseProps.animated = true;
    ImGui::SetCursorScreenPos(mcClosePos);
    Lucide::Icon("X", mcCloseProps);

    // --- APP MAXIMIZE BUTTON ---
    ImVec2 mcMaxPos(screenSize.x - 75, mP.y + 30);
    ImGui::SetCursorScreenPos(ImVec2(mcMaxPos.x - 5, mcMaxPos.y - 5));
    if (ImGui::InvisibleButton("AppMaxMain", ImVec2(30, 30))) {
        SetTimer(hWnd, 1002, 10, MaximizeTimerProc);
    }
    bool mcMaxHov = ImGui::IsItemHovered();

    Lucide::IconProps mcMaxProps; mcMaxProps.size = 20.0f; mcMaxProps.color = mcMaxHov ? colAccentP : IM_COL32_WHITE; mcMaxProps.thickness = 2.0f; mcMaxProps.hoverThickness = 2.5f; mcMaxProps.animated = true;
    ImGui::SetCursorScreenPos(mcMaxPos);
    Lucide::Icon("Maximize", mcMaxProps);


    ImVec2 sMin = ImVec2(mP.x + 40, mP.y + 20);
    ImVec2 sMax = ImVec2(sMin.x + 400, sMin.y + 40);
    mDl->AddRectFilled(sMin, sMax, IM_COL32(255, 255, 255, 15), 20.0f);

    ImGui::SetCursorScreenPos(ImVec2(sMin.x + 20 - 8.0f, sMin.y + 20 - 8.0f));
    Lucide::IconProps searchProps; searchProps.size = 16.0f; searchProps.color = IM_COL32(161, 161, 170, 255); searchProps.thickness = 2.0f;
    Lucide::Icon("Search", searchProps);

    mDl->AddText(ImVec2(sMin.x + 40, sMin.y + 12), IM_COL32(161, 161, 170, 255), "Search globally across all servers...");


    ImGui::End();

    // ==========================================
    // DETAILS MODAL OVERLAY
    // ==========================================
    if (g_ActiveDetails) {
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(screenSize);
        ImGui::Begin("DetailsOverlay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground);
        ImDrawList* dDl = ImGui::GetWindowDrawList();

        dDl->AddRectFilled(ImVec2(0, 0), screenSize, IM_COL32(0, 0, 0, 200));

        ImVec2 modalSize(800, 500);
        ImVec2 modalPos((screenSize.x - modalSize.x) * 0.5f, (screenSize.y - modalSize.y) * 0.5f);
        dDl->AddRectFilled(modalPos, ImVec2(modalPos.x + modalSize.x, modalPos.y + modalSize.y), IM_COL32(20, 24, 34, 255), 16.0f);
        dDl->AddRect(modalPos, ImVec2(modalPos.x + modalSize.x, modalPos.y + modalSize.y), colBorder, 16.0f);

        // --- FAVORITES TOGGLE ---
        bool isFavorite = false;
        auto favIt = std::find_if(g_ActiveProfile.favorites.begin(), g_ActiveProfile.favorites.end(), [&](const MockMedia& m) { return m.title == g_ActiveDetails->title; });
        if (favIt != g_ActiveProfile.favorites.end()) isFavorite = true;

        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + modalSize.x - 80, modalPos.y + 15));
        if (ImGui::InvisibleButton("FavBtn", ImVec2(30, 30))) {
            if (isFavorite) g_ActiveProfile.favorites.erase(favIt);
            else g_ActiveProfile.favorites.push_back(*g_ActiveDetails);
        }
        bool favHov = ImGui::IsItemHovered();

        Lucide::IconProps favProps; favProps.size = 20.0f; favProps.color = isFavorite ? IM_COL32(239, 68, 68, 255) : (favHov ? IM_COL32_WHITE : IM_COL32(160, 160, 170, 255)); favProps.thickness = 2.0f;
        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + modalSize.x - 75, modalPos.y + 20));
        Lucide::Icon("Heart", favProps);

        // --- MODAL / CARD CLOSE BUTTON ---
        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + modalSize.x - 40, modalPos.y + 15));
        if (ImGui::InvisibleButton("CloseModal", ImVec2(30, 30))) {
            g_ActiveDetails = nullptr;
        }
        bool modalCloseHov = ImGui::IsItemHovered();

        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + modalSize.x - 35, modalPos.y + 20));
        Lucide::IconProps xProps; xProps.size = 20.0f; xProps.color = modalCloseHov ? IM_COL32(255, 100, 100, 255) : IM_COL32_WHITE; xProps.thickness = 2.0f;
        Lucide::Icon("X", xProps);

        if (!g_ActiveDetails) {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return; // Safety exit to avoid dangling dereferencing
        }

        ImGui::SetWindowFontScale(2.0f);
        dDl->AddText(ImVec2(modalPos.x + 40, modalPos.y + 40), IM_COL32_WHITE, g_ActiveDetails->title.c_str());
        ImGui::SetWindowFontScale(1.0f);
        dDl->AddText(ImVec2(modalPos.x + 40, modalPos.y + 80), IM_COL32(160, 160, 170, 255), g_ActiveDetails->type.c_str());
        dDl->AddText(ImVec2(modalPos.x + 100, modalPos.y + 80), IM_COL32(160, 160, 170, 255), g_ActiveDetails->source.c_str());

        // --- PLAY LOGIC (Respecting History Start Points) ---
        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + 40, modalPos.y + 120));
        if (ImGui::InvisibleButton("ModalPlayBtn", ImVec2(150, 45))) {
            g_Settings.videoTitle = g_ActiveDetails->title;
            g_Settings.progress = g_ActiveDetails->historyStats.progress; // Start right back at exact position

            if (g_ActiveDetails->type == "Series" && !g_ActiveDetails->seasons.empty()) {
                if (g_SelectedSeasonIdx < g_ActiveDetails->seasons.size()) {
                    g_Settings.activeSeason = g_ActiveDetails->seasons[g_SelectedSeasonIdx].name;
                    if (g_SelectedEpisodeIdx < g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps.size()) {
                        g_Settings.videoTitle += " - " + g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps[g_SelectedEpisodeIdx].title;
                        g_Settings.currentUrl = g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps[g_SelectedEpisodeIdx].url;
                    }
                }
            }
            else {
                if (!g_ActiveDetails->seasons.empty() && !g_ActiveDetails->seasons[0].eps.empty()) {
                    g_Settings.currentUrl = g_ActiveDetails->seasons[0].eps[0].url;
                }
            }
            g_Settings.sourceName = g_ActiveDetails->source;
            g_Settings.requestPlay = true;
            g_AppMode = MODE_PLAYER;
        }

        if (!g_ActiveDetails) {
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            return;
        }

        bool playHov = ImGui::IsItemHovered();
        dDl->AddRectFilled(ImVec2(modalPos.x + 40, modalPos.y + 120), ImVec2(modalPos.x + 190, modalPos.y + 165), playHov ? IM_COL32(150, 100, 250, 255) : colAccentP, 8.0f);

        ImGui::SetCursorScreenPos(ImVec2(modalPos.x + 65 - 8.0f, modalPos.y + 142 - 8.0f));
        Lucide::IconProps mPlayProps; mPlayProps.size = 16.0f; mPlayProps.color = IM_COL32_WHITE; mPlayProps.thickness = 2.0f;
        Lucide::Icon("Play", mPlayProps);

        bool hasProgress = g_ActiveDetails->historyStats.progress > 0.01f;
        dDl->AddText(ImVec2(modalPos.x + 90, modalPos.y + 135), IM_COL32_WHITE, hasProgress ? "Resume" : "Play Now");

        // --- SEASON & EPISODE SELECTION ---
        if (g_ActiveDetails->type == "Series" && !g_ActiveDetails->seasons.empty()) {

            ImGui::SetCursorScreenPos(ImVec2(modalPos.x + 40, modalPos.y + 190));
            ImGui::PushItemWidth(200.0f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 20));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(20, 24, 34, 255));
            if (ImGui::BeginCombo("##SeasonCombo", g_ActiveDetails->seasons[g_SelectedSeasonIdx].name.c_str())) {
                for (int s = 0; s < g_ActiveDetails->seasons.size(); s++) {
                    bool isSelected = (g_SelectedSeasonIdx == s);
                    if (ImGui::Selectable(g_ActiveDetails->seasons[s].name.c_str(), isSelected)) {
                        g_SelectedSeasonIdx = s;
                        g_SelectedEpisodeIdx = 0;
                    }
                    if (isSelected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopStyleColor(2);
            ImGui::PopItemWidth();

            dDl->AddText(ImVec2(modalPos.x + 40, modalPos.y + 240), IM_COL32_WHITE, "Episodes");
            float epY = modalPos.y + 270;
            float epX = modalPos.x + 40;

            for (size_t i = 0; i < g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps.size(); ++i) {
                ImGui::SetCursorScreenPos(ImVec2(epX, epY));

                if (ImGui::InvisibleButton((std::string("EpBtn") + std::to_string(i)).c_str(), ImVec2(160, 45))) {
                    g_SelectedEpisodeIdx = (int)i;
                }
                bool epHov = ImGui::IsItemHovered();
                bool isEpSelected = (g_SelectedEpisodeIdx == i);

                dDl->AddRectFilled(ImVec2(epX, epY), ImVec2(epX + 160, epY + 45),
                    isEpSelected ? colAccentP : (epHov ? IM_COL32(255, 255, 255, 20) : IM_COL32(255, 255, 255, 10)), 8.0f);

                dDl->AddText(ImVec2(epX + 15, epY + 15), IM_COL32_WHITE, g_ActiveDetails->seasons[g_SelectedSeasonIdx].eps[i].title.c_str());

                epX += 175;
                if (epX > modalPos.x + modalSize.x - 180) { epX = modalPos.x + 40; epY += 55; }
            }
        }
        ImGui::End();
    }

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}