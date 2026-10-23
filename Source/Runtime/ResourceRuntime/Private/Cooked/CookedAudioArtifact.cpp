/** @file CookedAudioArtifact.cpp @brief Deterministic PCM16 artifact IO */

#include "Resource/Cooked/CookedAudioArtifact.h"

#include "CookedArtifactIO.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace RVX::Resource
{
namespace
{
    constexpr uint32 MaxAudioChannels = 32;
    constexpr uint32 MaxAudioSampleRate = 768000;

    bool IsZeroDigest(const Hash::SHA256Digest& digest)
    {
        return std::all_of(digest.begin(), digest.end(),
                           [](uint8 value) { return value == 0; });
    }
} // namespace

bool CookedAudioArtifact::IsValid(std::string& outError) const
{
    if (sourcePath.empty() || sourcePath.find('\0') != std::string::npos ||
        sampleRate == 0 || sampleRate > MaxAudioSampleRate || channels == 0 ||
        channels > MaxAudioChannels || frameCount == 0)
    {
        outError = "Cooked audio metadata is invalid";
        return false;
    }
    if (frameCount > std::numeric_limits<size_t>::max() / channels ||
        static_cast<size_t>(frameCount) * channels != pcm16.size())
    {
        outError = "Cooked audio PCM16 payload size does not match frame metadata";
        return false;
    }
    if ((!looping && (loopStartFrame != 0 || loopEndFrame != 0)) ||
        (looping && (loopStartFrame >= loopEndFrame ||
                     loopEndFrame > frameCount)))
    {
        outError = "Cooked audio loop metadata is invalid";
        return false;
    }
    outError.clear();
    return true;
}

bool SerializeCookedAudioArtifact(
    const CookedAudioArtifact& artifact,
    std::vector<uint8>& outBytes,
    std::string& outError)
{
    outBytes.clear();
    if (!artifact.IsValid(outError))
        return false;

    CookedArtifactIO::Writer payload;
    if (!payload.String(artifact.sourcePath, outError))
        return false;
    payload.U32(artifact.sampleRate);
    payload.U32(artifact.channels);
    payload.U64(artifact.frameCount);
    payload.U8(artifact.looping ? 1u : 0u);
    payload.U64(artifact.loopStartFrame);
    payload.U64(artifact.loopEndFrame);
    payload.U64(static_cast<uint64>(artifact.pcm16.size()));
    for (int16 sample : artifact.pcm16)
        payload.U16(std::bit_cast<uint16>(sample));

    return CookedArtifactIO::WriteEnvelope(
        RVX_AUDIO_ARTIFACT_MAGIC,
        RVX_AUDIO_ARTIFACT_SCHEMA_VERSION,
        payload.Data(),
        outBytes,
        outError);
}

bool DeserializeCookedAudioArtifact(
    std::span<const uint8> bytes,
    CookedAudioArtifact& outArtifact,
    std::string& outError)
{
    outArtifact = {};
    std::span<const uint8> payloadBytes;
    Hash::SHA256Digest payloadHash{};
    if (!CookedArtifactIO::ReadEnvelope(
            bytes,
            RVX_AUDIO_ARTIFACT_MAGIC,
            RVX_AUDIO_ARTIFACT_SCHEMA_VERSION,
            payloadBytes,
            payloadHash,
            outError))
    {
        return false;
    }

    CookedArtifactIO::Reader payload(payloadBytes);
    CookedAudioArtifact parsed;
    uint8 looping = 0;
    uint64 sampleCount = 0;
    if (!payload.String(parsed.sourcePath) ||
        !payload.U32(parsed.sampleRate) ||
        !payload.U32(parsed.channels) ||
        !payload.U64(parsed.frameCount) ||
        !payload.U8(looping) || looping > 1 ||
        !payload.U64(parsed.loopStartFrame) ||
        !payload.U64(parsed.loopEndFrame) ||
        !payload.U64(sampleCount) ||
        sampleCount > std::numeric_limits<size_t>::max() ||
        sampleCount > payload.Remaining() / sizeof(uint16))
    {
        outError = "Cooked audio payload metadata is truncated or invalid";
        return false;
    }
    parsed.looping = looping != 0;
    parsed.pcm16.resize(static_cast<size_t>(sampleCount));
    for (int16& sample : parsed.pcm16)
    {
        uint16 bits = 0;
        if (!payload.U16(bits))
        {
            outError = "Cooked audio PCM16 payload is truncated";
            return false;
        }
        sample = std::bit_cast<int16>(bits);
    }
    if (payload.Remaining() != 0)
    {
        outError = "Cooked audio payload has trailing bytes";
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
