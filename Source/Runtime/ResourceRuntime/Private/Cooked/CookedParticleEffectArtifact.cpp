/** @file CookedParticleEffectArtifact.cpp @brief Strict CPU billboard effect IO. */

#include "Resource/Cooked/CookedParticleEffectArtifact.h"

#include "CookedArtifactIO.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace RVX::Resource
{
namespace
{
    constexpr uint32 MaxParticleCount = 1u << 20u;
    constexpr uint32 MaxFlipbookDimension = 256u;
    constexpr float32 MaxFiniteMagnitude = 65504.0f;

    bool IsCanonicalRelativePath(const std::string& value)
    {
        if (value.empty() || value.find('\0') != std::string::npos ||
            value.find('\\') != std::string::npos)
        {
            return false;
        }
        const std::filesystem::path path(value);
        if (path.is_absolute() || path.has_root_name() ||
            path.generic_string() != value)
        {
            return false;
        }
        for (const std::filesystem::path& component : path)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }
        return true;
    }

    bool IsFiniteInRange(float32 value, float32 minimum, float32 maximum)
    {
        return std::isfinite(value) && value >= minimum && value <= maximum;
    }

    bool IsFinite(const Vec2& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z);
    }

    bool IsFinite(const Vec4& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
            std::isfinite(value.z) && std::isfinite(value.w);
    }

    bool IsZero(const Vec3& value)
    {
        return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
    }

    bool IsZeroDigest(const Hash::SHA256Digest& digest)
    {
        return std::all_of(digest.begin(), digest.end(),
            [](uint8 value) { return value == 0u; });
    }

    void WriteVec2(CookedArtifactIO::Writer& writer, const Vec2& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
    }

    void WriteVec3(CookedArtifactIO::Writer& writer, const Vec3& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
        writer.Float(value.z);
    }

    void WriteVec4(CookedArtifactIO::Writer& writer, const Vec4& value)
    {
        writer.Float(value.x);
        writer.Float(value.y);
        writer.Float(value.z);
        writer.Float(value.w);
    }

    bool ReadVec2(CookedArtifactIO::Reader& reader, Vec2& value)
    {
        return reader.Float(value.x) && reader.Float(value.y);
    }

    bool ReadVec3(CookedArtifactIO::Reader& reader, Vec3& value)
    {
        return reader.Float(value.x) && reader.Float(value.y) &&
            reader.Float(value.z);
    }

    bool ReadVec4(CookedArtifactIO::Reader& reader, Vec4& value)
    {
        return reader.Float(value.x) && reader.Float(value.y) &&
            reader.Float(value.z) && reader.Float(value.w);
    }
} // namespace

bool CookedParticleEffectArtifact::IsValid(std::string& outError) const
{
    if (!IsCanonicalRelativePath(logicalAssetId))
    {
        outError = "Particle effect logical asset id is not canonical and package-relative";
        return false;
    }
    if (blendMode != CookedParticleEffectBlendMode::AlphaBlend &&
        blendMode != CookedParticleEffectBlendMode::Premultiplied &&
        blendMode != CookedParticleEffectBlendMode::Additive)
    {
        outError = "Particle effect blend mode is unsupported";
        return false;
    }

    const CookedParticleEffectEmitter& value = emitter;
    if (value.maxParticles == 0u || value.maxParticles > MaxParticleCount ||
        !IsFiniteInRange(value.durationSeconds, 0.001f, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.emissionRate, 0.0f, MaxFiniteMagnitude) ||
        !IsFinite(value.position) ||
        !IsFiniteInRange(value.speedMin, 0.0f, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.speedMax, value.speedMin, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.lifetimeMin, 0.001f, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.lifetimeMax, value.lifetimeMin, MaxFiniteMagnitude) ||
        !IsFinite(value.initialColor) ||
        !IsFiniteInRange(value.sizeMin, 0.001f, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.sizeMax, value.sizeMin, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.rotationDegreesMin, -MaxFiniteMagnitude, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.rotationDegreesMax, value.rotationDegreesMin, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.rotationSpeedDegreesMin, -MaxFiniteMagnitude, MaxFiniteMagnitude) ||
        !IsFiniteInRange(value.rotationSpeedDegreesMax,
                         value.rotationSpeedDegreesMin,
                         MaxFiniteMagnitude))
    {
        outError = "Particle effect emitter contains non-finite, out-of-range, or non-deterministic values";
        return false;
    }
    const auto colorComponentIsValid = [](float32 component)
    {
        return IsFiniteInRange(component, 0.0f, MaxFiniteMagnitude);
    };
    if (!colorComponentIsValid(value.initialColor.x) ||
        !colorComponentIsValid(value.initialColor.y) ||
        !colorComponentIsValid(value.initialColor.z) ||
        !colorComponentIsValid(value.initialColor.w))
    {
        outError = "Particle effect colors are invalid";
        return false;
    }

    const CookedParticleEffectFlipbook& flipbookValue = flipbook;
    const uint64 tileCount = static_cast<uint64>(flipbookValue.tilesX) * flipbookValue.tilesY;
    if (!IsCanonicalRelativePath(flipbookValue.textureArtifactPath) ||
        std::filesystem::path(flipbookValue.textureArtifactPath).extension().string() != ".rva")
    {
        outError = "Particle effect flipbook atlas path is not canonical and package-relative .rva";
        return false;
    }
    if (IsZeroDigest(flipbookValue.expectedContentHash))
    {
        outError = "Particle effect flipbook atlas content hash is required";
        return false;
    }
    if (
        flipbookValue.tilesX == 0u || flipbookValue.tilesX > MaxFlipbookDimension ||
        flipbookValue.tilesY == 0u || flipbookValue.tilesY > MaxFlipbookDimension ||
        tileCount == 0u || flipbookValue.firstFrame >= tileCount ||
        flipbookValue.frameCount == 0u ||
        flipbookValue.frameCount > tileCount - flipbookValue.firstFrame ||
        !IsFiniteInRange(flipbookValue.framesPerSecond, 0.001f, 240.0f) ||
        !IsFinite(flipbookValue.uvOffset) || !IsFinite(flipbookValue.uvScale) ||
        flipbookValue.uvOffset.x < 0.0f || flipbookValue.uvOffset.y < 0.0f ||
        flipbookValue.uvScale.x <= 0.0f || flipbookValue.uvScale.y <= 0.0f ||
        flipbookValue.uvScale.x > 1.0f || flipbookValue.uvScale.y > 1.0f ||
        flipbookValue.uvOffset.x + flipbookValue.uvScale.x > 1.0f ||
        flipbookValue.uvOffset.y + flipbookValue.uvScale.y > 1.0f)
    {
        outError = "Particle effect flipbook metadata is invalid";
        return false;
    }

    const CookedParticleEffectSoftDepth& softDepthValue = softDepth;
    if (softDepthValue.policy == CookedParticleEffectSoftDepthPolicy::Disabled)
    {
        if (softDepthValue.fadeDistanceMeters != 0.0f || softDepthValue.contrastPower != 1.0f)
        {
            outError = "Disabled particle soft depth policy must have canonical fade=0 and contrast=1 parameters";
            return false;
        }
    }
    else if ((softDepthValue.policy != CookedParticleEffectSoftDepthPolicy::Optional &&
              softDepthValue.policy != CookedParticleEffectSoftDepthPolicy::Required) ||
             !IsFiniteInRange(softDepthValue.fadeDistanceMeters, 0.001f, 1000.0f) ||
             !IsFiniteInRange(softDepthValue.contrastPower, 0.001f, 16.0f))
    {
        outError = "Particle soft depth policy is unsupported or invalid";
        return false;
    }

    if (!IsFiniteInRange(distortion.distortionStrengthPixels, 0.0f, 64.0f) ||
        !IsFiniteInRange(reactiveMask.reactiveContribution, 0.0f, 1.0f))
    {
        outError = "Particle effect distortion or reactive-mask parameters are invalid";
        return false;
    }

    const CookedParticleEffectLightHint& light = lightHint;
    if (!light.enabled)
    {
        if (!IsZero(light.color) || light.intensityCandela != 0.0f ||
            light.rangeMeters != 0.0f || light.flickerFrequencyHz != 0.0f)
        {
            outError = "Disabled particle light hint must have canonical zero parameters";
            return false;
        }
    }
    else if (!IsFinite(light.color) || !IsFiniteInRange(light.color.x, 0.0f, MaxFiniteMagnitude) ||
             !IsFiniteInRange(light.color.y, 0.0f, MaxFiniteMagnitude) ||
             !IsFiniteInRange(light.color.z, 0.0f, MaxFiniteMagnitude) ||
             !IsFiniteInRange(light.intensityCandela, 0.001f, MaxFiniteMagnitude) ||
             !IsFiniteInRange(light.rangeMeters, 0.001f, MaxFiniteMagnitude) ||
             !IsFiniteInRange(light.flickerFrequencyHz, 0.1f, 10.0f))
    {
        outError = "Particle light hint must be finite, bounded, and low frequency";
        return false;
    }

    outError.clear();
    return true;
}

bool SerializeCookedParticleEffectArtifact(
    const CookedParticleEffectArtifact& artifact,
    std::vector<uint8>& outBytes,
    std::string& outError)
{
    outBytes.clear();
    if (!artifact.IsValid(outError))
    {
        return false;
    }

    CookedArtifactIO::Writer writer;
    if (!writer.String(artifact.logicalAssetId, outError) ||
        !writer.String(artifact.flipbook.textureArtifactPath, outError))
    {
        return false;
    }
    writer.U8(static_cast<uint8>(artifact.blendMode));
    writer.U8(artifact.emitter.looping ? 1u : 0u);
    writer.U8(static_cast<uint8>(artifact.softDepth.policy));
    writer.U8(artifact.lightHint.enabled ? 1u : 0u);
    writer.Bytes(artifact.flipbook.expectedContentHash);
    writer.U32(artifact.emitter.maxParticles);
    writer.Float(artifact.emitter.durationSeconds);
    writer.Float(artifact.emitter.emissionRate);
    WriteVec3(writer, artifact.emitter.position);
    writer.Float(artifact.emitter.speedMin);
    writer.Float(artifact.emitter.speedMax);
    writer.Float(artifact.emitter.lifetimeMin);
    writer.Float(artifact.emitter.lifetimeMax);
    WriteVec4(writer, artifact.emitter.initialColor);
    writer.Float(artifact.emitter.sizeMin);
    writer.Float(artifact.emitter.sizeMax);
    writer.Float(artifact.emitter.rotationDegreesMin);
    writer.Float(artifact.emitter.rotationDegreesMax);
    writer.Float(artifact.emitter.rotationSpeedDegreesMin);
    writer.Float(artifact.emitter.rotationSpeedDegreesMax);
    writer.U32(artifact.flipbook.tilesX);
    writer.U32(artifact.flipbook.tilesY);
    writer.U32(artifact.flipbook.firstFrame);
    writer.U32(artifact.flipbook.frameCount);
    writer.Float(artifact.flipbook.framesPerSecond);
    WriteVec2(writer, artifact.flipbook.uvOffset);
    WriteVec2(writer, artifact.flipbook.uvScale);
    writer.Float(artifact.softDepth.fadeDistanceMeters);
    writer.Float(artifact.softDepth.contrastPower);
    writer.Float(artifact.distortion.distortionStrengthPixels);
    writer.Float(artifact.reactiveMask.reactiveContribution);
    WriteVec3(writer, artifact.lightHint.color);
    writer.Float(artifact.lightHint.intensityCandela);
    writer.Float(artifact.lightHint.rangeMeters);
    writer.Float(artifact.lightHint.flickerFrequencyHz);

    return CookedArtifactIO::WriteEnvelope(
        RVX_PARTICLE_EFFECT_ARTIFACT_MAGIC,
        RVX_PARTICLE_EFFECT_ARTIFACT_SCHEMA_VERSION,
        writer.Data(),
        outBytes,
        outError);
}

bool DeserializeCookedParticleEffectArtifact(
    std::span<const uint8> bytes,
    CookedParticleEffectArtifact& outArtifact,
    std::string& outError)
{
    outArtifact = {};
    std::span<const uint8> payloadBytes;
    Hash::SHA256Digest payloadHash{};
    if (!CookedArtifactIO::ReadEnvelope(
            bytes,
            RVX_PARTICLE_EFFECT_ARTIFACT_MAGIC,
            RVX_PARTICLE_EFFECT_ARTIFACT_SCHEMA_VERSION,
            payloadBytes,
            payloadHash,
            outError))
    {
        return false;
    }

    CookedArtifactIO::Reader reader(payloadBytes);
    CookedParticleEffectArtifact parsed;
    uint8 blendMode = 0;
    uint8 looping = 0;
    uint8 softDepthPolicy = 0;
    uint8 lightEnabled = 0;
    std::span<const uint8> expectedTextureHash;
    if (!reader.String(parsed.logicalAssetId) ||
        !reader.String(parsed.flipbook.textureArtifactPath) ||
        !reader.U8(blendMode) || blendMode > static_cast<uint8>(CookedParticleEffectBlendMode::Additive) ||
        !reader.U8(looping) || looping > 1u ||
        !reader.U8(softDepthPolicy) ||
            softDepthPolicy > static_cast<uint8>(CookedParticleEffectSoftDepthPolicy::Required) ||
        !reader.U8(lightEnabled) || lightEnabled > 1u ||
        !reader.Bytes(Hash::RVX_SHA256_DIGEST_SIZE, expectedTextureHash) ||
        !reader.U32(parsed.emitter.maxParticles) ||
        !reader.Float(parsed.emitter.durationSeconds) ||
        !reader.Float(parsed.emitter.emissionRate) ||
        !ReadVec3(reader, parsed.emitter.position) ||
        !reader.Float(parsed.emitter.speedMin) ||
        !reader.Float(parsed.emitter.speedMax) ||
        !reader.Float(parsed.emitter.lifetimeMin) ||
        !reader.Float(parsed.emitter.lifetimeMax) ||
        !ReadVec4(reader, parsed.emitter.initialColor) ||
        !reader.Float(parsed.emitter.sizeMin) ||
        !reader.Float(parsed.emitter.sizeMax) ||
        !reader.Float(parsed.emitter.rotationDegreesMin) ||
        !reader.Float(parsed.emitter.rotationDegreesMax) ||
        !reader.Float(parsed.emitter.rotationSpeedDegreesMin) ||
        !reader.Float(parsed.emitter.rotationSpeedDegreesMax) ||
        !reader.U32(parsed.flipbook.tilesX) ||
        !reader.U32(parsed.flipbook.tilesY) ||
        !reader.U32(parsed.flipbook.firstFrame) ||
        !reader.U32(parsed.flipbook.frameCount) ||
        !reader.Float(parsed.flipbook.framesPerSecond) ||
        !ReadVec2(reader, parsed.flipbook.uvOffset) ||
        !ReadVec2(reader, parsed.flipbook.uvScale) ||
        !reader.Float(parsed.softDepth.fadeDistanceMeters) ||
        !reader.Float(parsed.softDepth.contrastPower) ||
        !reader.Float(parsed.distortion.distortionStrengthPixels) ||
        !reader.Float(parsed.reactiveMask.reactiveContribution) ||
        !ReadVec3(reader, parsed.lightHint.color) ||
        !reader.Float(parsed.lightHint.intensityCandela) ||
        !reader.Float(parsed.lightHint.rangeMeters) ||
        !reader.Float(parsed.lightHint.flickerFrequencyHz) ||
        reader.Remaining() != 0u)
    {
        outError = "Cooked particle effect payload is truncated, malformed, or has trailing bytes";
        return false;
    }

    parsed.blendMode = static_cast<CookedParticleEffectBlendMode>(blendMode);
    parsed.emitter.looping = looping != 0u;
    parsed.softDepth.policy = static_cast<CookedParticleEffectSoftDepthPolicy>(softDepthPolicy);
    parsed.lightHint.enabled = lightEnabled != 0u;
    std::copy(expectedTextureHash.begin(), expectedTextureHash.end(),
              parsed.flipbook.expectedContentHash.begin());
    parsed.payloadHash = payloadHash;
    if (IsZeroDigest(parsed.payloadHash) || !parsed.IsValid(outError))
    {
        if (outError.empty())
        {
            outError = "Cooked particle effect payload hash is invalid";
        }
        return false;
    }
    outArtifact = std::move(parsed);
    outError.clear();
    return true;
}
} // namespace RVX::Resource
