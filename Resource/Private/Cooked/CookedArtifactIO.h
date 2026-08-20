#pragma once

/** @file CookedArtifactIO.h @brief Internal bounded binary artifact helpers */

#include "Core/Hash/SHA256.h"
#include "Core/Types.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RVX::Resource::CookedArtifactIO
{
    inline constexpr uint64 MaxArtifactBytes = 2ull * 1024ull * 1024ull * 1024ull;
    inline constexpr uint32 MaxStringBytes = 1024u * 1024u;

    class Writer
    {
    public:
        void U8(uint8 value) { m_bytes.push_back(value); }
        void U16(uint16 value)
        {
            U8(static_cast<uint8>(value));
            U8(static_cast<uint8>(value >> 8u));
        }
        void U32(uint32 value)
        {
            for (uint32 shift = 0; shift < 32; shift += 8)
                U8(static_cast<uint8>(value >> shift));
        }
        void U64(uint64 value)
        {
            for (uint32 shift = 0; shift < 64; shift += 8)
                U8(static_cast<uint8>(value >> shift));
        }
        void Float(float32 value) { U32(std::bit_cast<uint32>(value)); }
        void Bytes(std::span<const uint8> value)
        {
            m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        }
        bool String(std::string_view value, std::string& outError)
        {
            if (value.size() > MaxStringBytes ||
                value.find('\0') != std::string_view::npos)
            {
                outError = "Cooked artifact string is invalid or too large";
                return false;
            }
            U32(static_cast<uint32>(value.size()));
            Bytes({reinterpret_cast<const uint8*>(value.data()), value.size()});
            return true;
        }
        [[nodiscard]] const std::vector<uint8>& Data() const { return m_bytes; }
        [[nodiscard]] std::vector<uint8> Take() { return std::move(m_bytes); }

    private:
        std::vector<uint8> m_bytes;
    };

    class Reader
    {
    public:
        explicit Reader(std::span<const uint8> bytes) : m_bytes(bytes) {}

        bool U8(uint8& value)
        {
            if (Remaining() < 1) return false;
            value = m_bytes[m_offset++];
            return true;
        }
        bool U16(uint16& value)
        {
            uint8 low = 0;
            uint8 high = 0;
            if (!U8(low) || !U8(high)) return false;
            value = static_cast<uint16>(low) |
                (static_cast<uint16>(high) << 8u);
            return true;
        }
        bool U32(uint32& value)
        {
            value = 0;
            for (uint32 shift = 0; shift < 32; shift += 8)
            {
                uint8 byte = 0;
                if (!U8(byte)) return false;
                value |= static_cast<uint32>(byte) << shift;
            }
            return true;
        }
        bool U64(uint64& value)
        {
            value = 0;
            for (uint32 shift = 0; shift < 64; shift += 8)
            {
                uint8 byte = 0;
                if (!U8(byte)) return false;
                value |= static_cast<uint64>(byte) << shift;
            }
            return true;
        }
        bool Float(float32& value)
        {
            uint32 bits = 0;
            if (!U32(bits)) return false;
            value = std::bit_cast<float32>(bits);
            return true;
        }
        bool Bytes(size_t count, std::span<const uint8>& value)
        {
            if (count > Remaining()) return false;
            value = m_bytes.subspan(m_offset, count);
            m_offset += count;
            return true;
        }
        bool String(std::string& value)
        {
            uint32 count = 0;
            std::span<const uint8> bytes;
            if (!U32(count) || count > MaxStringBytes || !Bytes(count, bytes))
                return false;
            value.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            return value.find('\0') == std::string::npos;
        }
        [[nodiscard]] size_t Remaining() const { return m_bytes.size() - m_offset; }

    private:
        std::span<const uint8> m_bytes;
        size_t m_offset = 0;
    };

    [[nodiscard]] inline bool WriteEnvelope(
        std::string_view magic,
        uint32 schemaVersion,
        std::span<const uint8> payload,
        std::vector<uint8>& outBytes,
        std::string& outError)
    {
        const size_t envelopeBytes = magic.size() + sizeof(uint32) +
            sizeof(uint64) + Hash::RVX_SHA256_DIGEST_SIZE;
        if (magic.empty() || payload.empty() ||
            envelopeBytes > MaxArtifactBytes ||
            payload.size() > MaxArtifactBytes - envelopeBytes)
        {
            outError = "Cooked artifact envelope input is invalid";
            return false;
        }
        const Hash::SHA256Digest digest =
            Hash::ComputeSHA256(payload.data(), payload.size());
        Writer writer;
        writer.Bytes({reinterpret_cast<const uint8*>(magic.data()), magic.size()});
        writer.U32(schemaVersion);
        writer.U64(static_cast<uint64>(payload.size()));
        writer.Bytes(digest);
        writer.Bytes(payload);
        outBytes = writer.Take();
        outError.clear();
        return true;
    }

    [[nodiscard]] inline bool ReadEnvelope(
        std::span<const uint8> bytes,
        std::string_view expectedMagic,
        uint32 expectedSchemaVersion,
        std::span<const uint8>& outPayload,
        Hash::SHA256Digest& outPayloadHash,
        std::string& outError)
    {
        constexpr size_t EnvelopeMetadataBytes =
            sizeof(uint32) + sizeof(uint64) + Hash::RVX_SHA256_DIGEST_SIZE;
        if (bytes.size() < expectedMagic.size() + EnvelopeMetadataBytes ||
            bytes.size() > MaxArtifactBytes)
        {
            outError = "Cooked artifact envelope size is invalid";
            return false;
        }
        const std::span<const uint8> observedMagic =
            bytes.first(expectedMagic.size());
        if (!std::equal(observedMagic.begin(), observedMagic.end(),
                        reinterpret_cast<const uint8*>(expectedMagic.data())))
        {
            outError = "Cooked artifact magic is invalid";
            return false;
        }
        Reader reader(bytes.subspan(expectedMagic.size()));
        uint32 schemaVersion = 0;
        uint64 payloadSize = 0;
        std::span<const uint8> digestBytes;
        if (!reader.U32(schemaVersion) || schemaVersion != expectedSchemaVersion ||
            !reader.U64(payloadSize) || payloadSize > MaxArtifactBytes ||
            !reader.Bytes(Hash::RVX_SHA256_DIGEST_SIZE, digestBytes) ||
            payloadSize != reader.Remaining() ||
            !reader.Bytes(static_cast<size_t>(payloadSize), outPayload) ||
            reader.Remaining() != 0)
        {
            outError = "Cooked artifact envelope metadata is invalid";
            return false;
        }
        std::copy(digestBytes.begin(), digestBytes.end(), outPayloadHash.begin());
        const Hash::SHA256Digest observed =
            Hash::ComputeSHA256(outPayload.data(), outPayload.size());
        if (observed != outPayloadHash)
        {
            outError = "Cooked artifact payload hash mismatch";
            return false;
        }
        outError.clear();
        return true;
    }
} // namespace RVX::Resource::CookedArtifactIO
