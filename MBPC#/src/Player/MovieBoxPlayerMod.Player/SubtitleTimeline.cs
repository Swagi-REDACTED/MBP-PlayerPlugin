using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net;
using System.Text.RegularExpressions;
using MovieBoxPlayerMod.Bridge;

namespace MovieBoxPlayerMod.Player;

internal static class SubtitleTimeline
{
    internal static List<SubtitleCue> Parse(string text)
    {
        var result = new List<SubtitleCue>();
        foreach (string block in Regex.Split(text.Replace("\r", ""), @"\n\s*\n"))
        {
            var match = Regex.Match(block, @"(?m)^(?<start>(?:\d+:)?\d{2}:\d{2}[,.]\d{3})\s+-->\s+(?<end>(?:\d+:)?\d{2}:\d{2}[,.]\d{3})[^\n]*\n(?<text>[\s\S]+)");
            if (!match.Success) continue;
            double Time(string value)
            {
                string normalized = value.Replace(',', '.');
                if (normalized.Count(c => c == ':') == 1) normalized = "00:" + normalized;
                return TimeSpan.Parse(normalized, CultureInfo.InvariantCulture).TotalSeconds;
            }
            result.Add(new() { Start = Time(match.Groups["start"].Value), End = Time(match.Groups["end"].Value),
                Text = WebUtility.HtmlDecode(Regex.Replace(match.Groups["text"].Value.Trim(), "<[^>]+>", "")) });
        }
        return result.OrderBy(c => c.Start).ToList();
    }

    internal static string TextAt(IReadOnlyList<SubtitleCue> cues, double seconds)
    {
        // Binary search the last cue beginning at/before the playhead, including after backward seeks.
        int low = 0, high = cues.Count - 1, last = -1;
        while (low <= high)
        {
            int mid = low + (high - low) / 2;
            if (cues[mid].Start <= seconds) { last = mid; low = mid + 1; } else high = mid - 1;
        }
        if (last < 0) return "";
        // Include overlapping cues; no stateful cursor to become stale after seeking.
        var active = new List<string>();
        for (int i = 0; i <= last; i++)
            if (cues[i].End > seconds && cues[i].Text.Length > 0) active.Add(cues[i].Text);
        return string.Join("\n", active);
    }
}
