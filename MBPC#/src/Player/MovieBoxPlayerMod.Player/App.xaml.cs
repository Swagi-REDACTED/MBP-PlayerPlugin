using System;
using System.IO;
using System.Windows;
using FlyleafLib;

namespace MovieBoxPlayerMod.Player
{
    public partial class App : Application
    {
        public static string? VideoUri { get; private set; }
        public static string? VideoTitle { get; private set; }
        public static double ResumeSeconds { get; private set; }
        public static string? UserAgent { get; private set; }
        public static string? BridgeName { get; private set; }

        protected override void OnStartup(StartupEventArgs e)
        {
            LaunchOptions options = LaunchOptions.Parse(e.Args);
            VideoUri = options.VideoUri;
            VideoTitle = options.VideoTitle;
            ResumeSeconds = options.ResumeSeconds;
            UserAgent = options.UserAgent;
            BridgeName = options.BridgeName;

            try
            {
                Engine.Start(new EngineConfig()
                {
                    FFmpegPath = ResolveFFmpegPath(),
                    UIRefresh = true
                });
            }
            catch (Exception ex)
            {
                MessageBox.Show(
                    $"MovieBox Player could not start the media engine.\n\n{ex.Message}",
                    "MovieBox Player",
                    MessageBoxButton.OK,
                    MessageBoxImage.Error);
                Shutdown(-1);
                return;
            }

            base.OnStartup(e);
        }

        private static string ResolveFFmpegPath()
        {
            string? environmentPath = Environment.GetEnvironmentVariable("MOVIEBOX_FFMPEG_PATH");
            string appDirectory = AppContext.BaseDirectory;

            string[] candidates =
            {
                // Explicit override first, then preserve the exact path used by the
                // working MovieBox install before trying portable side-by-side folders.
                environmentPath ?? string.Empty,
                @"C:\MovieBox\MovieBoxPro\MovieBoxPro\FFmpeg",
                Path.Combine(appDirectory, "FFmpeg"),
                Path.Combine(appDirectory, "ffmpeg")
            };

            foreach (string candidate in candidates)
            {
                if (!string.IsNullOrWhiteSpace(candidate) && Directory.Exists(candidate))
                    return candidate;
            }

            // Preserve the original MovieBox layout as the final fallback so existing
            // installations keep the same failure behavior instead of silently using
            // an unrelated FFmpeg installation from PATH.
            return @"C:\MovieBox\MovieBoxPro\MovieBoxPro\FFmpeg";
        }
    }
}
