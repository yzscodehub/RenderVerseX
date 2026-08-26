#pragma once

/**
 * @file PostProcessTypes.h
 * @brief Lightweight, renderer-wide contracts for typed post processing.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RHI/RHIDefinitions.h"

namespace RVX
{
    /** @brief Stable identity for a built-in post-process pass. */
    enum class PostProcessPassId : uint16
    {
        Invalid = 0,
        Bloom,
        ToneMapping,
        ColorGrading,
        ChromaticAberration,
        Vignette,
        FilmGrain,
        FXAA,
        TemporalAntiAliasing,
        DepthOfField,
        MotionBlur,
        VolumetricLighting,
        /** HDR stylized silhouette and crease pass. */
        ScreenSpaceOutline,
    };

    /** @brief Legal semantic stage for a post-process pass. */
    enum class PostProcessStage : uint8
    {
        Temporal = 0,
        HDRPost,
        ToneMap,
        LDRPost,
        Overlay,
    };

    /** @brief Explicit color meaning of a post-process resource. */
    enum class PostProcessColorDomain : uint8
    {
        Unknown = 0,
        SceneLinearHDR,
        DisplayLinearLDR,
        DisplayEncodedLDR,
    };

    /** @brief Explicit color transform performed by a pass. */
    enum class PostProcessTransfer : uint8
    {
        None = 0,
        SceneLinearToDisplayLinear,
        SceneLinearToDisplayEncoded,
        DisplayLinearToEncoded,
    };

    /** @brief Immutable semantic contract for an IPostProcessPass. */
    struct PostProcessPassDescriptor
    {
        PostProcessPassId id = PostProcessPassId::Invalid;
        const char* debugName = nullptr;
        PostProcessStage stage = PostProcessStage::HDRPost;
        int32 order = 0;
        PostProcessColorDomain inputDomain = PostProcessColorDomain::Unknown;
        PostProcessColorDomain outputDomain = PostProcessColorDomain::Unknown;
        PostProcessTransfer transfer = PostProcessTransfer::None;
    };

    /**
     * @brief Graph-independent surface contract consumed by post-process passes.
     *
     * A texture handle alone does not express its semantic layout.  This
     * value is deliberately copied from the producing blackboard entry, so a
     * post-process plan can fail before it records graph mutations.
     */
    struct PostProcessTextureMetadata
    {
        RenderOutputExtent extent{};
        RHIFormat format = RHIFormat::Unknown;
        RHISampleCount sampleCount = RHISampleCount::Count1;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return extent.IsValid() && format != RHIFormat::Unknown;
        }

        friend constexpr bool operator==(const PostProcessTextureMetadata&,
                                         const PostProcessTextureMetadata&) =
            default;
    };

    /**
     * @brief Immutable ABI/content identity for one material-parameter table.
     *
     * The table is a structured buffer indexed by the matching full-resolution
     * material-identity surface.  The content hash is supplied by the table
     * producer and prevents an otherwise compatible handle from silently
     * changing the semantic material rows selected by an outline pass.
     */
    struct PostProcessMaterialTableMetadata
    {
        static constexpr uint32 SchemaVersion = 2;

        uint32 schemaVersion = 0;
        uint32 stride = 0;
        uint32 slotCount = 0;
        /** Value-only classification rows eligible for a Toon outline. */
        uint32 eligibleToonMaterialCount = 0;
        uint64 contentHash = 0;
        /** Full-resolution scene extent the slot-index surface was produced for. */
        RenderOutputExtent sourceExtent{};
        RHISampleCount sourceSampleCount = RHISampleCount::Count1;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return schemaVersion == SchemaVersion && stride != 0 &&
                   slotCount != 0 && eligibleToonMaterialCount <= slotCount &&
                   contentHash != 0 &&
                   sourceExtent.IsValid() &&
                   sourceSampleCount == RHISampleCount::Count1;
        }

        friend constexpr bool operator==(
            const PostProcessMaterialTableMetadata&,
            const PostProcessMaterialTableMetadata&) = default;
    };

    const char* GetPostProcessPassCanonicalName(PostProcessPassId id);
} // namespace RVX
