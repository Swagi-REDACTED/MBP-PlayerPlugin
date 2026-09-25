using System;
using System.Globalization;

namespace MovieBoxPlayerMod.Player
{
    internal sealed class LaunchOptions
    {
        public string? VideoUri { get; private set; }
        public string? VideoTitle { get; private set; }
        public double ResumeSeconds { get; private set; }
        public string? UserAgent { get; private set; }
        public string? BridgeName { get; private set; }

        public static LaunchOptions Parse(string[] args)
        {
            LaunchOptions options = new LaunchOptions();

            for (int i = 0; i < args.Length; i++)
            {
                string token = args[i];
                string key = token;
                string? inlineValue = null;

                int equalsIndex = token.IndexOf('=');
                if (equalsIndex > 0)
                {
                    key = token[..equalsIndex];
                    inlineValue = token[(equalsIndex + 1)..];
                }

                switch (key.ToLowerInvariant())
                {
                    case "--bridge":
                        options.BridgeName = ReadValue(inlineValue, args, ref i);
                        break;
                    case "--uri":
                        options.VideoUri = ReadValue(inlineValue, args, ref i);
                        break;

                    case "--title":
                        options.VideoTitle = ReadValue(inlineValue, args, ref i);
                        break;

                    case "--seconds":
                    case "--start":
                        string? secondsText = ReadValue(inlineValue, args, ref i);
                        if (double.TryParse(secondsText, NumberStyles.Float, CultureInfo.InvariantCulture, out double seconds))
                            options.ResumeSeconds = Math.Max(0, seconds);
                        break;

                    case "--user-agent":
                        options.UserAgent = ReadValue(inlineValue, args, ref i);
                        break;
                }
            }

            return options;
        }

        private static string? ReadValue(string? inlineValue, string[] args, ref int index)
        {
            if (inlineValue != null)
                return inlineValue;

            if (index + 1 >= args.Length)
                return null;

            return args[++index];
        }
    }
}
