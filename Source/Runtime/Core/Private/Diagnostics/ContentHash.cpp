/**
 * @file ContentHash.cpp
 * @brief Lightweight deterministic content hashing helpers for diagnostics artifacts.
 */

#include "Core/Diagnostics/ContentHash.h"

#include <array>
#include <fstream>

namespace RVX::Diagnostics
{
    std::string FormatContentHash(uint64 hash)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text(16, '0');
        for (int32 i = 15; i >= 0; --i)
        {
            text[static_cast<size_t>(i)] = digits[hash & 0x0F];
            hash >>= 4;
        }
        return text;
    }

    void MixContentHashString(uint64& hash, std::string_view value)
    {
        for (char ch : value)
        {
            hash ^= static_cast<uint8>(ch);
            hash *= RVX_DIAGNOSTICS_FNV1A64_PRIME;
        }
        hash ^= 0xFFu;
        hash *= RVX_DIAGNOSTICS_FNV1A64_PRIME;
    }

    void MixContentHashValue(uint64& hash, uint64 value)
    {
        MixContentHashString(hash, std::to_string(value));
    }

    std::string ComputeFileContentHash(const std::filesystem::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::error_code fileError;
        if (!std::filesystem::is_regular_file(path, fileError) || fileError)
        {
            return {};
        }

        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return {};
        }

        uint64 hash = RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        std::array<char, 4096> buffer{};
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize bytesRead = file.gcount();
            for (std::streamsize i = 0; i < bytesRead; ++i)
            {
                hash ^= static_cast<uint8>(buffer[static_cast<size_t>(i)]);
                hash *= RVX_DIAGNOSTICS_FNV1A64_PRIME;
            }
        }

        if (file.bad())
        {
            return {};
        }

        return FormatContentHash(hash);
    }
} // namespace RVX::Diagnostics
