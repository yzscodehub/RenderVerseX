#pragma once

/**
 * @file CookedAnimationArtifact.h
 * @brief Deterministic, versioned skeletal animation artifact contract.
 */

#include "Animation/Data/AnimationClip.h"
#include "Animation/Data/Skeleton.h"
#include "Core/Types.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace RVX::Resource
{
    inline constexpr char RVX_ANIMATION_ARTIFACT_MAGIC[8] = {
        'R', 'V', 'X', 'A', 'N', 'I', 'M', '\0'};
    inline constexpr uint32 RVX_ANIMATION_ARTIFACT_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_ANIMATION_ARTIFACT_BUILD_FINGERPRINT =
        "RVX_SKELETAL_ANIMATION_V1";

    /** @brief Parsed CPU-only payload carried by one .rvxanim artifact. */
    struct CookedAnimationArtifact
    {
        using AnimationClipMap =
            std::map<std::string, Animation::AnimationClip::ConstPtr>;

        std::string sourcePath;
        Animation::Skeleton::ConstPtr skeleton;
        AnimationClipMap clips;
    };

    /** @brief Validate the immutable skeletal payload shared by codec and resources. */
    bool ValidateCookedAnimationPayload(
        const Animation::Skeleton::ConstPtr& skeleton,
        const CookedAnimationArtifact::AnimationClipMap& clips,
        std::string& outError);

    /** @brief Serialize canonical little-endian bytes with a SHA-256 payload digest. */
    bool SerializeCookedAnimationArtifact(const CookedAnimationArtifact& artifact,
                                          std::vector<uint8>& outBytes,
                                          std::string& outError);

    /** @brief Parse and fully validate one canonical .rvxanim artifact. */
    bool DeserializeCookedAnimationArtifact(std::span<const uint8> bytes,
                                            CookedAnimationArtifact& outArtifact,
                                            std::string& outError);
} // namespace RVX::Resource
