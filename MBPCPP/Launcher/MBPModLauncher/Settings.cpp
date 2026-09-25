#include "Settings.hpp"
#include "CockWrapper.HPP"
#include "LauncherDefaultsPayload.HPP"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <functional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace launcher
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr wchar_t kInstalledMovieBox[] = L"C:\\Program Files\\MovieBoxPro\\MovieBoxPro\\MovieBoxPro.exe";

        fs::path HookRelative() { return fs::path(L"build") / L"Hook" / L"MovieBoxPlayerMod.Hook.dll"; }
        fs::path CppPlayerRelative() { return fs::path(L"build") / L"PlayerCPP"; }
        fs::path CSharpPlayerRelative() { return fs::path(L"build") / L"PlayerC#"; }

        bool Exists(const fs::path& path) noexcept
        {
            std::error_code ec;
            return fs::exists(path, ec);
        }

        bool IsRegularExe(const fs::path& path) noexcept
        {
            if (path.empty())
                return false;
            std::error_code ec;
            if (!fs::is_regular_file(path, ec) || ec)
                return false;
            std::wstring ext;
            try { ext = path.extension().wstring(); }
            catch (...) { return false; }
            std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            return ext == L".exe";
        }

        std::wstring MissingMessage(std::wstring_view label, const fs::path& path)
        {
            std::wstring out(label);
            out += L" not found:\n";
            out += path.wstring();
            return out;
        }

        std::wstring WidenAscii(std::string_view text)
        {
            return std::wstring(text.begin(), text.end());
        }

        bool PathToUtf8(const fs::path& path, std::string& out)
        {
#ifdef _WIN32
            const std::wstring wide = path.wstring();
            if (wide.empty()) { out.clear(); return true; }
            const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (count <= 0) return false;
            out.assign(static_cast<std::size_t>(count), '\0');
            return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), out.data(), count, nullptr, nullptr) == count;
#else
            const auto u8 = path.u8string();
            out.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
            return true;
#endif
        }

        bool Utf8ToPath(std::string_view value, fs::path& out)
        {
#ifdef _WIN32
            if (value.empty()) { out.clear(); return true; }
            const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
            if (count <= 0) return false;
            std::wstring wide(static_cast<std::size_t>(count), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), count) != count)
                return false;
            out = fs::path(wide);
            return true;
#else
            std::u8string u8;
            u8.resize(value.size());
            std::transform(value.begin(), value.end(), u8.begin(), [](char c) { return static_cast<char8_t>(static_cast<unsigned char>(c)); });
            out = fs::path(u8);
            return true;
#endif
        }
    }

    Settings::Settings()
        : Settings(SettingsOptions{})
    {
    }

    Settings::Settings(SettingsOptions options)
        : options_(std::move(options))
    {
        Refresh();
    }

    fs::path Settings::ExecutableDirectory()
    {
#ifdef _WIN32
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size())
            return fs::current_path();
        return fs::path(std::wstring_view(buffer.data(), length)).parent_path();
#else
        return fs::current_path();
#endif
    }

    fs::path Settings::EffectiveExecutableDirectory() const
    {
        return options_.executableDirectoryOverride.empty() ? ExecutableDirectory() : options_.executableDirectoryOverride;
    }

    fs::path Settings::EffectiveLocalAppData() const
    {
        if (!options_.localAppDataOverride.empty())
            return options_.localAppDataOverride;
#ifdef _WIN32
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size())
            return fs::path(std::wstring_view(buffer.data(), length));

        std::array<wchar_t, MAX_PATH> folder{};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, folder.data())))
            return fs::path(folder.data());
        return {};
#else
        if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
            return fs::path(local);
        return fs::temp_directory_path();
#endif
    }

    fs::path Settings::EffectiveStandardMovieBox() const
    {
        return options_.standardMovieBoxExecutableOverride.empty() ? fs::path(kInstalledMovieBox) : options_.standardMovieBoxExecutableOverride;
    }

    fs::path Settings::FindLauncherRoot(const fs::path& exeDirectory)
    {
        fs::path probe = exeDirectory;
        for (int depth = 0; depth < 6; ++depth)
        {
            if (Exists(probe / HookRelative()))
                return probe;
            if (Exists(probe / L"MBPModLauncher.slnx") || Exists(probe / L"MBPModLauncher" / L"MBPModLauncher.vcxproj"))
                return probe;

            const fs::path parent = probe.parent_path();
            if (parent.empty() || parent == probe)
                break;
            probe = parent;
        }
        return exeDirectory;
    }

    fs::path Settings::SettingsFilePath() const
    {
        const fs::path local = EffectiveLocalAppData();
        if (local.empty())
            return {};
        return local / L"MovieBoxPlayerMod" / L"Launcher" / L"Settings.cock";
    }

    void Settings::Refresh()
    {
        paths_ = {};
        movieBoxPathState_ = {};
        playerDefaults_ = PlayerDefaults{};
        legacyMigrationPending_ = false;

        const fs::path exeDirectory = EffectiveExecutableDirectory();
        paths_.launcherRoot = FindLauncherRoot(exeDirectory);
        paths_.hookDll = paths_.launcherRoot / HookRelative();
        paths_.cppPlayerDirectory = paths_.launcherRoot / CppPlayerRelative();
        paths_.csharpPlayerDirectory = paths_.launcherRoot / CSharpPlayerRelative();

        const fs::path settingsPath = SettingsFilePath();
        if (!settingsPath.empty())
        {
            const cock::LoadResult loaded = cock::LoadFile(settingsPath);
            if (loaded.ok)
            {
                movieBoxPathState_.savedSettingsPresent = true;
                LauncherSettingsData decoded{};
                std::string utf8Path;
                if (loaded.document.value.size() >= 4 && loaded.document.value.compare(0, 4, "LDEF") == 0)
                {
                    std::string decodeError;
                    if (!DecodeLdef(loaded.document.value, decoded, &decodeError))
                    {
                        movieBoxPathState_.persistenceWarning = L"Settings.cock contains invalid launcher defaults: " + WidenAscii(decodeError);
                    }
                    else
                    {
                        utf8Path = decoded.movieBoxPathUtf8;
                        playerDefaults_ = decoded.playerDefaults;
                    }
                }
                else
                {
                    utf8Path = loaded.document.value;
                    legacyMigrationPending_ = true;
                }

                if (!utf8Path.empty())
                {
                    fs::path saved;
                    if (!Utf8ToPath(utf8Path, saved))
                    {
                        movieBoxPathState_.persistenceWarning = L"Settings.cock contains an invalid UTF-8 MovieBox path.";
                    }
                    else
                    {
                        movieBoxPathState_.savedExecutable = saved;
                        movieBoxPathState_.savedExecutableExists = IsRegularExe(saved);
                        if (movieBoxPathState_.savedExecutableExists)
                        {
                            movieBoxPathState_.source = MovieBoxPathSource::Saved;
                            movieBoxPathState_.executable = saved;
                            movieBoxPathState_.exists = true;
                        }
                    }
                }
            }
            else if (loaded.error != cock::Error::NotFound)
            {
                movieBoxPathState_.persistenceWarning = L"Settings.cock could not be loaded: " + WidenAscii(loaded.message);
            }
        }

        if (movieBoxPathState_.source != MovieBoxPathSource::Saved)
        {
            const fs::path standard = EffectiveStandardMovieBox();
            if (IsRegularExe(standard))
            {
                movieBoxPathState_.source = MovieBoxPathSource::AutoDetected;
                movieBoxPathState_.executable = standard;
                movieBoxPathState_.exists = true;
            }
        }

        paths_.movieBoxExecutable = movieBoxPathState_.executable;
        paths_.movieBoxUsesInstalledFallback = movieBoxPathState_.source == MovieBoxPathSource::AutoDetected;
    }

    bool Settings::AutoFindMovieBox(fs::path* found) const
    {
        const fs::path standard = EffectiveStandardMovieBox();
        const bool detected = IsRegularExe(standard);
        if (found)
            *found = detected ? standard : fs::path{};
        return detected;
    }

    bool Settings::SaveMovieBoxPath(const fs::path& executable, std::wstring* reason)
    {
        return SaveAll(executable, playerDefaults_, reason);
    }

    bool Settings::SaveAll(const fs::path& executable, const PlayerDefaults& defaults, std::wstring* reason)
    {
        auto fail = [&](std::wstring message)
        {
            if (reason) *reason = std::move(message);
            return false;
        };
        if (executable.empty()) return fail(L"MovieBox executable path is empty.");
        std::error_code ec;
        if (!fs::exists(executable, ec) || ec) return fail(L"MovieBox executable does not exist.");
        ec.clear(); if (!fs::is_regular_file(executable, ec) || ec) return fail(L"MovieBox path must point to a regular file.");
        if (!IsRegularExe(executable)) return fail(L"MovieBox path must point to an .exe file.");
        PlayerDefaults normalizedDefaults = defaults;
        if (normalizedDefaults.serverMode == ServerDefaultMode::Preferred)
        {
            auto& v = normalizedDefaults.serverPreference;
            const auto first = v.find_first_not_of(" \t\r\n"), last = v.find_last_not_of(" \t\r\n");
            if (first == std::string::npos) return fail(L"Preferred server cannot be empty.");
            v = v.substr(first, last-first+1);
        }
        ec.clear(); fs::path normalized = fs::weakly_canonical(executable, ec);
        if (ec || normalized.empty()) { ec.clear(); normalized = fs::absolute(executable, ec).lexically_normal(); if (ec || normalized.empty()) return fail(L"MovieBox executable path could not be normalized."); }
        std::string utf8; if (!PathToUtf8(normalized, utf8)) return fail(L"MovieBox path could not be converted to UTF-8.");
        LauncherSettingsData data{}; data.movieBoxPathUtf8 = utf8; data.playerDefaults = normalizedDefaults;
        std::string payload, encodeError; if (!EncodeLdef(data, payload, &encodeError)) return fail(L"Launcher settings are invalid: " + WidenAscii(encodeError));
        const fs::path settingsPath = SettingsFilePath(); if (settingsPath.empty()) return fail(L"LocalAppData could not be resolved for Settings.cock.");
        std::string error; if (!cock::SaveFileAtomic(settingsPath, cock::Document{payload}, &error)) return fail(L"Settings.cock could not be saved: " + WidenAscii(error));
        Refresh();
        if (movieBoxPathState_.source != MovieBoxPathSource::Saved || !movieBoxPathState_.savedExecutableExists) return fail(L"Settings were written but the saved MovieBox path could not be activated.");
        std::error_code equivalentError; if (!fs::equivalent(movieBoxPathState_.executable, normalized, equivalentError) || equivalentError) return fail(L"Settings were written but resolved to a different MovieBox executable.");
        if (reason) reason->clear();
        return true;
    }

    fs::path Settings::PlayerDirectory(PlayerKind player) const
    {
        return player == PlayerKind::Cpp ? paths_.cppPlayerDirectory : paths_.csharpPlayerDirectory;
    }

    std::wstring Settings::PlayerLabel(PlayerKind player) const
    {
        return player == PlayerKind::Cpp ? L"C++ Player" : L"C# Player";
    }

    std::wstring Settings::PlayerEnvironmentValue(PlayerKind player) const
    {
        return player == PlayerKind::Cpp ? L"CPP" : L"CSHARP";
    }

    bool Settings::IsReady(PlayerKind player, std::wstring* reason) const
    {
        if (!Exists(paths_.hookDll))
        {
            if (reason) *reason = MissingMessage(L"Hook DLL", paths_.hookDll);
            return false;
        }

        const fs::path playerDir = PlayerDirectory(player);
        if (!Exists(playerDir))
        {
            if (reason) *reason = MissingMessage(L"Selected player folder", playerDir);
            return false;
        }

        if (paths_.movieBoxExecutable.empty())
        {
            if (reason) *reason = L"MovieBoxPro.exe is not configured. Open Settings and choose an install location.";
            return false;
        }
        if (!IsRegularExe(paths_.movieBoxExecutable))
        {
            if (reason) *reason = MissingMessage(L"MovieBoxPro.exe", paths_.movieBoxExecutable);
            return false;
        }

        if (reason) reason->clear();
        return true;
    }

    LaunchResult Settings::Launch(PlayerKind player) const
    {
        LaunchResult result{};
        std::wstring readiness;
        if (!IsReady(player, &readiness))
        {
            result.message = std::move(readiness);
            return result;
        }

#ifdef _WIN32
        const std::wstring playerValue = PlayerEnvironmentValue(player);
        const std::wstring hookValue = paths_.hookDll.wstring();
        if (!SetEnvironmentVariableW(L"MOVIEBOX_PLAYER", playerValue.c_str()))
        {
            result.message = L"Failed to set MOVIEBOX_PLAYER. Win32 error: " + std::to_wstring(GetLastError());
            return result;
        }
        if (!SetEnvironmentVariableW(L"DOTNET_STARTUP_HOOKS", hookValue.c_str()))
        {
            result.message = L"Failed to set DOTNET_STARTUP_HOOKS. Win32 error: " + std::to_wstring(GetLastError());
            return result;
        }

        const std::wstring exe = paths_.movieBoxExecutable.wstring();
        std::wstring commandLine = L"\"" + exe + L"\"";
        std::wstring workingDirectory = paths_.movieBoxExecutable.parent_path().wstring();
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL created = CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &si, &pi);
        if (!created)
        {
            result.message = L"MovieBoxPro could not be launched. Win32 error: " + std::to_wstring(GetLastError());
            return result;
        }
        result.success = true;
        result.processId = pi.dwProcessId;
        result.message = L"MovieBoxPro launched with " + PlayerLabel(player) + L".";
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
#else
        result.message = L"MovieBox launch is only available on Windows.";
#endif
        return result;
    }

    void Settings::TerminateProcessesNamed(const wchar_t* imageName) noexcept
    {
#ifdef _WIN32
        struct ProcessInfo { DWORD pid = 0; DWORD parentPid = 0; std::wstring image; };
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return;
        std::vector<ProcessInfo> processes;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry))
        {
            do { processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile}); }
            while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        std::function<void(DWORD)> terminateTree = [&](DWORD pid)
        {
            for (const auto& process : processes)
                if (process.parentPid == pid && process.pid != pid) terminateTree(process.pid);
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (!process) return;
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 750);
            CloseHandle(process);
        };
        for (const auto& process : processes)
            if (_wcsicmp(process.image.c_str(), imageName) == 0) terminateTree(process.pid);
#else
        (void)imageName;
#endif
    }

    void Settings::CloseExistingMovieBoxProcesses() noexcept
    {
        TerminateProcessesNamed(L"MovieBoxPlayerMod.Player.exe");
        TerminateProcessesNamed(L"MovieBoxPro.exe");
    }
}
