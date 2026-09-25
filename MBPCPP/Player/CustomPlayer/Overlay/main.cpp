#define DYNHTML_IMPLEMENTATION // MANDATORY: Compiles the single-header DynHTML engine here!
#include "main.hpp"
#include "videoengine.hpp"
#include "launch_args.hpp"
#define MOVIEBOX_BRIDGE_IMPLEMENTATION
#include "moviebox_bridge.hpp"
#include <dwmapi.h>
#include <utility>

#pragma comment(lib, "ole32.lib")

// DirectX Globals
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

PlayerSettings g_Settings;
AppMode g_AppMode = MODE_PROFILES;
UserProfile g_ActiveProfile;
std::vector<UserProfile> g_Profiles;

bool g_IsPIPMode = false;
RECT g_PrePIPWindowRect = { 0, 0, 0, 0 };

// Windows 11 can paint a thin accent-colored resize border around custom
// client-area windows. Suppress only that border color; preserve the original
// full DWM frame extension so Windows keeps the window rounding/shape exactly
// as before. No window style bits are changed here.
static constexpr DWORD kDWMWA_BORDER_COLOR = 34;
static constexpr COLORREF kDWMWA_COLOR_NONE = 0xFFFFFFFEu;
// Windows 11 DWM attribute/value. Keep numeric constants so this source also
// builds with Windows SDKs whose headers predate DWM_WINDOW_CORNER_PREFERENCE.
static constexpr DWORD kDWMWA_WINDOW_CORNER_PREFERENCE = 33;
static constexpr int kDWMWCP_ROUND = 2;

static void ApplyPlayerDwmChrome(HWND hwnd) {
    const COLORREF borderColor = kDWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(kDWMWA_BORDER_COLOR), &borderColor, sizeof(borderColor));

    const MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
}

// A maximized WS_THICKFRAME window can have an outer HWND rectangle that
// extends several resize-border pixels past the monitor even though the client
// area looks correct. On a multi-monitor desktop those off-monitor DWM pixels
// are visible on the neighboring display. Keep the original window styles and
// DWM rounding, but clip only the maximized presentation to this monitor's
// exact physical bounds. The clip is removed immediately on restore.
static void ApplyMaximizedMonitorClip(HWND hwnd) {
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect)) return;

    const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return;

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return;

    const LONG clipLeft = std::max(0L, monitorInfo.rcMonitor.left - windowRect.left);
    const LONG clipTop = std::max(0L, monitorInfo.rcMonitor.top - windowRect.top);
    const LONG clipRight = std::min(windowRect.right - windowRect.left, monitorInfo.rcMonitor.right - windowRect.left);
    const LONG clipBottom = std::min(windowRect.bottom - windowRect.top, monitorInfo.rcMonitor.bottom - windowRect.top);

    if (clipRight <= clipLeft || clipBottom <= clipTop) return;

    HRGN region = CreateRectRgn(
        static_cast<int>(clipLeft), static_cast<int>(clipTop),
        static_cast<int>(clipRight), static_cast<int>(clipBottom));
    if (!region) return;

    // After a successful SetWindowRgn, Windows owns the HRGN.
    if (!SetWindowRgn(hwnd, region, FALSE)) DeleteObject(region);
}

static void ClearMaximizedMonitorClip(HWND hwnd) {
    // Removing a custom region is not always enough to make Windows 11 DWM
    // resume automatic rounded corners. Re-request the rounded-corner policy
    // and force a non-client frame rebuild after the region is gone. This is
    // restore-only; the fullscreen monitor clip itself remains unchanged.
    SetWindowRgn(hwnd, nullptr, FALSE);

    const int cornerPreference = kDWMWCP_ROUND;
    DwmSetWindowAttribute(
        hwnd,
        static_cast<DWMWINDOWATTRIBUTE>(kDWMWA_WINDOW_CORNER_PREFERENCE),
        &cornerPreference,
        sizeof(cornerPreference));

    SetWindowPos(
        hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}


static std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) return {};

    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};

    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), bytes, nullptr, nullptr) != bytes) {
        return {};
    }
    return result;
}

static LaunchOptions ApplyLaunchArguments(LPCWSTR lpCmdLine) {
    LaunchOptions options;
    if (lpCmdLine && *lpCmdLine) options = ParseLaunchOptions(lpCmdLine);

    // VPQ is independent from --uri so it also works when the application
    // starts normally and a video is opened later from the dashboard.
    if (options.hasVPQ) {
        g_Settings.videoPlayerQuits = options.videoPlayerQuits;
    }

    if (options.hasUserAgent) {
        g_Settings.userAgent = WideToUtf8(options.userAgent);
    }

    // MovieBox bridge mode owns the playback session. Enter the player
    // immediately and never expose Profiles/Dashboard while the bridge is active.
    if (options.hasBridge) {
        g_Settings.bridgeMode = true;
        g_Settings.videoPlayerQuits = true;
        g_Settings.isPlaying = true;
        g_AppMode = MODE_PLAYER;
    }

    if (options.hasUri) {
        std::string utf8Uri = WideToUtf8(options.uri);
        if (!utf8Uri.empty()) {
            g_Settings.currentUrl = std::move(utf8Uri);
            g_Settings.progress = 0.0f;
            g_Settings.isPlaying = true;
            g_Settings.requestPlay = true;
            g_AppMode = MODE_PLAYER;
        }
    }

    return options;
}

void TogglePIP(HWND hwnd) {
    g_IsPIPMode = !g_IsPIPMode;
    if (g_IsPIPMode) {
        GetWindowRect(hwnd, &g_PrePIPWindowRect);
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int pipW = 480;
        int pipH = 270;
        SetWindowPos(hwnd, HWND_TOPMOST, screenW - pipW - 20, screenH - pipH - 40, pipW, pipH, SWP_SHOWWINDOW);
    }
    else {
        SetWindowPos(hwnd, HWND_NOTOPMOST, g_PrePIPWindowRect.left, g_PrePIPWindowRect.top,
            g_PrePIPWindowRect.right - g_PrePIPWindowRect.left,
            g_PrePIPWindowRect.bottom - g_PrePIPWindowRect.top, SWP_SHOWWINDOW);
    }
}

void RenderFrame(HWND hwnd) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Effect callbacks store per-frame constant data by index. Reset that
    // storage only after the previous frame has finished rendering.
    ClearShaderState();

    // --- APP ROUTING LOGIC ---
    // MovieBox bridge sessions are renderer-only. Even if legacy standalone
    // state mutates g_AppMode, never expose Profiles/Dashboard to MovieBox.
    if (g_Settings.bridgeMode) {
        RenderVideoEngine(hwnd);
    }
    else if (g_AppMode == MODE_PROFILES || g_AppMode == MODE_DASHBOARD) {
        RenderApp(hwnd);
    }
    else if (g_AppMode == MODE_PLAYER) {
        RenderVideoEngine(hwnd);
    }

    ImGui::Render();

    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow)
{
    // Media Foundation and WIC are COM-based. Initialize COM explicitly instead
    // of relying on some unrelated component to have done it first.
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldCoUninitialize = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        return 1;
    }

    const LaunchOptions launchOptions = ApplyLaunchArguments(lpCmdLine);
    if (launchOptions.hasBridge) MovieBoxBridgeStart(launchOptions.bridgeName);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"ImGui Player Class", nullptr };
    ::RegisterClassExW(&wc);

    HWND hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_APPWINDOW,
        wc.lpszClassName, L"TvAnime App",
        WS_POPUP | WS_THICKFRAME,
        100, 100, 1280, 720,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    // Preserve the original transparent DWM composition/rounding path. The only
    // change is suppressing Windows 11's thin colored resize-border before the
    // HWND is ever shown, which prevents the bottom/right white strip without
    // changing WS_THICKFRAME or the window shape.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    ApplyPlayerDwmChrome(hwnd);

    // Force the original non-client/DWM layout to settle while the HWND is
    // still hidden. This is intentionally a FRAMECHANGED recalculation only
    // (no style bits are modified). Creating the swapchain after this avoids
    // a one-frame client/backbuffer size mismatch on the bottom/right edge.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (!CreateDeviceD3D(hwnd)) {
        MovieBoxBridgeStop();
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        if (shouldCoUninitialize) CoUninitialize();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig font_config;
    font_config.PixelSnapH = true;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF((void*)poppins_font_data, poppins_font_size, 18.0f, &font_config);

    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.FontDataOwnedByAtlas = false;
    icons_config.GlyphMinAdvanceX = 13.0f;
    icons_config.GlyphOffset = ImVec2(0, 0);
    io.Fonts->AddFontFromMemoryTTF((void*)fa_icons_data, fa_icons_size, 16.0f, &icons_config);

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Compile the runtime D3D11 effects now that the device exists. This also
    // creates the procedural startup/loading shader used by the player.
    InitShaders();

    // Prime the swapchain while the native window is still hidden.
    // This prevents Windows/DWM from exposing an unpainted white client area
    // before our first CustomPlayer frame exists.
    RenderFrame(hwnd);

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    // Present once more after the window becomes visible so the loading screen
    // is immediately visible even on drivers that discard a hidden-window present.
    RenderFrame(hwnd);

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        RenderFrame(hwnd);
    }

    // MovieBox owns final progress/history persistence. Send the close sample
    // before tearing down the media engines so the last position is still valid.
    if (g_Settings.bridgeMode) {
        SaveCurrentEpisodeSettings();
        MovieBoxBridgeCloseSession(1200);
        MovieBoxBridgeStop();
    }

    // Release Media Foundation / VLC objects while D3D and COM are still alive.
    StopCPlayer();
    StopVLCPlayer();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (shouldCoUninitialize) CoUninitialize();
    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRenderTarget();

    return true;
}

void CleanupDeviceD3D() { CleanupRenderTarget(); if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; } if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; } if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; } }
void CreateRenderTarget() { ID3D11Texture2D* pBackBuffer; g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)); g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView); pBackBuffer->Release(); }
void CleanupRenderTarget() { if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; } }

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyPlayerDwmChrome(hWnd);
        return 0;
    case WM_NCCALCSIZE:
        if (wParam == TRUE) return 0;
        break;
    case WM_NCPAINT:
        return 0;
    case WM_NCACTIVATE:
        return DefWindowProc(hWnd, msg, wParam, -1);
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        if (!g_IsPIPMode) {
            mmi->ptMinTrackSize.x = 800;
            mmi->ptMinTrackSize.y = 600;
        }
        else {
            mmi->ptMinTrackSize.x = 400;
            mmi->ptMinTrackSize.y = 225;
        }
        return 0;
    }
    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc; GetWindowRect(hWnd, &rc);
        int borderThickness = 6;
        if (pt.x < rc.left + borderThickness && pt.y < rc.top + borderThickness) return HTTOPLEFT;
        if (pt.x > rc.right - borderThickness && pt.y < rc.top + borderThickness) return HTTOPRIGHT;
        if (pt.x < rc.left + borderThickness && pt.y > borderThickness) return HTBOTTOMLEFT;
        if (pt.x > rc.right - borderThickness && pt.y > rc.bottom - borderThickness) return HTBOTTOMRIGHT;
        if (pt.x < rc.left + borderThickness) return HTLEFT;
        if (pt.x > rc.right - borderThickness) return HTRIGHT;
        if (pt.y < rc.top + borderThickness) return HTTOP;
        if (pt.y > rc.bottom - borderThickness) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_SIZE:
        // SetWindowRgn() forces a rectangular window region while maximized so
        // hidden WS_THICKFRAME pixels cannot spill onto adjacent monitors. On
        // restore that region must be removed BEFORE DWM rebuilds the normal
        // chrome; otherwise DWM observes the rectangular region and the
        // restored window loses its rounded corners.
        if (wParam == SIZE_RESTORED) {
            ClearMaximizedMonitorClip(hWnd);
        }

        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
            // Maximizing/fullscreen-size transitions can cause DWM to refresh
            // its non-client visuals. Reassert only the border-color override;
            // the original full-frame rounding path remains unchanged.
            ApplyPlayerDwmChrome(hWnd);
        }

        // Keep the multi-monitor spill fix: only maximized presentation gets
        // clipped, and it is applied AFTER the DWM refresh.
        if (wParam == SIZE_MAXIMIZED) {
            ApplyMaximizedMonitorClip(hWnd);
        }

        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            RenderFrame(hWnd);
        }
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProc(hWnd, msg, wParam, lParam);
}

void HandleAppDrag(HWND hWnd) {
    // Prevent dragging if the window is maximized
    if (IsZoomed(hWnd)) return;

    static bool isDragging = false;
    static POINT dragStart;
    static RECT winStart;

    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        isDragging = true;
        GetCursorPos(&dragStart);
        GetWindowRect(hWnd, &winStart);
    }
    if (isDragging) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            POINT cur; GetCursorPos(&cur);
            SetWindowPos(hWnd, nullptr, winStart.left + (cur.x - dragStart.x), winStart.top + (cur.y - dragStart.y), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        else {
            isDragging = false;
        }
    }
}