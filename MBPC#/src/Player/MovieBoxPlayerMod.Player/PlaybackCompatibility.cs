using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using FlyleafLib.MediaPlayer;
using MovieBoxPlayerMod.Bridge;

namespace MovieBoxPlayerMod.Player;

public partial class MainWindow
{
    private BridgeClient? bridge;
    private Task? bridgeWork;
    private PlaybackMetadata? playbackMetadata;
    private readonly Queue<(string Action, string Id, double Value)> bridgeActions = new();
    private List<SubtitleCue> subtitleCues = new();
    private long playbackRevision;
    private double resumeSeconds, subtitleDelay;
    private bool mediaReady, endedSent, bridgeClosing, bridgeCloseFinished, resumePlaying = true, localSubtitles;
    private bool playbackMenuOpen;
    private DateTime lastMetadataRequest;
    private string selectedEpisodeGroup = string.Empty;
    private Button? activeSeasonMenuButton;
    private Button? activePlaybackMenuButton;
    private string activePlaybackSection = string.Empty;
    private readonly PlaybackPreferenceResolver preferenceResolver = new();
    private ProfileSource activeProfileSource = ProfileSource.Unknown;
    private PlayerSettings? activeEpisodeSettings;
    private EpisodePreferences activeEpisodePreferences = new();
    private LauncherPlayerDefaults activeLauncherDefaults = LauncherPlayerDefaults.BuiltIn;
    private string activeSettingsUri = string.Empty, activeSettingsTitle = string.Empty;
    private short activeSettingsSeason, activeSettingsEpisode;
    private byte activeSettingsBoxType;
    private int activeAudioTrackIndex = -1;
    private bool activeProfileDirty, pendingBaseProfileApply;

    private bool IsEpisodesMenuOpen => EpisodesMenuOverlay.Visibility == Visibility.Visible;
    private bool IsSeasonMenuOpen => SeasonMenuOverlay.Visibility == Visibility.Visible;
    private bool IsPlaybackMenuOpen => PlaybackMenuOverlay.Visibility == Visibility.Visible;
    private bool IsPlaybackSubmenuOpen => PlaybackSubmenuOverlay.Visibility == Visibility.Visible;

    private async Task ConnectBridgeAsync()
    {
        ShowStatus("Connecting to MovieBoxPro…", true);
        try
        {
            var connection = new BridgeClient(App.BridgeName!);
            await connection.ConnectAsync();
            bridge = connection;
            TickBridge();
        }
        catch (Exception ex) { ShowStatus("Cannot connect to MovieBoxPro: " + ex.Message, false); }
    }

    private PlaybackRequest Sample(string action, string id = "", double value = 0) => new()
    {
        Action = action, Id = id, Value = value, Revision = playbackRevision,
        Position = player.CurTime / (double)TimeSpan.TicksPerSecond,
        Duration = player.Duration / (double)TimeSpan.TicksPerSecond,
        Playing = player.Status == Status.Playing, Ready = mediaReady,
        Volume = player.Audio.Volume, Muted = player.Audio.Mute
    };

    private void QueueBridgeAction(string action, string id = "", double value = 0)
    {
        if (bridge != null && !bridgeClosing) bridgeActions.Enqueue((action, id, value));
    }

    private void TickBridge()
    {
        if (bridge == null || bridgeClosing || bridgeWork is { IsCompleted: false }) return;
        if (mediaReady && player.Status == Status.Ended && !endedSent)
        {
            endedSent = true;
            QueueBridgeAction("ended");
        }
        var action = bridgeActions.TryDequeue(out var next) ? next : ("poll", "", 0.0);
        var request = Sample(action.Item1, action.Item2, action.Item3);
        request.Metadata = DateTime.UtcNow - lastMetadataRequest > TimeSpan.FromSeconds(2);
        if (request.Metadata) lastMetadataRequest = DateTime.UtcNow;
        bridgeWork = ExchangeAsync(request);
    }

    private async Task ExchangeAsync(PlaybackRequest request)
    {
        try { ApplyReply(await bridge!.SendAsync(request)); }
        catch (Exception ex)
        {
            ShowStatus("MovieBoxPro connection lost; progress cannot sync. " + ex.Message, false);
            bridge?.Dispose();
            bridge = null;
        }
    }

    private static string ResolveSettingsTitle(string? settingsTitle, string? title, string uri)
    {
        string value = (settingsTitle ?? string.Empty).Trim();
        if (value.Length == 0) value = (title ?? string.Empty).Trim();
        if (value.Length == 0)
        {
            try
            {
                if (Uri.TryCreate(uri, UriKind.Absolute, out Uri? parsed))
                    value = Path.GetFileNameWithoutExtension(Uri.UnescapeDataString(parsed.IsFile ? parsed.LocalPath : parsed.AbsolutePath));
                else
                    value = Path.GetFileNameWithoutExtension(uri);
            }
            catch { }
        }
        return string.IsNullOrWhiteSpace(value) ? "video" : value.Trim();
    }

    private static EpisodePreferences ClonePreferences(EpisodePreferences p) => new()
    {
        SubtitlePresent = p.SubtitlePresent, SubtitleOff = p.SubtitleOff, SubtitleLanguage = p.SubtitleLanguage,
        QualityPresent = p.QualityPresent, QualityAuto = p.QualityAuto, TargetQualityHeight = p.TargetQualityHeight,
        ServerPresent = p.ServerPresent, ServerAuto = p.ServerAuto, ServerPreference = p.ServerPreference
    };

    private static EpisodePreferences PreferencesFromLauncher(LauncherPlayerDefaults d) => new()
    {
        SubtitlePresent = true, SubtitleOff = d.SubtitleOff, SubtitleLanguage = d.SubtitleLanguage,
        QualityPresent = true, QualityAuto = d.QualityAuto, TargetQualityHeight = d.TargetQualityHeight,
        ServerPresent = true, ServerAuto = d.ServerAuto, ServerPreference = d.ServerPreference
    };

    private void ResolveActiveProfile(PlaybackSource source)
    {
        activeSettingsUri = source.Uri ?? string.Empty;
        activeSettingsTitle = ResolveSettingsTitle(source.SettingsTitle, source.Title, activeSettingsUri);
        activeSettingsSeason = source.Season;
        activeSettingsEpisode = source.Episode;
        activeSettingsBoxType = source.BoxType;
        activeEpisodeSettings = PlayerSettings.Load(activeSettingsUri, activeSettingsTitle, activeSettingsSeason, activeSettingsEpisode, activeSettingsBoxType);
        activeProfileDirty = false;
        pendingBaseProfileApply = true;
        if (activeEpisodeSettings != null)
        {
            activeProfileSource = ProfileSource.EpisodeCock;
            activeEpisodePreferences = ClonePreferences(activeEpisodeSettings.Preferences);
            activeAudioTrackIndex = activeEpisodeSettings.AudioTrackIndex;
            subtitleDelay = activeEpisodeSettings.SubtitleDelay;
            preferenceResolver.ResetEpisode(activeEpisodePreferences);
            QueueBridgeAction("subtitleDelay", value: activeEpisodeSettings.SubtitleDelay);
        }
        else
        {
            activeProfileSource = ProfileSource.LauncherDefaults;
            activeLauncherDefaults = LauncherDefaults.Load();
            activeEpisodePreferences = PreferencesFromLauncher(activeLauncherDefaults);
            activeAudioTrackIndex = -1;
            preferenceResolver.ResetLauncherDefaults(activeLauncherDefaults);
        }
    }

    private void ApplyPendingBaseProfileSettings()
    {
        if (!pendingBaseProfileApply) return;
        pendingBaseProfileApply = false;
        if (activeProfileSource == ProfileSource.EpisodeCock && activeEpisodeSettings != null)
        {
            player.Audio.Volume = Math.Clamp(activeEpisodeSettings.Volume, (short)0, (short)100);
            player.Audio.Mute = activeEpisodeSettings.Muted;
            player.Speed = Math.Clamp(activeEpisodeSettings.PlaybackSpeed, 0.25f, 4.0f);
            subtitleDelay = activeEpisodeSettings.SubtitleDelay;
            if (activeAudioTrackIndex >= 0)
            {
                var stream = player.Audio.Streams.FirstOrDefault(item => item.StreamIndex == activeAudioTrackIndex);
                if (stream != null) player.Open(stream, resync: true, defaultAudio: false);
            }
        }
        else if (activeProfileSource == ProfileSource.LauncherDefaults)
        {
            player.Audio.Volume = Math.Clamp((int)activeLauncherDefaults.Volume, 0, 100);
        }
    }

    private void MarkEpisodeSettingsDirty()
    {
        if (!string.IsNullOrWhiteSpace(activeSettingsUri)) activeProfileDirty = true;
    }

    private void FlushActivePlayerSettings()
    {
        if (string.IsNullOrWhiteSpace(activeSettingsUri) || string.IsNullOrWhiteSpace(activeSettingsTitle)) return;
        if (!activeProfileDirty && activeEpisodeSettings != null) return;
        try
        {
            var snapshot = new PlayerSettings
            {
                Title = activeSettingsTitle, Season = activeSettingsSeason, Episode = activeSettingsEpisode, BoxType = activeSettingsBoxType,
                Muted = player.Audio.Mute, Volume = (short)Math.Clamp(player.Audio.Volume, 0, 100), PlaybackSpeed = (float)Math.Clamp(player.Speed, 0.25, 4.0),
                SubtitleDelay = (float)Math.Clamp(subtitleDelay, -30.0, 30.0), AudioTrackIndex = activeAudioTrackIndex,
                Preferences = ClonePreferences(activeEpisodePreferences)
            };
            PlayerSettings.Save(activeSettingsUri, snapshot);
            activeEpisodeSettings = snapshot;
            activeProfileDirty = false;
        }
        catch { /* Playback and close must survive a settings write failure. */ }
    }

    private static PreferenceChoice ToPreferenceChoice(PlaybackChoice c) => new(c.Id ?? string.Empty, c.Label ?? string.Empty, c.Selected);

    private void SettlePendingPreferences(PlaybackMetadata metadata)
    {
        foreach (PreferenceDecision decision in preferenceResolver.Settle(
            metadata.Subtitles.Select(ToPreferenceChoice), metadata.Qualities.Select(ToPreferenceChoice), metadata.Servers.Select(ToPreferenceChoice)))
        {
            CaptureAppliedPreferenceDecision(decision, metadata);
            QueueBridgeAction(decision.Action, decision.Id);
        }
    }

    private void CaptureAppliedPreferenceDecision(PreferenceDecision decision, PlaybackMetadata metadata)
    {
        if (decision.Action == "subtitle")
        {
            if (decision.Id == "off") { CaptureSubtitleOffPreference(); return; }
            foreach (PlaybackChoice choice in metadata.Subtitles)
                if (string.Equals(choice.Id, decision.Id, StringComparison.Ordinal)) { CaptureBridgePreference("subtitle", choice); return; }
        }
        else if (decision.Action == "quality")
        {
            foreach (PlaybackChoice choice in metadata.Qualities)
                if (string.Equals(choice.Id, decision.Id, StringComparison.Ordinal)) { CaptureBridgePreference("quality", choice); return; }
        }
        else if (decision.Action == "server")
        {
            foreach (PlaybackChoice choice in metadata.Servers)
                if (string.Equals(choice.Id, decision.Id, StringComparison.Ordinal)) { CaptureBridgePreference("server", choice); return; }
        }
    }

    private void CaptureSubtitleOffPreference()
    {
        activeEpisodePreferences.SubtitlePresent = true; activeEpisodePreferences.SubtitleOff = true; activeEpisodePreferences.SubtitleLanguage = string.Empty; MarkEpisodeSettingsDirty();
    }

    private void CaptureBridgePreference(string action, PlaybackChoice choice)
    {
        PreferenceChoice semantic = ToPreferenceChoice(choice);
        bool autoChoice = string.Equals(choice.Id?.Trim(), "auto", StringComparison.OrdinalIgnoreCase) ||
                          string.Equals(choice.Label?.Trim(), "auto", StringComparison.OrdinalIgnoreCase);
        if (action == "quality")
        {
            activeEpisodePreferences.QualityPresent = true;
            if (autoChoice)
            {
                activeEpisodePreferences.QualityAuto = true;
                activeEpisodePreferences.TargetQualityHeight = 0;
                MarkEpisodeSettingsDirty();
                return;
            }
            if (PlaybackPreferenceResolver.TryParseQuality(semantic, out int height))
            {
                activeEpisodePreferences.QualityAuto = false;
                activeEpisodePreferences.TargetQualityHeight = (ushort)height;
                MarkEpisodeSettingsDirty();
            }
            else
            {
                activeEpisodePreferences.QualityPresent = false;
            }
        }
        else if (action == "server")
        {
            activeEpisodePreferences.ServerPresent = true;
            if (autoChoice)
            {
                activeEpisodePreferences.ServerAuto = true;
                activeEpisodePreferences.ServerPreference = string.Empty;
            }
            else
            {
                activeEpisodePreferences.ServerAuto = false;
                activeEpisodePreferences.ServerPreference = string.IsNullOrWhiteSpace(choice.Id) ? (choice.Label ?? string.Empty).Trim() : choice.Id.Trim();
                if (activeEpisodePreferences.ServerPreference.Length == 0) activeEpisodePreferences.ServerPresent = false;
            }
            MarkEpisodeSettingsDirty();
        }
        else if (action == "subtitle")
        {
            string language = PlaybackPreferenceResolver.CanonicalLanguageForChoice(semantic);
            if (language.Length > 0)
            {
                activeEpisodePreferences.SubtitlePresent = true;
                activeEpisodePreferences.SubtitleOff = false;
                activeEpisodePreferences.SubtitleLanguage = language;
                MarkEpisodeSettingsDirty();
            }
        }
    }

    private void ApplyReply(PlaybackReply reply)
    {
        if (reply.Error != null) { ShowStatus(reply.Error, false); return; }
        if (reply.Closed) { bridgeCloseFinished = true; Close(); return; }
        if (reply.Source is { } source && source.Revision != playbackRevision)
        {
            FlushActivePlayerSettings();
            playbackRevision = source.Revision;
            bridgeActions.Clear();
            mediaReady = false;
            endedSent = false;
            localSubtitles = false;
            subtitleCues.Clear();
            SubtitleText.Text = "";
            resumeSeconds = source.Seconds;
            resumePlaying = source.Playing;
            TitleText.Text = Title = source.Title;
            SourceText.Text = GetSourceLabel(source.Uri);
            ShowStatus("Opening video…", true);
            ResolveActiveProfile(source);
            player.Speed = activeProfileSource == ProfileSource.EpisodeCock && activeEpisodeSettings != null
                ? Math.Clamp(activeEpisodeSettings.PlaybackSpeed, 0.25f, 4.0f)
                : Math.Clamp(source.Rate, 0.25, 4);
            player.Stop();
            player.OpenAsync(source.Uri);
        }
        if (reply.Metadata is { } metadata)
        {
            playbackMetadata = metadata;
            EpisodesButton.Visibility = metadata.IsSeries ? Visibility.Visible : Visibility.Collapsed;
            if (!metadata.IsSeries && IsEpisodesMenuOpen)
                CloseEpisodesMenuOverlay();
            else if (IsEpisodesMenuOpen)
                BuildEpisodesMenu();
            SettlePendingPreferences(metadata);
        }
        if (reply.Cues != null && !localSubtitles) subtitleCues = reply.Cues.OrderBy(c => c.Start).ToList();
        if (!localSubtitles && activeProfileSource != ProfileSource.EpisodeCock) subtitleDelay = reply.SubtitleDelay;
        foreach (var command in reply.Commands) ApplyPlaybackCommand(command);
    }

    private void ApplyPlaybackCommand(PlaybackCommand command)
    {
        double Number(string key, double fallback = 0) => command.Values.TryGetValue(key, out JsonElement value)
            && double.TryParse(value.ToString(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out double number) ? number : fallback;
        switch (command.Action)
        {
            case "Close": Close(); break;
            case "Pause":
                if (command.Values.TryGetValue("Pause", out var pause)) { if (pause.GetBoolean()) player.Pause(); else player.Play(); }
                else player.TogglePlayPause();
                break;
            case "Time":
                if (command.Values.TryGetValue("Time", out var time) && TimeSpan.TryParse(time.ToString(), out var span))
                    SeekToMilliseconds((long)span.TotalMilliseconds, false);
                else if (command.Values.ContainsKey("Percent")) SeekToMilliseconds((long)(player.Duration / 10000.0 * Number("Percent")), false);
                else if (command.Values.ContainsKey("Forward")) SeekToMilliseconds(player.CurTime / 10000 + SeekStepMilliseconds, false);
                else if (command.Values.ContainsKey("Backward")) SeekToMilliseconds(player.CurTime / 10000 - SeekStepMilliseconds, false);
                break;
            case "Volume": player.Audio.Volume = (int)Number("Volume", player.Audio.Volume); MarkEpisodeSettingsDirty(); break;
            case "Mute": player.Audio.Mute = command.Values.TryGetValue("Mute", out var mute) ? mute.GetBoolean() : !player.Audio.Mute; MarkEpisodeSettingsDirty(); break;
            case "Play": player.Speed = Math.Clamp(Number("rate", 1), 0.25, 4); MarkEpisodeSettingsDirty(); break;
            case "AudioDelay": player.Config.Audio.Delay = (long)(Number("AudioDelay") * TimeSpan.TicksPerSecond); break;
            case "Stop": player.Stop(); break;
        }
    }

    private async Task CloseBridgeAsync()
    {
        try
        {
            if (bridgeWork != null) await bridgeWork;
            if (bridge != null)
            {
                var reply = await bridge.SendAsync(Sample("close"));
                if (reply.Error != null) throw new IOException(reply.Error);
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "The final watch-time save could not be confirmed.\n" + ex.Message,
                "MovieBox Player", MessageBoxButton.OK, MessageBoxImage.Warning);
        }
    }

    private void RenderSubtitles()
    {
        double seconds = player.CurTime / (double)TimeSpan.TicksPerSecond - subtitleDelay;
        string text = SubtitleTimeline.TextAt(subtitleCues, seconds);
        if (SubtitleText.Text != text) SubtitleText.Text = text;
    }

    private Button AddPlaybackOverlayItem(StackPanel panel, string title, Action? click = null, bool selected = false, bool enabled = true)
    {
        var button = new Button
        {
            Content = title,
            Style = (Style)PlayerMenusLayer.FindResource("EpisodeMenuChoiceStyle"),
            IsEnabled = enabled,
            Tag = selected ? "Selected" : null
        };

        if (click != null && enabled)
        {
            button.Click += (_, e) =>
            {
                e.Handled = true;
                click();
            };
        }

        panel.Children.Add(button);
        return button;
    }

    private void AddPlaybackSection(string title, string section)
    {
        Button? sectionButton = null;
        sectionButton = AddPlaybackOverlayItem(PlaybackMenuPanel, title + "  ›", () =>
        {
            if (sectionButton != null)
                ShowPlaybackSubmenu(section, title, sectionButton);
        }, selected: activePlaybackSection == section);
        sectionButton.MouseEnter += (_, _) => ShowPlaybackSubmenu(section, title, sectionButton);
    }

    private void AddBridgeChoicesToPlaybackSubmenu(string action, IEnumerable<PlaybackChoice> choices)
    {
        var grouped = choices.GroupBy(choice => choice.Group ?? string.Empty).ToList();
        if (grouped.Count == 0)
        {
            AddPlaybackOverlayItem(PlaybackSubmenuPanel, "No options available.", enabled: false);
            return;
        }

        foreach (var group in grouped)
        {
            if (!string.IsNullOrWhiteSpace(group.Key))
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, group.Key, enabled: false);

            foreach (var choice in group)
            {
                PlaybackChoice selectedChoice = choice;
                AddPlaybackOverlayItem(PlaybackSubmenuPanel,
                    string.IsNullOrWhiteSpace(selectedChoice.Label) ? selectedChoice.Id : selectedChoice.Label,
                    () =>
                    {
                        localSubtitles = false;
                        ClosePlaybackMenuOverlay();
                        CaptureBridgePreference(action, selectedChoice);
                        QueueBridgeAction(action, selectedChoice.Id);
                    },
                    selectedChoice.Selected);
            }
        }
    }

    private Button AddEpisodeOverlayItem(StackPanel panel, string title, Action? click = null, bool selected = false, bool enabled = true)
    {
        var button = new Button
        {
            Content = title,
            Style = (Style)PlayerMenusLayer.FindResource("EpisodeMenuChoiceStyle"),
            IsEnabled = enabled,
            Tag = selected ? "Selected" : null
        };

        if (click != null && enabled)
        {
            button.Click += (_, e) =>
            {
                e.Handled = true;
                click();
            };
        }

        panel.Children.Add(button);
        return button;
    }

    private static string EpisodeGroupLabel(PlaybackChoice choice) =>
        string.IsNullOrWhiteSpace(choice.Group) ? "Episodes" : choice.Group.Trim();

    private void BuildEpisodesMenu()
    {
        EpisodesMenuPanel.Children.Clear();
        SeasonEpisodesMenuPanel.Children.Clear();
        SeasonMenuOverlay.Visibility = Visibility.Collapsed;
        activeSeasonMenuButton = null;

        if (playbackMetadata == null)
        {
            AddEpisodeOverlayItem(EpisodesMenuPanel, "Episode metadata is not available yet.", enabled: false);
            return;
        }

        AddEpisodeOverlayItem(EpisodesMenuPanel, "Next episode", () =>
        {
            CloseEpisodesMenuOverlay();
            QueueBridgeAction("next");
        });

        var groups = playbackMetadata.Episodes
            .GroupBy(EpisodeGroupLabel)
            .ToList();

        if (groups.Count == 0)
        {
            AddEpisodeOverlayItem(EpisodesMenuPanel,
                playbackMetadata.Loading ? "Loading episodes…" : "No episodes available.", enabled: false);
        }
        else
        {
            string? selectedGroup = groups.FirstOrDefault(group => group.Any(choice => choice.Selected))?.Key;
            if (string.IsNullOrWhiteSpace(selectedEpisodeGroup) || groups.All(group => group.Key != selectedEpisodeGroup))
                selectedEpisodeGroup = selectedGroup ?? groups[0].Key;

            foreach (var group in groups)
            {
                string groupKey = group.Key;
                Button? seasonButton = null;
                seasonButton = AddEpisodeOverlayItem(EpisodesMenuPanel, groupKey + "  ›", () =>
                {
                    if (seasonButton != null)
                        ShowSeasonMenu(groupKey, seasonButton);
                }, selected: groupKey == selectedGroup);
                seasonButton.MouseEnter += (_, _) => ShowSeasonMenu(groupKey, seasonButton);
            }
        }

        if (playbackMetadata.Loading)
            AddEpisodeOverlayItem(EpisodesMenuPanel, "Loading remaining seasons…", enabled: false);
        if (playbackMetadata.Error != null)
            AddEpisodeOverlayItem(EpisodesMenuPanel, playbackMetadata.Error, enabled: false);
    }

    private void ShowSeasonMenu(string groupKey, Button anchorButton)
    {
        if (playbackMetadata == null)
            return;

        var episodes = playbackMetadata.Episodes
            .Where(choice => EpisodeGroupLabel(choice) == groupKey)
            .ToList();
        if (episodes.Count == 0)
            return;

        selectedEpisodeGroup = groupKey;
        activeSeasonMenuButton = anchorButton;
        SeasonMenuTitle.Text = groupKey;
        SeasonEpisodesMenuPanel.Children.Clear();

        foreach (var choice in episodes)
        {
            PlaybackChoice selectedChoice = choice;
            AddEpisodeOverlayItem(SeasonEpisodesMenuPanel,
                string.IsNullOrWhiteSpace(selectedChoice.Label) ? selectedChoice.Id : selectedChoice.Label,
                () =>
                {
                    localSubtitles = false;
                    CloseEpisodesMenuOverlay();
                    QueueBridgeAction("episode", selectedChoice.Id);
                },
                selectedChoice.Selected);
        }

        SeasonMenuOverlay.Visibility = Visibility.Visible;
        SeasonMenuOverlay.UpdateLayout();
        PositionSeasonMenuOverlay(anchorButton);
    }

    private void PositionEpisodesMenuOverlay()
    {
        if (!IsEpisodesMenuOpen || OverlayRoot.ActualWidth <= 0 || OverlayRoot.ActualHeight <= 0)
            return;

        EpisodesMenuOverlay.UpdateLayout();
        Point anchor = EpisodesButton.TransformToAncestor(OverlayRoot).Transform(new Point(0, 0));
        double width = EpisodesMenuOverlay.ActualWidth > 0 ? EpisodesMenuOverlay.ActualWidth : 260;
        double height = EpisodesMenuOverlay.ActualHeight > 0 ? EpisodesMenuOverlay.ActualHeight : EpisodesMenuOverlay.DesiredSize.Height;
        double left = anchor.X + EpisodesButton.ActualWidth * 0.5 - width * 0.5;
        double top = anchor.Y - height - 10;

        left = Math.Clamp(left, 12, Math.Max(12, OverlayRoot.ActualWidth - width - 12));
        top = Math.Clamp(top, 12, Math.Max(12, OverlayRoot.ActualHeight - height - 12));
        Canvas.SetLeft(EpisodesMenuOverlay, left);
        Canvas.SetTop(EpisodesMenuOverlay, top);
    }

    private void PositionSeasonMenuOverlay(FrameworkElement anchorButton)
    {
        if (!IsSeasonMenuOpen || !IsEpisodesMenuOpen)
            return;

        SeasonMenuOverlay.UpdateLayout();
        double parentLeft = Canvas.GetLeft(EpisodesMenuOverlay);
        if (double.IsNaN(parentLeft)) parentLeft = 12;
        double parentWidth = EpisodesMenuOverlay.ActualWidth > 0 ? EpisodesMenuOverlay.ActualWidth : 260;
        double submenuWidth = SeasonMenuOverlay.ActualWidth > 0 ? SeasonMenuOverlay.ActualWidth : 300;
        double submenuHeight = SeasonMenuOverlay.ActualHeight > 0 ? SeasonMenuOverlay.ActualHeight : SeasonMenuOverlay.DesiredSize.Height;
        Point anchor = anchorButton.TransformToAncestor(OverlayRoot).Transform(new Point(0, 0));

        double rightSide = parentLeft + parentWidth + 8;
        double leftSide = parentLeft - submenuWidth - 8;
        double left = rightSide + submenuWidth <= OverlayRoot.ActualWidth - 12 ? rightSide : Math.Max(12, leftSide);
        double top = Math.Clamp(anchor.Y - 8, 12, Math.Max(12, OverlayRoot.ActualHeight - submenuHeight - 12));

        Canvas.SetLeft(SeasonMenuOverlay, left);
        Canvas.SetTop(SeasonMenuOverlay, top);
    }

    private void OpenEpisodesMenuOverlay()
    {
        if (IsPlaybackMenuOpen)
            ClosePlaybackMenuOverlay(restartHideTimer: false);

        BuildEpisodesMenu();
        EpisodesMenuOverlay.Visibility = Visibility.Visible;
        SeasonMenuOverlay.Visibility = Visibility.Collapsed;
        EpisodesButton.Tag = "Open";
        playbackMenuOpen = true;
        hideTimer.Stop();
        ShowControls();
        ShowCustomCursor();
        EpisodesMenuOverlay.UpdateLayout();
        PositionEpisodesMenuOverlay();
    }

    private void CloseEpisodesMenuOverlay(bool restartHideTimer = true)
    {
        if (!IsEpisodesMenuOpen)
            return;

        SeasonMenuOverlay.Visibility = Visibility.Collapsed;
        EpisodesMenuOverlay.Visibility = Visibility.Collapsed;
        activeSeasonMenuButton = null;
        EpisodesButton.Tag = null;
        playbackMenuOpen = IsPlaybackMenuOpen;
        if (restartHideTimer && !playbackMenuOpen)
            RestartHideTimer();
    }

    private void EpisodesMenuClose_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        CloseEpisodesMenuOverlay();
    }

    private void Episodes_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        if (IsEpisodesMenuOpen)
        {
            CloseEpisodesMenuOverlay();
            return;
        }

        OpenEpisodesMenuOverlay();
    }

    private void BuildPlaybackMenu()
    {
        PlaybackMenuPanel.Children.Clear();
        PlaybackSubmenuPanel.Children.Clear();
        PlaybackSubmenuOverlay.Visibility = Visibility.Collapsed;
        activePlaybackMenuButton = null;
        activePlaybackSection = string.Empty;

        AddPlaybackSection("Player engine", "engine");
        AddPlaybackSection("Playback speed", "speed");
        AddPlaybackSection("Audio track", "audio");
        AddPlaybackSection("Subtitles", "subtitles");
        AddPlaybackSection("Subtitle delay", "subtitleDelay");
        AddPlaybackSection("Quality", "quality");
        AddPlaybackSection("Server", "server");
    }

    private void ShowPlaybackSubmenu(string section, string title, Button anchorButton)
    {
        activePlaybackSection = section;
        activePlaybackMenuButton = anchorButton;
        PlaybackSubmenuTitle.Text = title;
        PlaybackSubmenuPanel.Children.Clear();

        foreach (Button button in PlaybackMenuPanel.Children.OfType<Button>())
            button.Tag = ReferenceEquals(button, anchorButton) ? "Selected" : null;

        switch (section)
        {
            case "engine":
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "FFmpeg (Default)", selected: true, enabled: false);
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "VLC — coming soon", enabled: false);
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "MPV — coming soon", enabled: false);
                break;

            case "speed":
                foreach (double rate in new[] { 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0 })
                {
                    double selectedRate = rate;
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, rate + "×", () =>
                    {
                        player.Speed = selectedRate;
                        MarkEpisodeSettingsDirty();
                        ClosePlaybackMenuOverlay();
                    }, Math.Abs(player.Speed - rate) < 0.01);
                }
                break;

            case "audio":
                var audioStreams = player.Audio.Streams.ToList();
                if (audioStreams.Count == 0)
                {
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, "No audio tracks available.", enabled: false);
                    break;
                }
                foreach (var stream in audioStreams)
                {
                    var selectedStream = stream;
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel,
                        stream.Language + " · " + stream.Codec + " · " + stream.StreamIndex,
                        () =>
                        {
                            player.Open(selectedStream, resync: true, defaultAudio: false);
                            activeAudioTrackIndex = selectedStream.StreamIndex;
                            MarkEpisodeSettingsDirty();
                            ClosePlaybackMenuOverlay();
                        });
                }
                break;

            case "subtitles":
                bool bridgeSubtitleSelected = playbackMetadata?.Subtitles.Any(choice => choice.Selected) == true;
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Off", () =>
                {
                    subtitleCues.Clear();
                    localSubtitles = true;
                    player.Config.Subtitles.Enabled = false;
                    ClosePlaybackMenuOverlay();
                    CaptureSubtitleOffPreference();
                    QueueBridgeAction("subtitle", "off");
                }, selected: !bridgeSubtitleSelected && !localSubtitles);
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Open subtitle file…", () =>
                {
                    ClosePlaybackMenuOverlay();
                    OpenSubtitleFile();
                }, selected: localSubtitles);
                if (playbackMetadata != null)
                    AddBridgeChoicesToPlaybackSubmenu("subtitle", playbackMetadata.Subtitles);
                break;

            case "subtitleDelay":
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Earlier by 0.5s", () =>
                {
                    SetSubtitleDelay(subtitleDelay - 0.5);
                    ClosePlaybackMenuOverlay();
                });
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Later by 0.5s", () =>
                {
                    SetSubtitleDelay(subtitleDelay + 0.5);
                    ClosePlaybackMenuOverlay();
                });
                AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Reset", () =>
                {
                    SetSubtitleDelay(0);
                    ClosePlaybackMenuOverlay();
                }, Math.Abs(subtitleDelay) < 0.001);
                break;

            case "quality":
                if (playbackMetadata == null)
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Quality metadata is not available yet.", enabled: false);
                else
                    AddBridgeChoicesToPlaybackSubmenu("quality", playbackMetadata.Qualities);
                break;

            case "server":
                if (playbackMetadata == null)
                {
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Server metadata is not available yet.", enabled: false);
                    break;
                }
                AddBridgeChoicesToPlaybackSubmenu("server", playbackMetadata.Servers);
                if (playbackMetadata.Loading)
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, "Loading…", enabled: false);
                if (playbackMetadata.Error != null)
                    AddPlaybackOverlayItem(PlaybackSubmenuPanel, playbackMetadata.Error, enabled: false);
                break;
        }

        PlaybackSubmenuOverlay.Visibility = Visibility.Visible;
        PlaybackSubmenuOverlay.UpdateLayout();
        PositionPlaybackSubmenuOverlay(anchorButton);
    }

    private void PositionPlaybackMenuOverlay()
    {
        if (!IsPlaybackMenuOpen || OverlayRoot.ActualWidth <= 0 || OverlayRoot.ActualHeight <= 0)
            return;

        PlaybackMenuOverlay.UpdateLayout();
        Point anchor = PlaybackButton.TransformToAncestor(OverlayRoot).Transform(new Point(0, 0));
        double width = PlaybackMenuOverlay.ActualWidth > 0 ? PlaybackMenuOverlay.ActualWidth : 270;
        double height = PlaybackMenuOverlay.ActualHeight > 0 ? PlaybackMenuOverlay.ActualHeight : PlaybackMenuOverlay.DesiredSize.Height;
        double left = anchor.X + PlaybackButton.ActualWidth * 0.5 - width * 0.5;
        double top = anchor.Y - height - 10;

        left = Math.Clamp(left, 12, Math.Max(12, OverlayRoot.ActualWidth - width - 12));
        top = Math.Clamp(top, 12, Math.Max(12, OverlayRoot.ActualHeight - height - 12));
        Canvas.SetLeft(PlaybackMenuOverlay, left);
        Canvas.SetTop(PlaybackMenuOverlay, top);
    }

    private void PositionPlaybackSubmenuOverlay(FrameworkElement anchorButton)
    {
        if (!IsPlaybackSubmenuOpen || !IsPlaybackMenuOpen)
            return;

        PlaybackSubmenuOverlay.UpdateLayout();
        double parentLeft = Canvas.GetLeft(PlaybackMenuOverlay);
        if (double.IsNaN(parentLeft)) parentLeft = 12;
        double parentWidth = PlaybackMenuOverlay.ActualWidth > 0 ? PlaybackMenuOverlay.ActualWidth : 270;
        double submenuWidth = PlaybackSubmenuOverlay.ActualWidth > 0 ? PlaybackSubmenuOverlay.ActualWidth : 310;
        double submenuHeight = PlaybackSubmenuOverlay.ActualHeight > 0 ? PlaybackSubmenuOverlay.ActualHeight : PlaybackSubmenuOverlay.DesiredSize.Height;
        Point anchor = anchorButton.TransformToAncestor(OverlayRoot).Transform(new Point(0, 0));

        double rightSide = parentLeft + parentWidth + 8;
        double leftSide = parentLeft - submenuWidth - 8;
        double left = rightSide + submenuWidth <= OverlayRoot.ActualWidth - 12 ? rightSide : Math.Max(12, leftSide);
        double top = Math.Clamp(anchor.Y - 8, 12, Math.Max(12, OverlayRoot.ActualHeight - submenuHeight - 12));

        Canvas.SetLeft(PlaybackSubmenuOverlay, left);
        Canvas.SetTop(PlaybackSubmenuOverlay, top);
    }

    private void OpenPlaybackMenuOverlay()
    {
        if (IsEpisodesMenuOpen)
            CloseEpisodesMenuOverlay(restartHideTimer: false);

        BuildPlaybackMenu();
        PlaybackMenuOverlay.Visibility = Visibility.Visible;
        PlaybackSubmenuOverlay.Visibility = Visibility.Collapsed;
        PlaybackButton.Tag = "Open";
        playbackMenuOpen = true;
        hideTimer.Stop();
        ShowControls();
        ShowCustomCursor();
        PlaybackMenuOverlay.UpdateLayout();
        PositionPlaybackMenuOverlay();
    }

    private void ClosePlaybackMenuOverlay(bool restartHideTimer = true)
    {
        if (!IsPlaybackMenuOpen)
            return;

        PlaybackSubmenuOverlay.Visibility = Visibility.Collapsed;
        PlaybackMenuOverlay.Visibility = Visibility.Collapsed;
        activePlaybackMenuButton = null;
        activePlaybackSection = string.Empty;
        PlaybackButton.Tag = null;
        playbackMenuOpen = IsEpisodesMenuOpen;
        if (restartHideTimer && !playbackMenuOpen)
            RestartHideTimer();
    }

    private void PlaybackMenuClose_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        ClosePlaybackMenuOverlay();
    }

    private void PlaybackMenu_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        if (IsPlaybackMenuOpen)
        {
            ClosePlaybackMenuOverlay();
            return;
        }

        OpenPlaybackMenuOverlay();
    }

    private void SetSubtitleDelay(double value)
    {
        subtitleDelay = value;
        MarkEpisodeSettingsDirty();
        QueueBridgeAction("subtitleDelay", value: value);
    }

    private void OpenSubtitleFile()
    {
        // The in-player Playback menu owns the custom cursor. Native dialogs do not,
        // so temporarily restore the system cursor while the file picker is active.
        RestoreSystemCursor();
        try
        {
            var dialog = new Microsoft.Win32.OpenFileDialog { Filter = "Subtitles (*.srt;*.vtt)|*.srt;*.vtt|All files|*.*" };
            if (dialog.ShowDialog(this) != true)
                return;

            try
            {
                subtitleCues = SubtitleTimeline.Parse(File.ReadAllText(dialog.FileName));
                localSubtitles = true;
                player.Config.Subtitles.Enabled = false;
                QueueBridgeAction("subtitle", "off");
            }
            catch (Exception ex) { ShowStatus("Could not load subtitles: " + ex.Message, false); }
        }
        finally
        {
            if (IsActive && pointerInside)
            {
                HideSystemCursor();
                ShowCustomCursor();
                RestartHideTimer();
            }
        }
    }
}
