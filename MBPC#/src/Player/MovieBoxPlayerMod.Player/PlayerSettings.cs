using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;

namespace MovieBoxPlayerMod.Player;


internal sealed class EpisodePreferences
{
    public bool SubtitlePresent { get; set; }
    public bool SubtitleOff { get; set; }
    public string SubtitleLanguage { get; set; } = string.Empty;
    public bool QualityPresent { get; set; }
    public bool QualityAuto { get; set; }
    public ushort TargetQualityHeight { get; set; }
    public bool ServerPresent { get; set; }
    public bool ServerAuto { get; set; }
    public string ServerPreference { get; set; } = string.Empty;
    public bool AnyPresent => SubtitlePresent || QualityPresent || ServerPresent;
}

internal sealed class PlayerSettings
{
    private static readonly byte[] Magic = "COCK"u8.ToArray();
    private const byte CurrentVersion = 0x01;
    private const short MutedFlag = 1 << 0;
    private const short RawTitleFlag = 1 << 1;

    public string Title { get; init; } = string.Empty;
    public short Season { get; init; }
    public short Episode { get; init; }
    public byte BoxType { get; init; }
    public bool Muted { get; init; }
    public short Volume { get; init; } = 100;
    public float PlaybackSpeed { get; init; } = 1.0f;
    public float SubtitleDelay { get; init; }
    public int AudioTrackIndex { get; init; } = -1;
    public EpisodePreferences Preferences { get; init; } = new();

    public bool Matches(string title, short season, short episode, byte boxType) =>
        string.Equals(Title, title, StringComparison.Ordinal) &&
        Season == season && Episode == episode && BoxType == boxType;

    public static PlayerSettings? Load(string videoUri, string title, short season, short episode, byte boxType)
    {
        string primaryPath = GetPrimaryPath(videoUri, title);
        PlayerSettings? primary = TryRead(primaryPath);
        if (primary?.Matches(title, season, episode, boxType) == true)
            return primary;

        string fallbackPath = GetIdentityPath(videoUri, title, season, episode, boxType);
        PlayerSettings? fallback = TryRead(fallbackPath);
        return fallback?.Matches(title, season, episode, boxType) == true ? fallback : null;
    }

    public static void Save(string videoUri, PlayerSettings settings)
    {
        string directory = SettingsDirectory;
        Directory.CreateDirectory(directory);

        string primaryPath = GetPrimaryPath(videoUri, settings.Title);
        string path = primaryPath;
        if (File.Exists(primaryPath))
        {
            PlayerSettings? existing = TryRead(primaryPath);
            if (existing == null || !existing.Matches(settings.Title, settings.Season, settings.Episode, settings.BoxType))
                path = GetIdentityPath(videoUri, settings.Title, settings.Season, settings.Episode, settings.BoxType);
        }

        byte[] bytes = Encode(settings);
        string tempPath = path + ".tmp";
        File.WriteAllBytes(tempPath, bytes);
        File.Move(tempPath, path, overwrite: true);
    }

    internal static byte[] Encode(PlayerSettings settings)
    {
        byte[] rawTitle = Encoding.UTF8.GetBytes(settings.Title ?? string.Empty);
        byte[] rleTitle = EncodeRle(rawTitle);
        bool useRaw = rleTitle.Length >= rawTitle.Length;
        byte[] storedTitle = useRaw ? rawTitle : rleTitle;

        short flags = 0;
        if (settings.Muted) flags |= MutedFlag;
        if (useRaw) flags |= RawTitleFlag;

        using var stream = new MemoryStream(32 + storedTitle.Length);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        writer.Write(CurrentVersion);
        writer.Write(settings.Season);
        writer.Write(settings.Episode);
        writer.Write(flags);
        writer.Write((short)Math.Clamp(settings.Volume, (short)0, (short)100));
        writer.Write(settings.PlaybackSpeed);
        writer.Write(settings.SubtitleDelay);
        writer.Write(storedTitle.Length);
        writer.Write(storedTitle);

        // Version-1 extension fields. Readers that only understand the documented
        // header/title payload can stop after CompressedTitleLength bytes.
        writer.Write(settings.BoxType);
        writer.Write(settings.AudioTrackIndex);
        WritePreferences(writer, settings.Preferences);
        writer.Flush();
        return stream.ToArray();
    }

    internal static PlayerSettings Decode(byte[] bytes)
    {
        using var stream = new MemoryStream(bytes, writable: false);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);

        if (stream.Length < 25)
            throw new InvalidDataException("Settings file is truncated.");
        if (!reader.ReadBytes(4).SequenceEqual(Magic))
            throw new InvalidDataException("Settings file magic is invalid.");

        byte version = reader.ReadByte();
        if (version != CurrentVersion)
            throw new InvalidDataException($"Unsupported settings version {version}.");

        short season = reader.ReadInt16();
        short episode = reader.ReadInt16();
        short flags = reader.ReadInt16();
        short volume = reader.ReadInt16();
        float playbackSpeed = reader.ReadSingle();
        float subtitleDelay = reader.ReadSingle();
        int titleLength = reader.ReadInt32();
        if (titleLength < 0 || titleLength > stream.Length - stream.Position)
            throw new InvalidDataException("Settings title payload is invalid.");

        byte[] storedTitle = reader.ReadBytes(titleLength);
        byte[] titleBytes = (flags & RawTitleFlag) != 0 ? storedTitle : DecodeRle(storedTitle);
        string title = Encoding.UTF8.GetString(titleBytes);

        byte boxType = stream.Position < stream.Length ? reader.ReadByte() : (byte)(season != 0 || episode != 0 ? 2 : 1);
        int audioTrackIndex = stream.Length - stream.Position >= sizeof(int) ? reader.ReadInt32() : -1;
        EpisodePreferences preferences = TryReadPreferences(reader, stream);

        return new PlayerSettings
        {
            Title = title,
            Season = season,
            Episode = episode,
            BoxType = boxType,
            Muted = (flags & MutedFlag) != 0,
            Volume = (short)Math.Clamp(volume, (short)0, (short)100),
            PlaybackSpeed = float.IsFinite(playbackSpeed) ? Math.Clamp(playbackSpeed, 0.25f, 4.0f) : 1.0f,
            SubtitleDelay = float.IsFinite(subtitleDelay) ? Math.Clamp(subtitleDelay, -30.0f, 30.0f) : 0.0f,
            AudioTrackIndex = audioTrackIndex,
            Preferences = preferences
        };
    }

    private static void WritePreferences(BinaryWriter writer, EpisodePreferences preferences)
    {
        if (preferences == null || !preferences.AnyPresent)
            return;
        const byte SubtitlePresent = 1 << 0, QualityPresent = 1 << 1, ServerPresent = 1 << 2;
        const byte SubtitleOff = 1 << 3, QualityAuto = 1 << 4, ServerAuto = 1 << 5;
        byte flags = 0;
        if (preferences.SubtitlePresent) flags |= SubtitlePresent;
        if (preferences.QualityPresent) flags |= QualityPresent;
        if (preferences.ServerPresent) flags |= ServerPresent;
        if (preferences.SubtitlePresent && preferences.SubtitleOff) flags |= SubtitleOff;
        if (preferences.QualityPresent && preferences.QualityAuto) flags |= QualityAuto;
        if (preferences.ServerPresent && preferences.ServerAuto) flags |= ServerAuto;
        byte[] language = preferences.SubtitlePresent && !preferences.SubtitleOff ? Encoding.UTF8.GetBytes(preferences.SubtitleLanguage ?? string.Empty) : Array.Empty<byte>();
        byte[] server = preferences.ServerPresent && !preferences.ServerAuto ? Encoding.UTF8.GetBytes(preferences.ServerPreference ?? string.Empty) : Array.Empty<byte>();
        if (language.Length > byte.MaxValue || server.Length > byte.MaxValue) throw new InvalidDataException("PREF text exceeds 255 UTF-8 bytes.");
        writer.Write("PREF"u8.ToArray()); writer.Write((byte)1); writer.Write(flags);
        writer.Write(preferences.QualityPresent && !preferences.QualityAuto ? preferences.TargetQualityHeight : (ushort)0);
        writer.Write((byte)language.Length); writer.Write((byte)server.Length); writer.Write(language); writer.Write(server);
    }

    private static EpisodePreferences TryReadPreferences(BinaryReader reader, Stream stream)
    {
        try
        {
            long remaining = stream.Length - stream.Position;
            if (remaining < 4) return new EpisodePreferences();
            byte[] tail = reader.ReadBytes((int)remaining);
            if (tail.Length < 4 || tail[0] != (byte)'P' || tail[1] != (byte)'R' || tail[2] != (byte)'E' || tail[3] != (byte)'F') return new EpisodePreferences();
            if (tail.Length < 10 || tail[4] != 1) return new EpisodePreferences();
            byte flags = tail[5]; if ((flags & 0xC0) != 0) return new EpisodePreferences();
            ushort quality = (ushort)(tail[6] | (tail[7] << 8)); int ln = tail[8], sn = tail[9];
            if (10 + ln + sn != tail.Length) return new EpisodePreferences();
            bool sp=(flags&1)!=0, qp=(flags&2)!=0, svp=(flags&4)!=0, so=(flags&8)!=0, qa=(flags&16)!=0, sa=(flags&32)!=0;
            if ((!sp && (so || ln != 0)) || (sp && so && ln != 0) || (sp && !so && ln == 0)) return new EpisodePreferences();
            if ((!qp && (qa || quality != 0)) || (qp && qa && quality != 0) || (qp && !qa && quality == 0)) return new EpisodePreferences();
            if ((!svp && (sa || sn != 0)) || (svp && sa && sn != 0) || (svp && !sa && sn == 0)) return new EpisodePreferences();
            int p=10; string language=Encoding.UTF8.GetString(tail,p,ln); p+=ln; string server=Encoding.UTF8.GetString(tail,p,sn);
            return new EpisodePreferences { SubtitlePresent=sp, SubtitleOff=so, SubtitleLanguage=language, QualityPresent=qp, QualityAuto=qa, TargetQualityHeight=quality, ServerPresent=svp, ServerAuto=sa, ServerPreference=server };
        }
        catch { return new EpisodePreferences(); }
    }

    private static PlayerSettings? TryRead(string path)
    {
        try
        {
            return File.Exists(path) ? Decode(File.ReadAllBytes(path)) : null;
        }
        catch
        {
            // A corrupt or newer settings file must never prevent playback.
            return null;
        }
    }

    private static byte[] EncodeRle(byte[] raw)
    {
        if (raw.Length == 0)
            return Array.Empty<byte>();

        using var stream = new MemoryStream(raw.Length * 2);
        int i = 0;
        while (i < raw.Length)
        {
            byte value = raw[i];
            int count = 1;
            while (i + count < raw.Length && raw[i + count] == value && count < byte.MaxValue)
                count++;
            stream.WriteByte(value);
            stream.WriteByte((byte)count);
            i += count;
        }
        return stream.ToArray();
    }

    private static byte[] DecodeRle(byte[] encoded)
    {
        if ((encoded.Length & 1) != 0)
            throw new InvalidDataException("Settings title RLE payload is invalid.");

        using var stream = new MemoryStream();
        for (int i = 0; i < encoded.Length; i += 2)
        {
            byte value = encoded[i];
            int count = encoded[i + 1];
            if (count == 0)
                throw new InvalidDataException("Settings title RLE run is invalid.");
            for (int j = 0; j < count; j++)
                stream.WriteByte(value);
        }
        return stream.ToArray();
    }

    private static string SettingsDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "MovieBoxPlayerMod", "Settings");

    private static string GetPrimaryPath(string videoUri, string title) =>
        Path.Combine(SettingsDirectory, GetSafeBaseName(videoUri, title) + ".cock");

    private static string GetIdentityPath(string videoUri, string title, short season, short episode, byte boxType)
    {
        string identity = $"{boxType}|{title}|{season}|{episode}";
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(identity));
        string suffix = Convert.ToHexString(hash.AsSpan(0, 5)).ToLowerInvariant();
        return Path.Combine(SettingsDirectory, $"{GetSafeBaseName(videoUri, title)}.{suffix}.cock");
    }

    private static string GetSafeBaseName(string videoUri, string title)
    {
        string candidate = string.Empty;
        try
        {
            if (Uri.TryCreate(videoUri, UriKind.Absolute, out Uri? uri))
                candidate = Path.GetFileNameWithoutExtension(Uri.UnescapeDataString(uri.IsFile ? uri.LocalPath : uri.AbsolutePath));
            else
                candidate = Path.GetFileNameWithoutExtension(videoUri);
        }
        catch { }

        if (string.IsNullOrWhiteSpace(candidate))
            candidate = string.IsNullOrWhiteSpace(title) ? "video" : title;

        foreach (char invalid in Path.GetInvalidFileNameChars())
            candidate = candidate.Replace(invalid, '_');
        candidate = candidate.Trim().TrimEnd('.');
        if (candidate.Length > 96)
            candidate = candidate[..96];
        return string.IsNullOrWhiteSpace(candidate) ? "video" : candidate;
    }
}
