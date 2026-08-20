#pragma once

/**
 * @file SHA256.h
 * @brief Streaming SHA-256 content hash implementation.
 */

#include "Core/Types.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace RVX::Hash
{
    inline constexpr size_t RVX_SHA256_DIGEST_SIZE = 32;

    using SHA256Digest = std::array<uint8, RVX_SHA256_DIGEST_SIZE>;

    /**
     * @brief Incremental SHA-256 hasher suitable for parser-owned read buffers.
     *
     * FinalizeDigest() is const and therefore may be called repeatedly while
     * additional input is still accepted by the original hasher.
     */
    class SHA256Hasher
    {
    public:
        SHA256Hasher();

        void Update(const void* data, size_t byteCount);
        void Update(std::string_view value);

        [[nodiscard]] SHA256Digest FinalizeDigest() const;
        [[nodiscard]] std::string FinalizeHex() const;

    private:
        void Transform(const uint8 block[64]);
        SHA256Digest FinalizeInPlace();

        std::array<uint32, 8> m_state{};
        std::array<uint8, 64> m_buffer{};
        uint64 m_totalBytes = 0;
        size_t m_bufferSize = 0;
    };

    [[nodiscard]] std::string FormatSHA256Digest(const SHA256Digest& digest);
    [[nodiscard]] SHA256Digest ComputeSHA256(const void* data, size_t byteCount);
    [[nodiscard]] SHA256Digest ComputeSHA256(std::string_view value);
} // namespace RVX::Hash
