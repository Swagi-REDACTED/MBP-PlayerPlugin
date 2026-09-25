using System;
using System.Collections.Generic;
using System.Text.Json;

namespace MovieBoxPlayerMod.Bridge;

public sealed class PlaybackRequest
{
    public string Action { get; set; } = "poll";
    public long Revision { get; set; }
    public double Position { get; set; }
    public double Duration { get; set; }
    public bool Playing { get; set; }
    public bool Ready { get; set; }
    public int Volume { get; set; }
    public bool Muted { get; set; }
    public bool Metadata { get; set; }
    public string Id { get; set; } = "";
    public double Value { get; set; }
}

public sealed class PlaybackReply
{
    public string? Error { get; set; }
    public PlaybackSource? Source { get; set; }
    public PlaybackMetadata? Metadata { get; set; }
    public List<PlaybackCommand> Commands { get; set; } = new();
    public List<SubtitleCue>? Cues { get; set; }
    public double SubtitleDelay { get; set; }
    public bool Closed { get; set; }
}

public sealed class PlaybackSource
{
    public long Revision { get; set; }
    public string Uri { get; set; } = "";
    public string Title { get; set; } = "MovieBoxPro";
    public string Engine { get; set; } = "FFmpeg";
    public double Seconds { get; set; }
    public bool Playing { get; set; } = true;
    public double Rate { get; set; } = 1;
    public string SettingsTitle { get; set; } = "";
    public short Season { get; set; }
    public short Episode { get; set; }
    public byte BoxType { get; set; }
}

public sealed class PlaybackMetadata
{
    public bool IsSeries { get; set; }
    public bool Loading { get; set; }
    public string? Error { get; set; }
    public List<PlaybackChoice> Episodes { get; set; } = new();
    public List<PlaybackChoice> Qualities { get; set; } = new();
    public List<PlaybackChoice> Subtitles { get; set; } = new();
    public List<PlaybackChoice> Servers { get; set; } = new();
}

public sealed class PlaybackChoice
{
    public string Id { get; set; } = "";
    public string Label { get; set; } = "";
    public string Group { get; set; } = "";
    public bool Selected { get; set; }
}

public sealed class PlaybackCommand
{
    public string Action { get; set; } = "";
    public Dictionary<string, JsonElement> Values { get; set; } = new();
}

public sealed class SubtitleCue
{
    public double Start { get; set; }
    public double End { get; set; }
    public string Text { get; set; } = "";
}

public static class PlaybackValidation
{
    public static bool IsCurrentProgress(PlaybackRequest request, long revision) =>
        request.Ready && request.Revision == revision &&
        double.IsFinite(request.Position) && double.IsFinite(request.Duration) &&
        request.Position >= 0 && request.Duration > 0 && request.Position <= request.Duration + 1;
}
