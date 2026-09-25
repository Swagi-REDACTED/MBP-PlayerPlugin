# MovieBoxPlayerMod

A custom video player mod for MovieBoxPro that intercepts playback and redirects it to a modern, high-performance player built with FlyleafLib.

## How It Works

When you launch MovieBoxPro through `LaunchMBP.bat`, the mod:

1. **Hooks into MovieBoxPro** using .NET's `DOTNET_STARTUP_HOOKS` — no files are modified
2. **Intercepts video playback** when you click Play on any movie/show
3. **Launches a custom player** with hardware-accelerated rendering and a clean UI

## Installation

### Quick Start (Pre-built)

1. Place the `MovieBoxPlayerMod` folder **next to** the `MovieBoxPro` folder:
   ```
   MovieBoxPro/
       MovieBoxPro.exe
       ...
   MovieBoxPlayerMod/
       LaunchMBP.bat
       build/
           Hook/
               MovieBoxPlayerMod.Hook.dll
           Player/
               MovieBoxPlayerMod.Player.exe
   ```
2. Double-click `LaunchMBP.bat`
3. Use MovieBoxPro normally — when you click Play, the custom player opens!

### Building from Source

Prerequisites: [.NET 8.0 SDK](https://dotnet.microsoft.com/download/dotnet/8.0)

```bash
cd MovieBoxPlayerMod
dotnet build MovieBoxPlayerMod.sln -c Release
```

Built files will be in `src/Hook/.../bin/Release/` and `src/Player/.../bin/Release/`.

To create a distributable `build` folder:
```bash
dotnet publish src/Hook/MovieBoxPlayerMod.Hook/MovieBoxPlayerMod.Hook.csproj -c Release -o build/Hook
dotnet publish src/Player/MovieBoxPlayerMod.Player/MovieBoxPlayerMod.Player.csproj -c Release -o build/Player
```

## Custom Player Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Space` | Play / Pause |
| `←` / `→` | Seek back / forward 10s |
| `↑` / `↓` | Volume up / down |
| `F` or `F11` | Toggle fullscreen |
| `Esc` | Exit fullscreen / Close |
| `M` | Toggle mute |

## Standalone Usage

The custom player also works as a standalone video player:

```bash
MovieBoxPlayerMod.Player.exe --uri "https://example.com/video.m3u8" --title "My Movie" --seconds 120
```

## Troubleshooting

- **Player doesn't open**: Check `playermod.log` in the `build/Hook/` folder for error details
- **MovieBoxPro won't start**: Make sure you're using `LaunchMBP.bat` and not launching MovieBoxPro.exe directly
- **Video doesn't play**: The custom player requires FFmpeg shared libraries — these should be bundled with FlyleafLib

## Architecture

- **Hook DLL** (`MovieBoxPlayerMod.Hook.dll`): Loaded via `DOTNET_STARTUP_HOOKS`, uses [Harmony](https://github.com/pardeike/Harmony) to patch `PlayerWindowFactory.Create()` and capture video metadata from Prism EventAggregator messages
- **Player EXE** (`MovieBoxPlayerMod.Player.exe`): Standalone WPF application using [FlyleafLib](https://github.com/SuRGeoNix/Flyleaf) for hardware-accelerated video playback via Direct3D 11
