using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows;
using System.Windows.Threading;
using HarmonyLib;

internal static class StartupHook
{
    private static Harmony? _harmony;
    private static string? _playerExePath;
    internal static object? PlayerViewModelInstance;
    internal static readonly Dictionary<string, object> Models = new();
    internal static string CapturedTitle = "MovieBoxPro";
    internal static PlaybackBridge? Bridge;
    private static bool _patched;

    public static void Initialize()
    {
        string hookDir = Path.GetDirectoryName(typeof(StartupHook).Assembly.Location)!;
        AppDomain.CurrentDomain.AssemblyResolve += (_, args) =>
        {
            string path = Path.Combine(hookDir, new AssemblyName(args.Name).Name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };
        InitializeInternal(hookDir);
    }

    // Keep Harmony references out of Initialize until AssemblyResolve is installed.
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.NoInlining)]
    private static void InitializeInternal(string hookDir)
    {
        string? playerVariant = Environment.GetEnvironmentVariable("MOVIEBOX_PLAYER");
        string? playerFolder = playerVariant?.Trim().ToUpperInvariant() switch
        {
            "CPP" => "PlayerCPP",
            "CSHARP" => "PlayerC#",
            _ => null
        };
        if (playerFolder == null)
        {
            Log($"Invalid or missing MOVIEBOX_PLAYER selection '{playerVariant ?? "<null>"}'; leaving original playback enabled.");
            return;
        }

        _playerExePath = Path.GetFullPath(Path.Combine(hookDir, "..", playerFolder, "MovieBoxPlayerMod.Player.exe"));
        if (!File.Exists(_playerExePath))
        {
            Log($"Selected custom player missing at '{_playerExePath}'; leaving original playback enabled.");
            _playerExePath = null;
            return;
        }
        Log($"Selected {playerVariant} player: {_playerExePath}");
        Bridge = new PlaybackBridge();
        AppDomain.CurrentDomain.AssemblyLoad += OnAssemblyLoad;
        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
            if (assembly.GetName().Name == "MovieBoxPro") Patch(assembly);
    }

    private static void OnAssemblyLoad(object? sender, AssemblyLoadEventArgs args)
    {
        if (args.LoadedAssembly.GetName().Name == "MovieBoxPro") Patch(args.LoadedAssembly);
    }

    private static void Patch(Assembly assembly)
    {
        if (_patched) return;
        _patched = true;
        try
        {
            _harmony = new Harmony("com.movieboxplayermod.hook");
            Type pvm = assembly.GetType("MovieBoxPro.Resources.UserControls.VlcPlayer.ViewModels.PlayerViewModel", true)!;
            _harmony.Patch(AccessTools.Method(pvm, "WindowViewModelPublish"),
                prefix: new HarmonyMethod(typeof(StartupHook), nameof(PlaybackPrefix)));
            _harmony.Patch(AccessTools.Method(pvm, "WindowPublish"),
                prefix: new HarmonyMethod(typeof(StartupHook), nameof(WindowPrefix)));
            foreach (string name in new[] { "VideoSubtitleLstViewModel", "VideoQualityLstViewModel", "VideoEpisodeLstViewModel", "SubtitleContentViewModel" })
            {
                Type type = assembly.GetType("MovieBoxPro.Resources.UserControls.VlcPlayer.ViewModels." + name, true)!;
                foreach (var ctor in type.GetConstructors(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
                    _harmony.Patch(ctor, postfix: new HarmonyMethod(typeof(StartupHook), nameof(CaptureModel)));
            }
            Log("Playback bridge installed for VLC, FFmpeg and MPV view models.");
        }
        catch (Exception ex)
        {
            _harmony?.UnpatchAll("com.movieboxplayermod.hook");
            Bridge = null;
            Log("Hook installation failed; original player remains enabled: " + ex);
        }
        AppDomain.CurrentDomain.AssemblyLoad -= OnAssemblyLoad;
    }

    private static void CaptureModel(object __instance) => Models[__instance.GetType().Name] = __instance;

    private static void WindowPrefix(Dictionary<string, object> __0)
    {
        if (__0.TryGetValue("Title", out object? title) && title is string text) CapturedTitle = text;
    }

    private static bool PlaybackPrefix(object __instance, Dictionary<string, object> __0)
    {
        if (Bridge == null || !__0.TryGetValue("MediaPlayer", out var value)) return true;
        string action = value?.ToString() ?? "";
        if (action is "Play" or "HotSwapPlay" && __0.TryGetValue("Uri", out var uri) && uri is string text && !string.IsNullOrWhiteSpace(text))
        {
            try
            {
                PlayerViewModelInstance = __instance;
                Bridge.Capture(__instance, __0);
                Bridge.EnsurePlayer(_playerExePath!);
                Application.Current.Dispatcher.BeginInvoke(() =>
                {
                    foreach (Window window in Application.Current.Windows)
                        if (window.GetType().Name is "PlayerVlcWindow" or "PlayerFFmpegWindow" or "PlayerMpvWindow")
                        {
                            // Don't Close or Hide — that triggers Unloaded and kills the session.
                            // Instead make the window completely invisible and non-interactive.
                            window.ShowInTaskbar = false;
                            window.Opacity = 0;
                            window.IsHitTestVisible = false;
                            window.Focusable = false;
                            window.Width = 0;
                            window.Height = 0;
                            window.Left = -30000;
                            window.Top = -30000;
                            window.Topmost = false;
                            window.WindowState = WindowState.Normal;
                        }
                }, DispatcherPriority.Background);
                return false; // The hidden original renderer must never play a second copy.
            }
            catch (Exception ex) { Log("Custom playback failed: " + ex); return true; }
        }
        if (Bridge.IsActive)
        {
            Bridge.CaptureCommand(action, __0);
            return false;
        }
        return true;
    }

    internal static void Log(string text)
    {
        try
        {
            string path = Path.Combine(Path.GetDirectoryName(typeof(StartupHook).Assembly.Location)!, "playermod.log");
            File.AppendAllText(path, $"[{DateTime.Now:O}] {text}{Environment.NewLine}");
        }
        catch { }
    }
}
