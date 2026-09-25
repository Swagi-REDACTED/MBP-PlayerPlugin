#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace cock
{
    inline constexpr std::size_t kMaxValueBytes = 16u * 1024u * 1024u;

    struct Document
    {
        std::string value;
    };

    enum class Error
    {
        None,
        NotFound,
        InvalidMagic,
        UnsupportedVersion,
        Truncated,
        CorruptData,
        TooLarge,
        IoReadFailed,
        IoWriteFailed,
        AtomicReplaceFailed
    };

    struct LoadResult
    {
        bool ok = false;
        Document document;
        Error error = Error::None;
        std::string message;
    };

    std::vector<std::uint8_t> Encode(const Document& document);
    LoadResult Decode(std::span<const std::uint8_t> bytes);
    LoadResult LoadFile(const std::filesystem::path& path);
    bool SaveFileAtomic(
        const std::filesystem::path& path,
        const Document& document,
        std::string* errorMessage = nullptr);
}
