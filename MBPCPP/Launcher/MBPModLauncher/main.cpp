#include "main.hpp"

#include "App.hpp"
#include "DX12Renderer.hpp"
#include "ShaderEffects.hpp"
#include "Settings.hpp"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "poppins.h"

#include <dwmapi.h>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace
{
    DX12Renderer* g_renderer = nullptr;

    constexpr DWORD kDwmWindowCornerPreference = 33;
    constexpr DWORD kDwmBorderColor = 34;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    constexpr int kDwmRound = 2;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFEu;

    void ApplyWindowChrome(HWND hwnd)
    {
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(kDwmUseImmersiveDarkMode), &dark, sizeof(dark));

        const int rounded = kDwmRound;
        DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(kDwmWindowCornerPreference), &rounded, sizeof(rounded));

        const COLORREF border = kDwmColorNone;
        DwmSetWindowAttribute(hwnd, static_cast<DWMWINDOWATTRIBUTE>(kDwmBorderColor), &border, sizeof(border));

        const MARGINS margins{ -1 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }

    RECT CenteredWindowRect(int width, int height)
    {
        POINT origin{ 0, 0 };
        HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(monitor, &info);

        const int workW = info.rcWork.right - info.rcWork.left;
        const int workH = info.rcWork.bottom - info.rcWork.top;
        const int x = info.rcWork.left + std::max(0, (workW - width) / 2);
        const int y = info.rcWork.top + std::max(0, (workH - height) / 2);
        return RECT{ x, y, x + width, y + height };
    }
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    launcher::Settings::CloseExistingMovieBoxProcesses();
    ImGui_ImplWin32_EnableDpiAwareness();

    constexpr wchar_t kClassName[] = L"MBPModLauncherDX12";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc))
        return 1;

    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    const int width = static_cast<int>(940.0f * dpiScale);
    const int height = static_cast<int>(620.0f * dpiScale);
    const RECT wr = CenteredWindowRect(width, height);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kClassName,
        L"MovieBox Player Mod",
        WS_POPUP | WS_MINIMIZEBOX,
        wr.left,
        wr.top,
        wr.right - wr.left,
        wr.bottom - wr.top,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
    if (!hwnd)
    {
        UnregisterClassW(kClassName, hInstance);
        return 1;
    }
    ApplyWindowChrome(hwnd);

    DX12Renderer renderer;
    g_renderer = &renderer;
    if (!renderer.Initialize(hwnd))
    {
        MessageBoxW(hwnd, L"Direct3D 12 initialization failed.", L"MovieBox Player Mod", MB_ICONERROR | MB_OK);
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, hInstance);
        g_renderer = nullptr;
        return 2;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImFontConfig fontConfig{};
    fontConfig.FontDataOwnedByAtlas = false;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 2;
    ImFont* poppins = io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned char*>(poppins_font_data),
        static_cast<int>(poppins_font_size),
        16.0f * dpiScale,
        &fontConfig);
    if (poppins)
        io.FontDefault = poppins;

    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 12.0f;
    style.PopupRounding = 14.0f;
    style.ScrollbarRounding = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Text] = ImVec4(0.96f, 0.97f, 1.0f, 1.0f);

    if (!renderer.InitializeImGui(hwnd))
    {
        MessageBoxW(hwnd, L"Dear ImGui DX12 backend initialization failed.", L"MovieBox Player Mod", MB_ICONERROR | MB_OK);
        ImGui::DestroyContext();
        renderer.Shutdown();
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, hInstance);
        g_renderer = nullptr;
        return 3;
    }

    ShaderEffects effects;
    if (!effects.Initialize(renderer.Device(), renderer.BackBufferFormat(), DX12Renderer::FramesInFlight))
    {
        MessageBoxW(hwnd, L"The embedded DX12 shader pipeline could not be created.", L"MovieBox Player Mod", MB_ICONERROR | MB_OK);
        renderer.Shutdown();
        ImGui::DestroyContext();
        DestroyWindow(hwnd);
        UnregisterClassW(kClassName, hInstance);
        g_renderer = nullptr;
        return 4;
    }

    App app;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    const auto started = std::chrono::steady_clock::now();
    bool done = false;
    while (!done)
    {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        if (IsIconic(hwnd))
        {
            Sleep(10);
            continue;
        }

        renderer.BeginImGuiFrame();
        app.Render(hwnd);

        const float timeSeconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        if (!renderer.RenderFrame(effects, timeSeconds, app.IsCppSelected(), app.BackgroundOpacity()))
        {
            done = true;
            break;
        }
    }

    renderer.WaitForPendingOperations();
    effects.Shutdown();
    renderer.Shutdown();
    ImGui::DestroyContext();
    g_renderer = nullptr;

    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(kClassName, hInstance);
    return 0;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return TRUE;

    switch (msg)
    {
    case WM_SIZE:
        if (g_renderer && wParam != SIZE_MINIMIZED)
            g_renderer->Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
