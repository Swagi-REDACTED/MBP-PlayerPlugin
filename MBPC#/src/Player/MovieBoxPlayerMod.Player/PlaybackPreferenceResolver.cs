using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace MovieBoxPlayerMod.Player;

internal enum ProfileSource
{
    Unknown,
    EpisodeCock,
    LauncherDefaults
}

internal readonly record struct PreferenceChoice(string Id, string Label, bool Selected = false);
internal readonly record struct PreferenceDecision(string Action, string Id);

internal sealed class PlaybackPreferenceResolver
{
    private bool subtitlePending, qualityPending, serverPending;
    private bool subtitleOff, qualityAuto, serverAuto;
    private string subtitleLanguage = string.Empty, serverPreference = string.Empty;
    private ushort qualityTarget;

    private static readonly Dictionary<string, string[]> LanguageAliases = new(StringComparer.OrdinalIgnoreCase)
    {
        ["en"] = new[] { "English" }, ["es"] = new[] { "Spanish" }, ["fr"] = new[] { "French" },
        ["de"] = new[] { "German" }, ["it"] = new[] { "Italian" }, ["pt"] = new[] { "Portuguese" },
        ["ja"] = new[] { "Japanese" }, ["ko"] = new[] { "Korean" }, ["zh"] = new[] { "Chinese" },
        ["ar"] = new[] { "Arabic" }, ["ru"] = new[] { "Russian" }, ["hi"] = new[] { "Hindi" }
    };

    public ProfileSource Source { get; private set; } = ProfileSource.Unknown;
    public bool HasPending => subtitlePending || qualityPending || serverPending;

    public void ResetLauncherDefaults(LauncherPlayerDefaults defaults)
    {
        Source = ProfileSource.LauncherDefaults;
        subtitlePending = qualityPending = serverPending = true;
        subtitleOff = defaults.SubtitleOff;
        subtitleLanguage = defaults.SubtitleLanguage ?? string.Empty;
        qualityAuto = defaults.QualityAuto;
        qualityTarget = defaults.TargetQualityHeight;
        serverAuto = defaults.ServerAuto;
        serverPreference = defaults.ServerPreference ?? string.Empty;
    }

    public void ResetEpisode(EpisodePreferences preferences)
    {
        Source = ProfileSource.EpisodeCock;
        subtitlePending = preferences.SubtitlePresent;
        qualityPending = preferences.QualityPresent;
        serverPending = preferences.ServerPresent;
        subtitleOff = preferences.SubtitleOff;
        subtitleLanguage = preferences.SubtitleLanguage ?? string.Empty;
        qualityAuto = preferences.QualityAuto;
        qualityTarget = preferences.TargetQualityHeight;
        serverAuto = preferences.ServerAuto;
        serverPreference = preferences.ServerPreference ?? string.Empty;
    }

    public void ResetUnknown()
    {
        Source = ProfileSource.Unknown;
        subtitlePending = qualityPending = serverPending = false;
    }

    public IReadOnlyList<PreferenceDecision> Settle(
        IEnumerable<PreferenceChoice> subtitles,
        IEnumerable<PreferenceChoice> qualities,
        IEnumerable<PreferenceChoice> servers)
    {
        var decisions = new List<PreferenceDecision>(3);
        if (subtitlePending)
        {
            subtitlePending = false;
            if (subtitleOff)
                decisions.Add(new PreferenceDecision("subtitle", "off"));
            else
            {
                PreferenceChoice? match = MatchSubtitle(subtitleLanguage, subtitles);
                decisions.Add(new PreferenceDecision("subtitle", match?.Id ?? "off"));
            }
        }
        if (qualityPending)
        {
            qualityPending = false;
            if (!qualityAuto)
            {
                PreferenceChoice? match = MatchQuality(qualityTarget, qualities);
                if (match is { } choice) decisions.Add(new PreferenceDecision("quality", choice.Id));
            }
        }
        if (serverPending)
        {
            serverPending = false;
            if (!serverAuto)
            {
                PreferenceChoice? match = MatchServer(serverPreference, servers);
                if (match is { } choice) decisions.Add(new PreferenceDecision("server", choice.Id));
            }
        }
        return decisions;
    }

    internal static PreferenceChoice? MatchQuality(ushort target, IEnumerable<PreferenceChoice> choices)
    {
        PreferenceChoice? best = null;
        int bestDistance = int.MaxValue;
        int bestHeight = -1;
        foreach (PreferenceChoice choice in choices)
        {
            if (!TryParseQuality(choice, out int height)) continue;
            int distance = Math.Abs(height - target);
            if (distance < bestDistance || (distance == bestDistance && height > bestHeight))
            {
                best = choice; bestDistance = distance; bestHeight = height;
            }
        }
        return best;
    }

    internal static PreferenceChoice? MatchServer(string preferred, IEnumerable<PreferenceChoice> choices)
    {
        string needle = (preferred ?? string.Empty).Trim();
        if (needle.Length == 0) return null;
        PreferenceChoice? byId = choices.FirstOrDefault(c => string.Equals(c.Id?.Trim(), needle, StringComparison.OrdinalIgnoreCase));
        if (byId is { } idChoice && !string.IsNullOrEmpty(idChoice.Id)) return idChoice;
        foreach (PreferenceChoice choice in choices)
            if (string.Equals(choice.Label?.Trim(), needle, StringComparison.OrdinalIgnoreCase)) return choice;
        return null;
    }

    internal static PreferenceChoice? MatchSubtitle(string languageCode, IEnumerable<PreferenceChoice> choices)
    {
        string code = NormalizeCode(languageCode);
        if (code.Length == 0) return null;
        foreach (PreferenceChoice choice in choices)
            if (string.Equals(NormalizeCode(choice.Id), code, StringComparison.Ordinal)) return choice;
        foreach (PreferenceChoice choice in choices)
        {
            string id = NormalizeCode(choice.Id);
            if (id.StartsWith(code + "-", StringComparison.Ordinal) || id.StartsWith(code + "_", StringComparison.Ordinal)) return choice;
        }
        if (!LanguageAliases.TryGetValue(code, out string[]? aliases)) return null;
        foreach (PreferenceChoice choice in choices)
            foreach (string alias in aliases)
                if (string.Equals(choice.Label?.Trim(), alias, StringComparison.OrdinalIgnoreCase)) return choice;
        foreach (PreferenceChoice choice in choices)
        {
            string label = choice.Label?.Trim() ?? string.Empty;
            foreach (string alias in aliases)
                if (label.StartsWith(alias + " ", StringComparison.OrdinalIgnoreCase) ||
                    label.StartsWith(alias + "(", StringComparison.OrdinalIgnoreCase) ||
                    label.StartsWith(alias + "-", StringComparison.OrdinalIgnoreCase) ||
                    label.StartsWith(alias + "[", StringComparison.OrdinalIgnoreCase)) return choice;
        }
        return null;
    }

    internal static bool TryParseQuality(PreferenceChoice choice, out int height)
    {
        foreach (string value in new[] { choice.Id ?? string.Empty, choice.Label ?? string.Empty })
        {
            Match m = Regex.Match(value, @"(?<!\d)(2160|1440|1080|720|480|360)p?(?!\d)", RegexOptions.IgnoreCase);
            if (m.Success && int.TryParse(m.Groups[1].Value, out height)) return true;
        }
        string text = ((choice.Id ?? string.Empty) + " " + (choice.Label ?? string.Empty)).ToUpperInvariant();
        if (text.Contains("UHD") || Regex.IsMatch(text, @"(^|\W)4K($|\W)")) { height = 2160; return true; }
        if (text.Contains("QHD")) { height = 1440; return true; }
        if (text.Contains("FHD")) { height = 1080; return true; }
        if (Regex.IsMatch(text, @"(^|\W)HD($|\W)")) { height = 720; return true; }
        height = 0; return false;
    }

    internal static string CanonicalLanguageForChoice(PreferenceChoice choice)
    {
        string id = NormalizeCode(choice.Id);
        foreach (string code in LanguageAliases.Keys)
            if (id == code || id.StartsWith(code + "-", StringComparison.Ordinal) || id.StartsWith(code + "_", StringComparison.Ordinal)) return code;
        string label = choice.Label?.Trim() ?? string.Empty;
        foreach ((string code, string[] aliases) in LanguageAliases)
            foreach (string alias in aliases)
                if (string.Equals(label, alias, StringComparison.OrdinalIgnoreCase) || label.StartsWith(alias + " ", StringComparison.OrdinalIgnoreCase) || label.StartsWith(alias + "(", StringComparison.OrdinalIgnoreCase)) return code;
        return string.Empty;
    }

    private static string NormalizeCode(string? value) => (value ?? string.Empty).Trim().ToLowerInvariant();
}
