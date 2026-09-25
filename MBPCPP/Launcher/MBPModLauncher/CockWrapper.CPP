#include "CockWrapper.HPP"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cock
{
    namespace
    {
        constexpr std::uint8_t kVersion = 0x01;
        constexpr std::uint16_t kRawValueFlag = 1u << 1;
        constexpr std::int16_t kSeason = 0;
        constexpr std::int16_t kEpisode = 0;
        constexpr std::int16_t kVolume = 100;
        constexpr float kPlaybackSpeed = 1.0f;
        constexpr float kSubtitleDelay = 0.0f;
        constexpr std::uint8_t kBoxType = 0;
        constexpr std::int32_t kAudioTrackIndex = -1;
        constexpr std::size_t kHeaderBytes = 25;
        constexpr std::size_t kFullTailBytes = 5;

        LoadResult Fail(Error error, std::string message)
        {
            LoadResult result;
            result.error = error;
            result.message = std::move(message);
            return result;
        }

        void WriteU16(std::vector<std::uint8_t>& out, std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
        }

        void WriteU32(std::vector<std::uint8_t>& out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
            out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
        }

        void WriteI16(std::vector<std::uint8_t>& out, std::int16_t value)
        {
            WriteU16(out, static_cast<std::uint16_t>(value));
        }

        void WriteI32(std::vector<std::uint8_t>& out, std::int32_t value)
        {
            WriteU32(out, static_cast<std::uint32_t>(value));
        }

        void WriteFloat(std::vector<std::uint8_t>& out, float value)
        {
            WriteU32(out, std::bit_cast<std::uint32_t>(value));
        }

        std::uint16_t ReadU16(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            return static_cast<std::uint16_t>(bytes[offset]) |
                (static_cast<std::uint16_t>(bytes[offset + 1]) << 8u);
        }

        std::uint32_t ReadU32(std::span<const std::uint8_t> bytes, std::size_t offset)
        {
            return static_cast<std::uint32_t>(bytes[offset]) |
                (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
                (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
                (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
        }

        std::vector<std::uint8_t> EncodeRle(std::string_view value)
        {
            std::vector<std::uint8_t> out;
            if (value.empty())
                return out;

            out.reserve(std::min<std::size_t>(value.size() * 2u, kMaxValueBytes));
            std::size_t i = 0;
            while (i < value.size())
            {
                const auto byte = static_cast<std::uint8_t>(value[i]);
                std::size_t run = 1;
                while (i + run < value.size() && value[i + run] == value[i] && run < 255u)
                    ++run;
                out.push_back(byte);
                out.push_back(static_cast<std::uint8_t>(run));
                i += run;
            }
            return out;
        }

        bool IsRegularFile(const std::filesystem::path& path) noexcept
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
        }
    }

    std::vector<std::uint8_t> Encode(const Document& document)
    {
        if (document.value.size() > kMaxValueBytes || document.value.size() > std::numeric_limits<std::uint32_t>::max())
            return {};

        const auto rle = EncodeRle(document.value);
        const bool useRaw = rle.size() >= document.value.size();
        const std::uint16_t flags = useRaw ? kRawValueFlag : 0u;
        const auto payloadSize = useRaw ? document.value.size() : rle.size();

        std::vector<std::uint8_t> out;
        out.reserve(kHeaderBytes + payloadSize + kFullTailBytes);
        out.insert(out.end(), {'C', 'O', 'C', 'K'});
        out.push_back(kVersion);
        WriteI16(out, kSeason);
        WriteI16(out, kEpisode);
        WriteU16(out, flags);
        WriteI16(out, kVolume);
        WriteFloat(out, kPlaybackSpeed);
        WriteFloat(out, kSubtitleDelay);
        WriteU32(out, static_cast<std::uint32_t>(payloadSize));
        if (useRaw)
        {
            out.insert(out.end(), document.value.begin(), document.value.end());
        }
        else
        {
            out.insert(out.end(), rle.begin(), rle.end());
        }
        out.push_back(kBoxType);
        WriteI32(out, kAudioTrackIndex);
        return out;
    }

    LoadResult Decode(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() < kHeaderBytes)
            return Fail(Error::Truncated, "COCK v1 fixed header is truncated.");
        if (!(bytes[0] == 'C' && bytes[1] == 'O' && bytes[2] == 'C' && bytes[3] == 'K'))
            return Fail(Error::InvalidMagic, "Invalid COCK magic.");
        if (bytes[4] != kVersion)
            return Fail(Error::UnsupportedVersion, "Unsupported COCK version.");

        const std::uint16_t flags = ReadU16(bytes, 9);
        const std::uint32_t payloadLength = ReadU32(bytes, 21);
        if (payloadLength > kMaxValueBytes)
            return Fail(Error::TooLarge, "COCK payload exceeds the configured safety limit.");
        if (static_cast<std::size_t>(payloadLength) > bytes.size() - kHeaderBytes)
            return Fail(Error::Truncated, "COCK payload extends beyond the input buffer.");

        const auto payload = bytes.subspan(kHeaderBytes, payloadLength);
        Document document;
        if ((flags & kRawValueFlag) != 0)
        {
            document.value.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
        }
        else
        {
            if ((payload.size() & 1u) != 0u)
                return Fail(Error::CorruptData, "COCK RLE payload has an odd byte count.");

            std::size_t decodedSize = 0;
            for (std::size_t i = 0; i < payload.size(); i += 2)
            {
                const std::uint8_t count = payload[i + 1];
                if (count == 0)
                    return Fail(Error::CorruptData, "COCK RLE payload contains a zero-length run.");
                decodedSize += count;
                if (decodedSize > kMaxValueBytes)
                    return Fail(Error::TooLarge, "Decoded COCK value exceeds the configured safety limit.");
            }

            document.value.reserve(decodedSize);
            for (std::size_t i = 0; i < payload.size(); i += 2)
                document.value.append(payload[i + 1], static_cast<char>(payload[i]));
        }

        LoadResult result;
        result.ok = true;
        result.document = std::move(document);
        return result;
    }

    LoadResult LoadFile(const std::filesystem::path& path)
    {
        try
        {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec))
                return Fail(Error::NotFound, "COCK file was not found.");
            if (ec || !IsRegularFile(path))
                return Fail(Error::IoReadFailed, "COCK path is not a readable regular file.");

            const auto size = std::filesystem::file_size(path, ec);
            if (ec)
                return Fail(Error::IoReadFailed, "Unable to query COCK file size.");
            if (size > kMaxValueBytes + kHeaderBytes + kFullTailBytes)
                return Fail(Error::TooLarge, "COCK file exceeds the configured safety limit.");

            std::ifstream input(path, std::ios::binary);
            if (!input)
                return Fail(Error::IoReadFailed, "Unable to open COCK file for reading.");
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (!bytes.empty())
            {
                input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size()))
                    return Fail(Error::IoReadFailed, "Unable to read the complete COCK file.");
            }
            return Decode(bytes);
        }
        catch (const std::exception& e)
        {
            return Fail(Error::IoReadFailed, e.what());
        }
        catch (...)
        {
            return Fail(Error::IoReadFailed, "Unknown COCK file read failure.");
        }
    }

    bool SaveFileAtomic(const std::filesystem::path& path, const Document& document, std::string* errorMessage)
    {
        auto setError = [&](std::string text)
        {
            if (errorMessage)
                *errorMessage = std::move(text);
        };
        if (errorMessage)
            errorMessage->clear();

        try
        {
            const auto bytes = Encode(document);
            if (bytes.empty() && !document.value.empty())
            {
                setError("COCK value exceeds the configured safety limit.");
                return false;
            }

            std::error_code ec;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), ec);
                if (ec)
                {
                    setError("Unable to create the COCK settings directory: " + ec.message());
                    return false;
                }
            }

            auto temp = path;
#ifdef _WIN32
            temp += L".tmp";
#else
            temp += ".tmp";
#endif
            std::filesystem::remove(temp, ec);
            ec.clear();

            {
                std::ofstream output(temp, std::ios::binary | std::ios::trunc);
                if (!output)
                {
                    setError("Unable to open the temporary COCK file for writing.");
                    return false;
                }
                if (!bytes.empty())
                    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                output.flush();
                if (!output)
                {
                    output.close();
                    std::filesystem::remove(temp, ec);
                    setError("Unable to write the complete temporary COCK file.");
                    return false;
                }
            }

#ifdef _WIN32
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const DWORD code = GetLastError();
                std::filesystem::remove(temp, ec);
                setError("Unable to atomically replace the COCK file. Win32 error: " + std::to_string(code));
                return false;
            }
#else
            std::filesystem::rename(temp, path, ec);
            if (ec)
            {
                std::filesystem::remove(temp, ec);
                setError("Unable to atomically replace the COCK file: " + ec.message());
                return false;
            }
#endif
            return true;
        }
        catch (const std::exception& e)
        {
            setError(e.what());
            std::error_code ec;
            auto temp = path;
#ifdef _WIN32
            temp += L".tmp";
#else
            temp += ".tmp";
#endif
            std::filesystem::remove(temp, ec);
            return false;
        }
        catch (...)
        {
            setError("Unknown COCK file write failure.");
            return false;
        }
    }
}
