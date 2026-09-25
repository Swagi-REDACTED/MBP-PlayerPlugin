using System;

namespace MovieBoxPlayerMod.Player
{
    internal static class PlayerUiLogic
    {
        public static long ClampSeekMilliseconds(long requestedMilliseconds, long durationMilliseconds)
        {
            long requested = Math.Max(0, requestedMilliseconds);
            if (durationMilliseconds <= 0)
                return requested;

            return Math.Min(requested, durationMilliseconds);
        }


        public static double SnapPlaybackPositionToEnd(
            double currentMilliseconds,
            double durationMilliseconds,
            double terminalThresholdMilliseconds = 50.0)
        {
            if (double.IsNaN(currentMilliseconds) || double.IsInfinity(currentMilliseconds))
                currentMilliseconds = 0;

            if (double.IsNaN(durationMilliseconds) || double.IsInfinity(durationMilliseconds) || durationMilliseconds <= 0)
                return Math.Max(0, currentMilliseconds);

            double current = Math.Clamp(currentMilliseconds, 0, durationMilliseconds);
            double threshold = Math.Max(0, terminalThresholdMilliseconds);

            return durationMilliseconds - current <= threshold
                ? durationMilliseconds
                : current;
        }

        public static string FormatTime(double milliseconds)
        {
            if (double.IsNaN(milliseconds) || double.IsInfinity(milliseconds) || milliseconds < 0)
                milliseconds = 0;

            TimeSpan time = TimeSpan.FromMilliseconds(milliseconds);
            if (time.TotalHours >= 1)
                return $"{(int)time.TotalHours}:{time.Minutes:00}:{time.Seconds:00}";

            return $"{time.Minutes}:{time.Seconds:00}";
        }
    }
}
