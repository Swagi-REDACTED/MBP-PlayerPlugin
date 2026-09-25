#pragma once

#include <filesystem>
#include <string>
#include "LauncherDefaultsPayload.HPP"

namespace launcher
{
    enum class PlayerKind
    {
        Cpp,
        CSharp
    };

    struct Paths
    {
        std::filesystem::path launcherRoot;
        std::filesystem::path hookDll;
        std::filesystem::path cppPlayerDirectory;
        std::filesystem::path csharpPlayerDirectory;
        std::filesystem::path movieBoxExecutable;
        bool movieBoxUsesInstalledFallback = false;
    };

    struct LaunchResult
    {
        bool success = false;
        std::wstring message;
        unsigned long processId = 0;
    };

    enum class MovieBoxPathSource
    {
        None,
        Saved,
        AutoDetected
    };

    struct MovieBoxPathState
    {
        std::filesystem::path executable;
        std::filesystem::path savedExecutable;
        MovieBoxPathSource source = MovieBoxPathSource::None;
        bool exists = false;
        bool savedSettingsPresent = false;
        bool savedExecutableExists = false;
        std::wstring persistenceWarning;
    };

    struct SettingsOptions
    {
        std::filesystem::path executableDirectoryOverride;
        std::filesystem::path localAppDataOverride;
        std::filesystem::path standardMovieBoxExecutableOverride;
    };

    class Settings
    {
    public:
        Settings();
        explicit Settings(SettingsOptions options);

        void Refresh();
        const Paths& GetPaths() const noexcept { return paths_; }
        const MovieBoxPathState& GetMovieBoxPathState() const noexcept { return movieBoxPathState_; }
        std::filesystem::path SettingsFilePath() const;
        bool AutoFindMovieBox(std::filesystem::path* found = nullptr) const;
        bool SaveMovieBoxPath(const std::filesystem::path& executable, std::wstring* reason = nullptr);
        bool SaveAll(const std::filesystem::path& executable, const PlayerDefaults& defaults, std::wstring* reason = nullptr);
        const PlayerDefaults& GetPlayerDefaults() const noexcept { return playerDefaults_; }
        bool LegacyMigrationPending() const noexcept { return legacyMigrationPending_; }

        std::filesystem::path PlayerDirectory(PlayerKind player) const;
        std::wstring PlayerLabel(PlayerKind player) const;
        std::wstring PlayerEnvironmentValue(PlayerKind player) const;

        bool IsReady(PlayerKind player, std::wstring* reason = nullptr) const;
        LaunchResult Launch(PlayerKind player) const;

        static void CloseExistingMovieBoxProcesses() noexcept;

    private:
        static std::filesystem::path ExecutableDirectory();
        static std::filesystem::path FindLauncherRoot(const std::filesystem::path& exeDirectory);
        static void TerminateProcessesNamed(const wchar_t* imageName) noexcept;

        std::filesystem::path EffectiveExecutableDirectory() const;
        std::filesystem::path EffectiveLocalAppData() const;
        std::filesystem::path EffectiveStandardMovieBox() const;

        SettingsOptions options_{};
        Paths paths_{};
        MovieBoxPathState movieBoxPathState_{};
        PlayerDefaults playerDefaults_{};
        bool legacyMigrationPending_ = false;
    };
}
