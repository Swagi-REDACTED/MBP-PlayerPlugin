# MBP-PlayerPlugin

A custom player plugin for **MBP** that intercepts playback and redirects it into your choice of two custom video players:

* **Native C++ Player**
* **C# / Flyleaf Player**

The project also includes a dedicated **DX12 launcher** for choosing the player backend, configuring MBP, and managing shared playback defaults.

MBP itself is not modified THIS IS NOT A DRM BYPASS YOU MUST OWN MBP.

---

## Features

### Custom Launcher

The included launcher provides a polished interface for starting MBP and selecting which player backend to use.

* Select between the **C++ Player** and **C# Player**
* Automatically configures the required `DOTNET_STARTUP_HOOKS`
* Detects or manually configures the MBP executable
* Saves launcher configuration in a compact `.cock` settings file
* Smooth animated Settings panel
* Shared player defaults:

  * Default subtitle language or **Off**
  * Default target quality or **Auto**
  * Default server or **Auto**
  * Default audio level
* Smooth scrolling and custom Settings UI
* Automatically launches MBP with the selected player backend

---

## Players

### Native C++ Player

A custom native Windows video player designed for low overhead and tight MBP integration.

Features include:

* Native C++ UI
* Direct3D rendering
* MBP bridge integration
* Quality switching
* Server switching
* Subtitle selection
* Audio track selection
* Playback speed control
* Per-episode settings
* Fullscreen support
* Custom player UI and shaders
* Loading visuals
* Standalone playback support

---

### C# Player

A Windows WPF player powered by **FlyleafLib**.

Features include:

* Hardware-accelerated playback
* Direct3D 11 rendering through Flyleaf
* MBP bridge integration
* Quality switching
* Server switching
* Subtitle selection
* Audio track selection
* Playback speed control
* Custom fullscreen/player UI
* Per-episode settings
* Standalone playback support

---

## How It Works

When MBP is launched through the MBP Player Plugin launcher:

1. The launcher selects either the **C++** or **C#** player backend.
2. It configures `.NET`'s `DOTNET_STARTUP_HOOKS`.
3. `MovieBoxPlayerMod.Hook.dll` is injected when MBP starts.
4. The hook intercepts MBP playback and exposes playback information through the bridge.
5. When you play a movie or episode, playback is redirected to the selected custom player.
6. The player communicates with MBP for metadata and playback options such as:

   * subtitles
   * qualities
   * servers
   * episode information
7. Per-episode preferences are automatically restored when available.

No MBP files need to be patched or replaced.

---

## Settings System

MBP-PlayerPlugin uses its own compact binary settings format with the `.cock` extension.

### Launcher Settings

Stored under:

```text
%LOCALAPPDATA%\MovieBoxPlayerMod\Launcher\Settings.cock
```

Launcher settings include:

* MBP executable path
* Default subtitle language
* Default quality
* Default server
* Default audio level

### Episode Settings

Individual movie/episode settings are stored separately under the MovieBoxPlayerMod settings directory.

Episode settings can preserve things such as:

* Volume
* Mute state
* Playback speed
* Subtitle delay
* Audio track
* Subtitle preference
* Quality preference
* Server preference

### Default Precedence

Player defaults are only used when an episode does not already have saved settings.

```text
Existing episode .cock
        │
        ├── Yes → Restore episode settings
        │
        └── No  → Use launcher player defaults
```

Once an episode has its own saved profile, that profile takes priority over launcher defaults.

---

# Installation

## Quick Start — Pre-built

A typical installation looks like:

```text
MovieBox/
│
├── MovieBoxPro/
│   ├── MovieBoxPro.exe
│   └── ...
│
└── MovieBoxPlayerMod/
    ├── MBPModLauncher.exe
    │
    └── build/
        ├── Hook/
        │   └── MovieBoxPlayerMod.Hook.dll
        │
        ├── PlayerC#/
        │   └── MovieBoxPlayerMod.Player.exe
        │
        └── PlayerCPP/
            └── ...
```

Then:

1. Run `MBPModLauncher.exe`.
2. Choose either the **C++ Player** or **C# Player**.
3. Open Settings using the gear button if MBP was not automatically detected.
4. Configure any player defaults you want.
5. Click **Launch MovieBox**.
6. Use MBP normally.

When playback begins, the selected custom player will open automatically.

---

# Building from Source

The project contains both .NET and native C++ components.

## Requirements

### Hook + C# Player

* Windows
* [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0)

### C++ Player + Launcher

* Windows
* Visual Studio 2022 or newer
* Desktop development with C++
* Windows SDK

---

## Build the Hook

From the `MovieBoxPlayerMod` root:

```powershell
dotnet build src/Hook/MovieBoxPlayerMod.Hook/MovieBoxPlayerMod.Hook.csproj -c Release -o build/Hook
```

---

## Build the C# Player

```powershell
dotnet publish src/Player/MovieBoxPlayerMod.Player/MovieBoxPlayerMod.Player.csproj -c Release -o build/PlayerC#
```

You can also build both consecutively:

```powershell
cmd /c "dotnet build src/Hook/MovieBoxPlayerMod.Hook/MovieBoxPlayerMod.Hook.csproj -c Release -o build/Hook && dotnet publish src/Player/MovieBoxPlayerMod.Player/MovieBoxPlayerMod.Player.csproj -c Release -o build/PlayerC#"
```

---

## Build the C++ Player

Open the included C++ player project in Visual Studio and build:

```text
Configuration: Release
Platform:      x64
```

Place the resulting player files in:

```text
build/PlayerCPP/
```

---

## Build the Launcher

Open the included `MBPModLauncher` Visual Studio project and build:

```text
Configuration: Release
Platform:      x64
```

The launcher is a native Windows application using **DirectX 12 + Dear ImGui**.

---

# Player Selection

The launcher communicates the selected backend through:

```text
MOVIEBOX_PLAYER=CPP
```

or:

```text
MOVIEBOX_PLAYER=CSHARP
```

The hook uses this value to route playback to the appropriate player.

---

# Keyboard Shortcuts

Player-specific shortcuts can differ slightly between the two backends, but common controls include:

| Key         | Action                  |
| ----------- | ----------------------- |
| `Space`     | Play / Pause            |
| `←` / `→`   | Seek backward / forward |
| `↑` / `↓`   | Adjust volume           |
| `F` / `F11` | Toggle fullscreen       |
| `Esc`       | Exit fullscreen / close |
| `M`         | Toggle mute             |

---

# Standalone Usage

The players can also be used without MBP.

### Local Player Usage

```powershell
MovieBoxPlayerMod.Player.exe --uri "https://example.com/video.m3u8"
```

Additional launch arguments may be supplied by MBP when running through the bridge.

The native and C# player can likewise operate independently of the MovieBox bridge.

---

# Architecture

```text
                MBPModLauncher
                      │
                      │ launches
                      ▼
                     MBP
                      │
            DOTNET_STARTUP_HOOKS
                      │
                      ▼
         MovieBoxPlayerMod.Hook.dll
                      │
              Playback Bridge
                 /          \
                /            \
               ▼              ▼
        C++ Player        C# Player
                            Flyleaf
```

### Launcher

Native **C++ / DirectX 12 / Dear ImGui** application responsible for:

* Player selection
* MBP path configuration
* Shared player defaults
* Launching MBP
* Setting the required environment variables

### Hook

`MovieBoxPlayerMod.Hook.dll`

Loaded through:

```text
DOTNET_STARTUP_HOOKS
```

The hook integrates with MBP and exposes playback information to the selected player.

### C++ Player

Native Windows player responsible for video playback and MovieBox bridge integration.

### C# Player

WPF application using [FlyleafLib](https://github.com/SuRGeoNix/Flyleaf) for hardware-accelerated playback.

---

# Playback Bridge

The MovieBox bridge allows the custom players to interact with the current MBP playback session.

Depending on the available metadata, players can access and control:

* Movie / episode identity
* Playback URI
* Playback progress
* Subtitle tracks
* Video qualities
* Playback servers
* Subtitle delay
* Playback rate

This lets both custom players remain synchronized with MBP while replacing its normal playback window.

---

# `.cock` Format

MBP-PlayerPlugin uses a lightweight custom binary format for persistent settings.

The format is designed to be:

* Small
* Fast to read
* Fast to write
* Dependency-free
* Atomically saved
* Compatible between the C++ and C# implementations

Existing episode files remain compatible while newer players can append additional preference information.

---

# Troubleshooting

### MBP does not launch

Open the launcher Settings and verify that the configured path points to:

```text
MovieBoxPro.exe
```

The default location checked by the launcher is:

```text
C:\Program Files\MovieBoxPro\MovieBoxPro\MovieBoxPro.exe
```

---

### Custom player does not open

Check that:

* the Hook was built successfully;
* the selected player exists in its expected build folder;
* MBP was launched through `MBPModLauncher`;
* the appropriate player was selected before launch.

Hook diagnostics can also be checked in the generated `playermod.log`.

---

### C# player does not play video

Make sure all required Flyleaf/FFmpeg runtime files are present beside the published C# player.

---

### Settings are not being restored

Launcher defaults only apply to content without an existing episode profile.

If an episode already has its own `.cock` file, its saved settings intentionally take precedence.

---

# Project Components

| Component                | Technology         | Purpose                                      |
| ------------------------ | ------------------ | -------------------------------------------- |
| `MBPModLauncher`         | C++ / DX12 / ImGui | Launching, player selection, global defaults |
| `MovieBoxPlayerMod.Hook` | .NET / Harmony     | MBP interception and bridge                  |
| C# Player                | WPF / Flyleaf /    |                                              |
