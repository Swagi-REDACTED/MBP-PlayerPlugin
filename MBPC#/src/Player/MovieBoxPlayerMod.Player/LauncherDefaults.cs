using System;
using System.IO;
using System.Text;

namespace MovieBoxPlayerMod.Player;

internal sealed record LauncherPlayerDefaults(
    bool SubtitleOff,
    string SubtitleLanguage,
    bool QualityAuto,
    ushort TargetQualityHeight,
    bool ServerAuto,
    string ServerPreference,
    byte Volume)
{
    public static LauncherPlayerDefaults BuiltIn { get; } = new(true, string.Empty, true, 0, true, string.Empty, 100);
}

internal static class LauncherDefaults
{
    private static readonly byte[] LdefMagic = "LDEF"u8.ToArray();
    private const short RawTitleFlag = 1 << 1;
    private const byte SubtitleOffFlag = 1 << 0;
    private const byte QualityAutoFlag = 1 << 1;
    private const byte ServerAutoFlag = 1 << 2;
    private const byte KnownFlags = SubtitleOffFlag | QualityAutoFlag | ServerAutoFlag;

    internal static string SettingsPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "MovieBoxPlayerMod", "Launcher", "Settings.cock");

    public static LauncherPlayerDefaults Load()
    {
        try
        {
            return File.Exists(SettingsPath) ? DecodeFile(File.ReadAllBytes(SettingsPath)) : LauncherPlayerDefaults.BuiltIn;
        }
        catch
        {
            return LauncherPlayerDefaults.BuiltIn;
        }
    }

    internal static LauncherPlayerDefaults DecodeFile(byte[] bytes)
    {
        try
        {
            byte[] payload = DecodeOuterValue(bytes);
            if (payload.Length < 4 || !payload.AsSpan(0, 4).SequenceEqual(LdefMagic))
                return LauncherPlayerDefaults.BuiltIn; // legacy path-only launcher file
            return DecodeLdef(payload);
        }
        catch
        {
            return LauncherPlayerDefaults.BuiltIn;
        }
    }

    private static LauncherPlayerDefaults DecodeLdef(byte[] b)
    {
        if (b.Length < 13 || b[4] != 1) throw new InvalidDataException();
        byte flags = b[5];
        if ((flags & ~KnownFlags) != 0) throw new InvalidDataException();
        byte volume = Math.Min((byte)100, b[6]);
        ushort quality = ReadU16(b, 7);
        ushort pathLength = ReadU16(b, 9);
        int languageLength = b[11], serverLength = b[12];
        int expected = 13 + pathLength + languageLength + serverLength;
        if (expected != b.Length) throw new InvalidDataException();
        int p = 13 + pathLength;
        string language = Encoding.UTF8.GetString(b, p, languageLength); p += languageLength;
        string server = Encoding.UTF8.GetString(b, p, serverLength);
        bool subtitleOff = (flags & SubtitleOffFlag) != 0;
        bool qualityAuto = (flags & QualityAutoFlag) != 0;
        bool serverAuto = (flags & ServerAutoFlag) != 0;
        if (subtitleOff != (languageLength == 0)) throw new InvalidDataException();
        if (qualityAuto != (quality == 0)) throw new InvalidDataException();
        if (serverAuto != (serverLength == 0)) throw new InvalidDataException();
        return new LauncherPlayerDefaults(subtitleOff, language, qualityAuto, quality, serverAuto, server, volume);
    }

    private static byte[] DecodeOuterValue(byte[] bytes)
    {
        if (bytes.Length < 25 || bytes[0] != (byte)'C' || bytes[1] != (byte)'O' || bytes[2] != (byte)'C' || bytes[3] != (byte)'K' || bytes[4] != 1)
            throw new InvalidDataException();
        short flags = BitConverter.ToInt16(bytes, 9);
        int payloadLength = BitConverter.ToInt32(bytes, 21);
        if (payloadLength < 0 || 25 + payloadLength > bytes.Length) throw new InvalidDataException();
        byte[] stored = bytes.AsSpan(25, payloadLength).ToArray();
        if ((flags & RawTitleFlag) != 0) return stored;
        if ((stored.Length & 1) != 0) throw new InvalidDataException();
        using var stream = new MemoryStream();
        for (int i = 0; i < stored.Length; i += 2)
        {
            int count = stored[i + 1];
            if (count == 0) throw new InvalidDataException();
            for (int j = 0; j < count; j++) stream.WriteByte(stored[i]);
        }
        return stream.ToArray();
    }

    private static ushort ReadU16(byte[] b, int p) => (ushort)(b[p] | (b[p + 1] << 8));
}
