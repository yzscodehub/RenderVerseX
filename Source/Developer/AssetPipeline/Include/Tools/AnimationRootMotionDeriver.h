#pragma once

/**
 * @file AnimationRootMotionDeriver.h
 * @brief Deterministically derives the approved Casual Female walk root-motion clip.
 */

#include "Core/Types.h"

#include <filesystem>
#include <string>

namespace RVX::Tools
{
    namespace fs = std::filesystem;

    /** @brief Version of this standalone derivation/publisher tool contract. */
    inline constexpr char RVX_ANIMATION_ROOT_MOTION_DERIVE_TOOL_VERSION[] = "1.0.0";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PARENT_ASSET_ID[] =
        "casual-female";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PARENT_CONTENT_ID[] =
        "rvx-asset-content-v1:sha256:42dec4e0c8ed81f540f666943f7823e500f53d22c9eccc7a9750cc9c1324dee5:3595794:5";
    inline constexpr uint64 RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_BYTE_COUNT =
        927'584;
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SHA256[] =
        "36f4824d1d82aeaae42ce33208b3c774622e09d4547290c89ace8560eeb3ad11";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PARENT_ANIMATION_SOURCE_PATH[] =
        "cooked/casual-female/models/casual-female/"
        "Casual_Female.rvdeps/animation.rvxanim";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PACKAGE_ARTIFACT_PATH[] =
        "casual-female-walk-root-motion.rvxanim";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_PACKAGE_MANIFEST_PATH[] =
        "CookManifest.rvxmanifest";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_SOURCE_CLIP[] = "Walk";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_OUTPUT_CLIP[] =
        "casual-female-walk-root-motion";
    inline constexpr char RVX_ROOT_MOTION_DERIVE_ROOT_BONE[] =
        "CharacterArmature/Bone/Body/Hips";
    inline constexpr int64 RVX_ROOT_MOTION_DERIVE_DURATION_US =
        1'250'000;

    /** @brief The sole supported translation policy for this approved derivation. */
    enum class AnimationRootMotionDeriveAxis : uint8
    {
        PositiveZ = 0
    };

    /** @brief Structured form of the catalog-owned parent AssetContentId. */
    struct AnimationRootMotionParentContentId
    {
        uint32 schemaVersion = 1;
        std::string algorithm = "sha256";
        std::string digest =
            "42dec4e0c8ed81f540f666943f7823e500f53d22c9eccc7a9750cc9c1324dee5";
        uint64 byteCount = 3'595'794;
        uint32 fileCount = 5;
    };

    /** @brief Complete, intentionally narrow request for one derived .rvxanim. */
    struct AnimationRootMotionDeriveRequest
    {
        fs::path inputPath;
        fs::path outputPath;
        std::string sourceClip = RVX_ROOT_MOTION_DERIVE_SOURCE_CLIP;
        std::string outputClip = RVX_ROOT_MOTION_DERIVE_OUTPUT_CLIP;
        std::string rootBone = RVX_ROOT_MOTION_DERIVE_ROOT_BONE;
        AnimationRootMotionDeriveAxis axis =
            AnimationRootMotionDeriveAxis::PositiveZ;
        float32 speed = 1.0f;
        int64 duration = RVX_ROOT_MOTION_DERIVE_DURATION_US;
        std::string parentAssetId = RVX_ROOT_MOTION_DERIVE_PARENT_ASSET_ID;
        AnimationRootMotionParentContentId parentContentId;
    };

    /** @brief Stable, catalog-compatible serialization of the parent content ID. */
    [[nodiscard]] std::string FormatAnimationRootMotionParentContentId(
        const AnimationRootMotionParentContentId& contentId);

    /** @brief Deterministic receipt produced after a successful publication. */
    struct AnimationRootMotionDeriveResult
    {
        std::string inputSha256;
        std::string recipe;
        std::string recipeSha256;
    };

    /**
     * @brief Narrow publication request for a child package derived from the pinned parent.
     *
     * @p derivation.inputPath must be the pinned animation under @p sourceRoot;
     * @p derivation.outputPath is reserved for the publisher and must be empty.
     */
    struct AnimationRootMotionPackagePublishRequest
    {
        fs::path sourceRoot;
        fs::path packagePath;
        AnimationRootMotionDeriveRequest derivation;
    };

    /** @brief Identities recorded after the staged package has passed Resource admission. */
    struct AnimationRootMotionPackagePublishResult
    {
        AnimationRootMotionDeriveResult derivation;
        uint64 artifactByteCount = 0;
        std::string artifactSha256;
        std::string cookSettingsHash;
        std::string manifestRecipeHash;
        std::string cookedContentSha256;
        std::string manifestContentSha256;
    };

    /**
     * @brief Create the isolated root-motion artifact and atomically replace its output.
     *
     * The request is deliberately pinned to the approved Casual Female Walk clip.  The
     * implementation reads no more than 256 MiB, validates before staging, and never
     * modifies the parent artifact or a failed output.
     */
    bool DeriveAnimationRootMotion(
        const AnimationRootMotionDeriveRequest& request,
        AnimationRootMotionDeriveResult& outResult,
        std::string& outError);

    /**
     * @brief Atomically publish a validated two-file child package.
     *
     * The child package contains only the derived .rvxanim and its v2 cook
     * manifest. The source parent is mounted independently from @p sourceRoot,
     * avoiding a package/content hash cycle.
     */
    bool PublishAnimationRootMotionPackage(
        const AnimationRootMotionPackagePublishRequest& request,
        AnimationRootMotionPackagePublishResult& outResult,
        std::string& outError);
} // namespace RVX::Tools
