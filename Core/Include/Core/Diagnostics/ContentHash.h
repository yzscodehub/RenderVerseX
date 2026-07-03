#pragma once

/**
 * @file ContentHash.h
 * @brief Lightweight deterministic content hashing helpers for diagnostics artifacts.
 */

#include "Core/Types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace RVX::Diagnostics
{
    inline constexpr uint64 RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS = 14695981039346656037ull;
    inline constexpr uint64 RVX_DIAGNOSTICS_FNV1A64_PRIME = 1099511628211ull;

    std::string FormatContentHash(uint64 hash);
    void MixContentHashString(uint64& hash, std::string_view value);
    void MixContentHashValue(uint64& hash, uint64 value);
    std::string ComputeFileContentHash(const std::filesystem::path& path);
} // namespace RVX::Diagnostics
