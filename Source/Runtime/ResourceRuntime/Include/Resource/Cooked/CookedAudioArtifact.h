#pragma once

/** @file CookedAudioArtifact.h @brief Deterministic PCM16 audio product */

#include "Core/Hash/SHA256.h"
#include "Core/Types.h"

#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr char RVX_AUDIO_ARTIFACT_MAGIC[] =
        "RVX_AUDIO_ARTIFACT_V2";
    inline constexpr uint32 RVX_AUDIO_ARTIFACT_SCHEMA_VERSION = 2;

    struct CookedAudioArtifact
    {
        std::string sourcePath;
        uint32 sampleRate = 0;
        uint32 channels = 0;
        uint64 frameCount = 0;
        bool looping = false;
        uint64 loopStartFrame = 0;
        uint64 loopEndFrame = 0;
        std::vector<int16> pcm16;
        Hash::SHA256Digest payloadHash{};

        [[nodiscard]] bool IsValid(std::string& outError) const;
    };

    [[nodiscard]] bool SerializeCookedAudioArtifact(
        const CookedAudioArtifact& artifact,
        std::vector<uint8>& outBytes,
        std::string& outError);

    [[nodiscard]] bool DeserializeCookedAudioArtifact(
        std::span<const uint8> bytes,
        CookedAudioArtifact& outArtifact,
        std::string& outError);
} // namespace RVX::Resource
