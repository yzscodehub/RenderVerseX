/** @file CookedEnvironmentArtifact.cpp @brief Deterministic float16 IBL IO */

#include "Resource/Cooked/CookedEnvironmentArtifact.h"

#include "CookedArtifactIO.h"
#include "Resource/Types/TextureResource.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace RVX::Resource
{
namespace
{
    constexpr uint32 EnvironmentTextureCount = 4;
    constexpr uint32 MaxTextureDimension = 16384;
    constexpr uint32 MaxMipLevels = 32;
    constexpr uint32 CubemapLayerCount = 6;

    uint16 FloatToHalfBits(float32 value)
    {
        const uint32 bits = std::bit_cast<uint32>(value);
        const uint32 sign = (bits >> 16u) & 0x8000u;
        const uint32 exponent = (bits >> 23u) & 0xffu;
        const uint32 mantissa = bits & 0x7fffffu;
        if (exponent == 0xffu)
            return static_cast<uint16>(sign | 0x7c00u |
                (mantissa == 0 ? 0u : 0x0200u));

        const int32 adjustedExponent = static_cast<int32>(exponent) - 127 + 15;
        if (adjustedExponent >= 31)
            return static_cast<uint16>(sign | 0x7c00u);
        if (adjustedExponent <= 0)
        {
            if (adjustedExponent < -10)
                return static_cast<uint16>(sign);
            uint32 normalized = mantissa | 0x800000u;
            const uint32 shift = static_cast<uint32>(14 - adjustedExponent);
            uint32 halfMantissa = normalized >> shift;
            const uint32 roundBit = (normalized >> (shift - 1u)) & 1u;
            const uint32 stickyMask = (uint32{1} << (shift - 1u)) - 1u;
            if (roundBit && ((normalized & stickyMask) != 0 ||
                             (halfMantissa & 1u) != 0))
                ++halfMantissa;
            return static_cast<uint16>(sign | halfMantissa);
        }

        uint32 halfExponent = static_cast<uint32>(adjustedExponent) << 10u;
        uint32 halfMantissa = mantissa >> 13u;
        const uint32 remainder = mantissa & 0x1fffu;
        if (remainder > 0x1000u ||
            (remainder == 0x1000u && (halfMantissa & 1u) != 0))
        {
            ++halfMantissa;
            if (halfMantissa == 0x400u)
            {
                halfMantissa = 0;
                halfExponent += 0x400u;
                if (halfExponent >= 0x7c00u)
                    halfExponent = 0x7c00u;
            }
        }
        return static_cast<uint16>(sign | halfExponent | halfMantissa);
    }

    bool ComputeElementCount(
        uint32 width,
        uint32 height,
        uint32 mipLevels,
        uint32 arrayLayers,
        uint64& outCount)
    {
        outCount = 0;
        for (uint32 mip = 0; mip < mipLevels; ++mip)
        {
            const uint64 mipWidth = std::max<uint32>(1, width >> mip);
            const uint64 mipHeight = std::max<uint32>(1, height >> mip);
            const uint64 texels = mipWidth * mipHeight * arrayLayers;
            if (texels > (std::numeric_limits<uint64>::max() - outCount) / 4u)
                return false;
            outCount += texels * 4u;
        }
        return true;
    }

    Hash::SHA256Digest HashHalfPayload(std::span<const uint16> values)
    {
        CookedArtifactIO::Writer bytes;
        for (uint16 value : values)
            bytes.U16(value);
        return Hash::ComputeSHA256(bytes.Data().data(), bytes.Data().size());
    }

    bool IsZeroDigest(const Hash::SHA256Digest& digest)
    {
        return std::all_of(digest.begin(), digest.end(),
                           [](uint8 value) { return value == 0; });
    }
} // namespace

bool CookedEnvironmentTexture::IsValid(std::string& outError) const
{
    const bool expectedCubemap =
        role != CookedEnvironmentTextureRole::BrdfLut;
    if (width == 0 || width > MaxTextureDimension || height == 0 ||
        height > MaxTextureDimension || mipLevels == 0 ||
        mipLevels > MaxMipLevels || arrayLayers == 0 ||
        cubemap != expectedCubemap ||
        (cubemap && arrayLayers != CubemapLayerCount) ||
        (!cubemap && arrayLayers != 1))
    {
        outError = "Cooked environment texture metadata is invalid";
        return false;
    }
    uint64 expectedElements = 0;
    if (!ComputeElementCount(width, height, mipLevels, arrayLayers,
                             expectedElements) ||
        expectedElements != rgba16f.size())
    {
        outError = "Cooked environment texture size does not match metadata";
        return false;
    }
    outError.clear();
    return true;
}

bool CookedEnvironmentArtifact::IsValid(std::string& outError) const
{
    if (sourcePath.empty() || sourcePath.find('\0') != std::string::npos ||
        importOptionsHash == 0 || !std::isfinite(intensity) || intensity <= 0.0f)
    {
        outError = "Cooked environment root metadata is invalid";
        return false;
    }
    for (size_t index = 0; index < textures.size(); ++index)
    {
        const CookedEnvironmentTexture& texture = textures[index];
        if (texture.role != static_cast<CookedEnvironmentTextureRole>(index))
        {
            outError =
                "Cooked environment texture roles are not in canonical order";
            return false;
        }
        if (!texture.IsValid(outError))
            return false;
    }
    outError.clear();
    return true;
}

bool BuildCookedEnvironmentTexture(
    CookedEnvironmentTextureRole role,
    const TextureResource& texture,
    CookedEnvironmentTexture& outTexture,
    std::string& outError)
{
    outTexture = {};
    const TextureMetadata& metadata = texture.GetMetadata();
    CookedEnvironmentTexture converted;
    converted.role = role;
    converted.width = metadata.width;
    converted.height = metadata.height;
    converted.mipLevels = metadata.mipLevels;
    converted.arrayLayers = metadata.arrayLayers;
    converted.cubemap = metadata.isCubemap;

    uint64 elementCount = 0;
    if (!ComputeElementCount(converted.width, converted.height,
                             converted.mipLevels, converted.arrayLayers,
                             elementCount) ||
        elementCount > std::numeric_limits<size_t>::max())
    {
        outError = "Environment texture dimensions overflow the cooked payload";
        return false;
    }
    converted.rgba16f.resize(static_cast<size_t>(elementCount));
    const std::vector<uint8>& source = texture.GetData();
    if (metadata.format == TextureFormat::RGBA16F)
    {
        if (source.size() != converted.rgba16f.size() * sizeof(uint16))
        {
            outError = "RGBA16F environment texture byte size is invalid";
            return false;
        }
        for (size_t index = 0; index < converted.rgba16f.size(); ++index)
        {
            const size_t offset = index * 2u;
            converted.rgba16f[index] = static_cast<uint16>(source[offset]) |
                (static_cast<uint16>(source[offset + 1u]) << 8u);
        }
    }
    else if (metadata.format == TextureFormat::RGBA32F)
    {
        if (source.size() != converted.rgba16f.size() * sizeof(float32))
        {
            outError = "RGBA32F environment texture byte size is invalid";
            return false;
        }
        for (size_t index = 0; index < converted.rgba16f.size(); ++index)
        {
            float32 value = 0.0f;
            std::memcpy(&value, source.data() + index * sizeof(float32),
                        sizeof(float32));
            if (!std::isfinite(value)) value = 0.0f;
            converted.rgba16f[index] = FloatToHalfBits(value);
        }
    }
    else
    {
        outError = "Environment artifacts accept only RGBA32F or RGBA16F inputs";
        return false;
    }
    converted.contentHash = HashHalfPayload(converted.rgba16f);
    if (!converted.IsValid(outError))
        return false;
    outTexture = std::move(converted);
    outError.clear();
    return true;
}

bool SerializeCookedEnvironmentArtifact(
    const CookedEnvironmentArtifact& artifact,
    std::vector<uint8>& outBytes,
    std::string& outError)
{
    outBytes.clear();
    if (!artifact.IsValid(outError))
        return false;

    CookedArtifactIO::Writer payload;
    if (!payload.String(artifact.sourcePath, outError))
        return false;
    payload.U64(artifact.importOptionsHash);
    payload.Float(artifact.intensity);
    payload.U32(EnvironmentTextureCount);
    for (const CookedEnvironmentTexture& texture : artifact.textures)
    {
        payload.U8(static_cast<uint8>(texture.role));
        payload.U8(texture.cubemap ? 1u : 0u);
        payload.U16(0);
        payload.U32(texture.width);
        payload.U32(texture.height);
        payload.U32(texture.mipLevels);
        payload.U32(texture.arrayLayers);
        payload.U64(static_cast<uint64>(texture.rgba16f.size()));
        const Hash::SHA256Digest digest = HashHalfPayload(texture.rgba16f);
        payload.Bytes(digest);
        for (uint16 value : texture.rgba16f)
            payload.U16(value);
    }
    return CookedArtifactIO::WriteEnvelope(
        RVX_ENVIRONMENT_ARTIFACT_MAGIC,
        RVX_ENVIRONMENT_ARTIFACT_SCHEMA_VERSION,
        payload.Data(),
        outBytes,
        outError);
}

bool DeserializeCookedEnvironmentArtifact(
    std::span<const uint8> bytes,
    CookedEnvironmentArtifact& outArtifact,
    std::string& outError)
{
    outArtifact = {};
    std::span<const uint8> payloadBytes;
    Hash::SHA256Digest payloadHash{};
    if (!CookedArtifactIO::ReadEnvelope(
            bytes,
            RVX_ENVIRONMENT_ARTIFACT_MAGIC,
            RVX_ENVIRONMENT_ARTIFACT_SCHEMA_VERSION,
            payloadBytes,
            payloadHash,
            outError))
    {
        return false;
    }

    CookedArtifactIO::Reader payload(payloadBytes);
    CookedEnvironmentArtifact parsed;
    uint32 textureCount = 0;
    if (!payload.String(parsed.sourcePath) ||
        !payload.U64(parsed.importOptionsHash) ||
        !payload.Float(parsed.intensity) ||
        !payload.U32(textureCount) || textureCount != EnvironmentTextureCount)
    {
        outError = "Cooked environment root payload is invalid";
        return false;
    }
    for (CookedEnvironmentTexture& texture : parsed.textures)
    {
        uint8 role = 0;
        uint8 cubemap = 0;
        uint16 reserved = 0;
        uint64 elementCount = 0;
        std::span<const uint8> digestBytes;
        if (!payload.U8(role) || role >= EnvironmentTextureCount ||
            !payload.U8(cubemap) || cubemap > 1 ||
            !payload.U16(reserved) || reserved != 0 ||
            !payload.U32(texture.width) ||
            !payload.U32(texture.height) ||
            !payload.U32(texture.mipLevels) ||
            !payload.U32(texture.arrayLayers) ||
            !payload.U64(elementCount) ||
            elementCount > std::numeric_limits<size_t>::max() ||
            elementCount > payload.Remaining() / sizeof(uint16) ||
            !payload.Bytes(Hash::RVX_SHA256_DIGEST_SIZE, digestBytes))
        {
            outError = "Cooked environment texture metadata is invalid";
            return false;
        }
        texture.role = static_cast<CookedEnvironmentTextureRole>(role);
        texture.cubemap = cubemap != 0;
        std::copy(digestBytes.begin(), digestBytes.end(),
                  texture.contentHash.begin());
        texture.rgba16f.resize(static_cast<size_t>(elementCount));
        for (uint16& value : texture.rgba16f)
        {
            if (!payload.U16(value))
            {
                outError = "Cooked environment texture payload is truncated";
                return false;
            }
        }
        if (!texture.IsValid(outError) || IsZeroDigest(texture.contentHash) ||
            HashHalfPayload(texture.rgba16f) != texture.contentHash)
        {
            if (outError.empty())
                outError = "Cooked environment texture content hash mismatch";
            return false;
        }
    }
    if (payload.Remaining() != 0)
    {
        outError = "Cooked environment payload has trailing bytes";
        return false;
    }
    parsed.payloadHash = payloadHash;
    if (!parsed.IsValid(outError) || IsZeroDigest(parsed.payloadHash))
        return false;

    outArtifact = std::move(parsed);
    outError.clear();
    return true;
}
} // namespace RVX::Resource
