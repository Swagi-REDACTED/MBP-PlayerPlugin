using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text.Json;
using System.Windows;
using MovieBoxPlayerMod.Bridge;
using static OriginalApp;

internal sealed class PlaybackBridge
{
    private readonly string pipeName = $"MovieBoxPlayerMod-{Environment.ProcessId}-{Guid.NewGuid():N}";
    private Process? process;
    private object? vm;
    private PlaybackSource? source;
    private readonly Queue<PlaybackCommand> commands = new();
    private readonly Dictionary<string, object> episodes = new(), servers = new(), qualities = new(), subtitles = new();
    private object? seriesDetail, lastCueContent;
    private bool loading, closed, closing, opened;
    private string? metadataError;
    private string? closeError;
    private int hotSwapToken;
    private double position, duration;
    private long revision;
    internal bool IsActive => source != null && !closed;

    internal PlaybackBridge() => _ = ListenAsync();

    internal void Capture(object viewModel, Dictionary<string, object> data)
    {
        vm = viewModel;
        closed = false;
        closeError = null;
        opened = false;
        commands.Clear();
        var detail = Get(vm, "MTDetail");
        var episode = Get(vm, "Episode");
        string settingsTitle = Text(detail, "title");
        string title = settingsTitle;
        bool isSeries = Number(detail, "box_type") == 2;
        short season = short.TryParse(Text(episode, "season"), out short parsedSeason) ? parsedSeason : (short)0;
        short episodeNumber = short.TryParse(Text(episode, "episode"), out short parsedEpisode) ? parsedEpisode : (short)0;
        if (isSeries) title += $"  S{season}:E{episodeNumber}";
        source = new PlaybackSource
        {
            Revision = ++revision, Uri = data["Uri"].ToString()!, Title = title.Length > 0 ? title : StartupHook.CapturedTitle,
            SettingsTitle = settingsTitle.Length > 0 ? settingsTitle : StartupHook.CapturedTitle,
            Season = season, Episode = episodeNumber, BoxType = isSeries ? (byte)2 : (byte)1,
            Engine = "FFmpeg", Seconds = Convert.ToDouble(data.GetValueOrDefault("seconds") ?? 0),
            Rate = double.TryParse(Convert.ToString(data.GetValueOrDefault("rate")), out double rate) ? rate : 1,
            Playing = data.GetValueOrDefault("WasPlaying") is not false
        };
        position = source.Seconds;
        duration = 0;
        lastCueContent = null;
        hotSwapToken = Convert.ToInt32(data.GetValueOrDefault("HotSwapToken") ?? 0);
        if (!ReferenceEquals(detail, seriesDetail))
        {
            seriesDetail = detail;
            episodes.Clear();
            foreach (object item in Items(Get(detail, "episode"))) episodes[EpisodeId(item)] = item;
            _ = LoadMetadataAsync(detail, isSeries);
        }
        StartupHook.Log($"Playback revision {revision}, engine selection {Text(vm, "Engine")}, series={isSeries}.");
    }

    internal void EnsurePlayer(string executable)
    {
        if (process != null && !process.HasExited) return;
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(executable)! };
        start.ArgumentList.Add("--bridge"); start.ArgumentList.Add(pipeName);
        string ua = Text(Global, "UserAgent");
        if (ua.Length > 0) { start.ArgumentList.Add("--user-agent"); start.ArgumentList.Add(ua); }
        // The child is a standalone player, not another host to inject the hook into.
        start.Environment.Remove("DOTNET_STARTUP_HOOKS");
        process = Process.Start(start) ?? throw new InvalidOperationException("Player process did not start.");
    }

    internal void CaptureCommand(string action, Dictionary<string, object> data)
    {
        if (action == "HotSwapAbort") { hotSwapToken = 0; return; }
        commands.Enqueue(new PlaybackCommand { Action = action,
            Values = data.Where(p => p.Key != "MediaPlayer").ToDictionary(p => p.Key, p => JsonSerializer.SerializeToElement(p.Value)) });
    }

    private async Task ListenAsync()
    {
        while (true)
        {
            try
            {
                using var pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await pipe.WaitForConnectionAsync().ConfigureAwait(false);
                using var reader = new StreamReader(pipe);
                using var writer = new StreamWriter(pipe) { AutoFlush = true };
                while (await reader.ReadLineAsync().ConfigureAwait(false) is string line)
                {
                    PlaybackReply reply;
                    try
                    {
                        var request = JsonSerializer.Deserialize<PlaybackRequest>(line) ?? throw new InvalidDataException("Empty request");
                        reply = await (await Application.Current.Dispatcher.InvokeAsync(() => HandleAsync(request))).ConfigureAwait(false);
                    }
                    catch (Exception ex) { StartupHook.Log("Bridge request: " + ex); reply = new() { Error = ex.GetBaseException().Message }; }
                    await writer.WriteLineAsync(JsonSerializer.Serialize(reply)).ConfigureAwait(false);
                    if (reply.Closed) break;
                }
            }
            catch (Exception ex) { StartupHook.Log("Bridge connection: " + ex.Message); }
            finally
            {
                // A crash/disconnect saves the last acknowledged sample as well.
                if (Application.Current?.Dispatcher is { HasShutdownStarted: false } dispatcher)
                    await (await dispatcher.InvokeAsync(CloseAsync)).ConfigureAwait(false);
            }
        }
    }

    internal async Task<PlaybackReply> HandleAsync(PlaybackRequest request)
    {
        if (source == null || vm == null || closed) return new() { Closed = closed };
        bool current = request.Revision == revision;
        if (PlaybackValidation.IsCurrentProgress(request, revision))
        {
            position = Math.Min(request.Position, request.Duration);
            duration = request.Duration;
            Send(vm, new() { ["VlcMediaPlayer"] = "TimeChanged", ["CurTime"] = position, ["Duration"] = duration });
            var state = Get(vm, "MediaPlayerSwitch");
            Set(state, "MediaTime", TimeSpan.FromSeconds(position)); // final samples must not be lost to the VM's UI throttle
            Set(state, "MediaLength", TimeSpan.FromSeconds(duration));
            Set(state, "IsPlaying", request.Playing);
            Set(state, "Volume", Convert.ToDouble(request.Volume));
            Set(state, "Mute", request.Muted);
        }
        if (current)
        {
            switch (request.Action)
            {
                case "opened":
                    opened = true;
                    Set(Get(vm, "MediaPlayerSwitch"), "IsBuffering", false);
                    Set(Get(vm, "MediaPlayerSwitch"), "IsStart", true);
                    if (hotSwapToken != 0) Send(vm, new() { ["VlcMediaPlayer"] = "HotSwapReady", ["HotSwapToken"] = hotSwapToken });
                    hotSwapToken = 0;
                    break;
                case "failed":
                    if (hotSwapToken != 0) Send(vm, new() { ["VlcMediaPlayer"] = "HotSwapFailed", ["HotSwapToken"] = hotSwapToken, ["Reason"] = "Custom player could not open this source." });
                    hotSwapToken = 0;
                    break;
                case "ended":
                    if (opened) { await SaveProgressAsync(); Send(vm, new() { ["VlcMediaPlayer"] = "EndReached" }); }
                    break;
                case "episode":
                    if (!episodes.TryGetValue(request.Id, out object? episode)) throw new InvalidOperationException("Episode is no longer available.");
                    await SaveProgressAsync();
                    Send(vm, new() { ["PlaySelectedEpisode"] = episode });
                    break;
                case "next":
                    await SaveProgressAsync();
                    Command(vm, "EpisodeOperationCommand", "Next");
                    break;
                case "quality":
                    if (!qualities.TryGetValue(request.Id, out object? quality)) throw new InvalidOperationException("Quality is no longer available.");
                    Command(Model("VideoQualityLstViewModel"), "ListViewItemMouseDownCommand", quality);
                    break;
                case "subtitle":
                    if (request.Id == "off")
                    {
                        var selected = subtitles.Values.FirstOrDefault(s => Get(s, "IsSelected") is true);
                        if (selected != null) Command(Model("VideoSubtitleLstViewModel"), "ListViewItemMouseDownCommand", selected);
                        else if (Model("SubtitleContentViewModel") is object content) Send(content, new() { ["SubtitleSwitch"] = null });
                    }
                    else if (subtitles.TryGetValue(request.Id, out object? subtitle))
                        Command(Model("VideoSubtitleLstViewModel"), "ListViewItemMouseDownCommand", subtitle);
                    break;
                case "subtitleDelay":
                    Set(Get(Model("SubtitleContentViewModel"), "SubtitleProperty"), "Delay", TimeSpan.FromSeconds(-request.Value));
                    break;
                case "server": await ChangeServerAsync(request.Id); break;
                case "close": await CloseAsync(); return new() { Closed = true, Error = closeError };
            }
        }
        var reply = new PlaybackReply { Source = !current ? source : null };
        while (commands.TryDequeue(out var command)) reply.Commands.Add(command);
        if (request.Metadata || !current) reply.Metadata = Metadata();
        var property = Get(Model("SubtitleContentViewModel"), "SubtitleProperty");
        object? cues = Get(property, "Item") != null ? Get(property, "Content") : null;
        if (!current || !ReferenceEquals(cues, lastCueContent))
        {
            lastCueContent = cues;
            reply.Cues = Items(cues).Select(c => new SubtitleCue { Start = ((TimeSpan?)Get(c, "BeginTime"))?.TotalSeconds ?? 0,
                End = ((TimeSpan?)Get(c, "EndTime"))?.TotalSeconds ?? 0, Text = Text(c, "Sub") }).ToList();
        }
        reply.SubtitleDelay = -(((TimeSpan?)Get(property, "Delay"))?.TotalSeconds ?? 0);
        return reply;
    }

    private async Task LoadMetadataAsync(object? detail, bool isSeries)
    {
        loading = true;
        metadataError = null;
        try
        {
            if (isSeries)
            {
                Type listType = typeof(List<>).MakeGenericType(Type("Resources.Models.EpisodeItem"));
                foreach (object season in Items(Get(detail, "season")))
                {
                    string json = await Api("TV_episodeAsync", Get(detail, "id"), season);
                    if (!ReferenceEquals(detail, seriesDetail)) return;
                    foreach (object episode in Items(Analyze(json, listType))) episodes[EpisodeId(episode)] = episode;
                }
            }
            if (servers.Count == 0)
            {
                object network = Analyze(await Api("Test_network_url_v2Async"), Type("Resources.Models.TestNetworkModel"));
                foreach (object server in Items(Get(network, "our"))) servers[Text(server, "id")] = server;
            }
        }
        catch (Exception ex) { metadataError = ex.GetBaseException().Message; StartupHook.Log("Metadata: " + ex); }
        finally { if (ReferenceEquals(detail, seriesDetail)) loading = false; }
    }

    private PlaybackMetadata Metadata()
    {
        var detail = Get(vm, "MTDetail");
        var episode = Get(vm, "Episode");
        var metadata = new PlaybackMetadata { IsSeries = Number(detail, "box_type") == 2, Loading = loading, Error = metadataError };
        foreach (object item in episodes.Values.OrderBy(e => Number(e, "season")).ThenBy(e => Number(e, "episode")))
            metadata.Episodes.Add(new() { Id = EpisodeId(item), Label = $"E{Text(item, "episode")}  {Text(item, "title")}",
                Group = "Season " + Text(item, "season"), Selected = EpisodeId(item) == EpisodeId(episode) });
        qualities.Clear();
        object? qualityList = Get(Model("VideoQualityLstViewModel"), "QualityLst") ?? Get(vm, "QualityLst");
        foreach (object item in Items(Get(qualityList, "list")))
        {
            string id = System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(item).ToString(); qualities[id] = item;
            string label = Text(item, "quality");
            if (label.Length == 0) label = Text(item, "real_quality");
            metadata.Qualities.Add(new() { Id = id, Label = label + " " + Text(item, "size"), Selected = Get(item, "IsSelected") is true });
        }
        subtitles.Clear();
        object? subtitleModel = Get(Model("VideoSubtitleLstViewModel"), "Subtitle");
        foreach (object language in Items(Get(Get(subtitleModel, "SubtitleLst") ?? Get(subtitleModel, "OnlineSubtitle"), "list")))
            foreach (object item in Items(Get(language, "subtitles")))
            {
                string id = System.Runtime.CompilerServices.RuntimeHelpers.GetHashCode(item).ToString(); subtitles[id] = item;
                metadata.Subtitles.Add(new() { Id = id, Label = Text(item, "file_name"), Group = Text(language, "language"), Selected = Get(item, "IsSelected") is true });
            }
        foreach (var pair in servers)
            metadata.Servers.Add(new() { Id = pair.Key, Label = Text(pair.Value, "description") is { Length: > 0 } name ? name : Text(pair.Value, "country"),
                Selected = pair.Key == Text(Get(Settings, "Server"), "id") });
        return metadata;
    }

    private async Task ChangeServerAsync(string id)
    {
        if (!servers.TryGetValue(id, out object? server)) throw new InvalidOperationException("Server is no longer available.");
        long expectedRevision = revision;
        object target = vm!;
        object? detail = Get(target, "MTDetail"), episode = Get(target, "Episode"), oldServer = Get(Settings, "Server");
        Set(Settings, "Server", server);
        try
        {
            string json = await Api("MovieTV_downloadurl_v4Async", Get(detail, "box_type"), Get(detail, "id"), Get(episode, "season") ?? 0, Get(episode, "episode") ?? 0);
            if (expectedRevision != revision) throw new InvalidOperationException("Playback changed while the server was loading; select the server again.");
            Type type = Type("Resources.Models.QualityLstModel");
            object qualityList = Analyze(json, type);
            type.GetMethod("ParseHLSFromList")?.Invoke(null, new[] { qualityList, json });
            PersistSettings();
            Send(target, new() { ["MediaPlay"] = null, ["MTDetail"] = detail, ["Episode"] = episode,
                ["QualityLst"] = qualityList, ["ResumeSeconds"] = (int)position });
        }
        catch { Set(Settings, "Server", oldServer); throw; }
    }

    private async Task SaveProgressAsync()
    {
        if (position <= 0 || duration <= 0 || Get(Settings, "IsPrivate") is true || vm == null) return;
        object? detail = Get(vm, "MTDetail"), episode = Get(vm, "Episode");
        object? box = Get(detail, "box_type"), id = Get(detail, "id"), fid = Get(Get(vm, "VideoInfo"), "fid") ?? 0;
        object season = Get(episode, "season") ?? 0, number = Get(episode, "episode") ?? 0;
        long seconds = (long)position;
        string[] results = await Task.WhenAll(Api("MovieTV_playAsync", box, id, fid, seconds, season, number),
            Api("MovieTV_play_progressAsync", box, id, fid, seconds, position + 180 > duration ? 1 : 0, season, number));
        foreach (string result in results)
        {
            using var document = JsonDocument.Parse(result);
            if (document.RootElement.TryGetProperty("code", out var code) && code.GetInt32() != 1)
                throw new InvalidOperationException("Progress save was rejected by MovieBoxPro.");
        }
        StartupHook.Log($"Progress saved: revision={revision}, seconds={seconds}.");
    }

    private async Task CloseAsync()
    {
        if (closed || closing || vm == null) return;
        closing = true;
        try
        {
            try { await SaveProgressAsync(); } catch (Exception ex) { closeError = ex.GetBaseException().Message; StartupHook.Log("Final progress save: " + ex); }
            // Original Unloaded also updates local downloads and refreshes history views.
            closed = true;
            foreach (Window window in Application.Current.Windows.Cast<Window>().ToArray())
                if (window.GetType().Name is "PlayerVlcWindow" or "PlayerMpvWindow" or "PlayerFFmpegWindow") window.Close();
        }
        finally { closing = false; }
    }

    private static string EpisodeId(object? item) => $"{Text(item, "season")}:{Text(item, "episode")}";
}
