#pragma once

/**
 * @file PostProcessTypes.h
 * @brief Lightweight, renderer-wide contracts for typed post processing.
 */

#include "Core/Types.h"

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

    const char* GetPostProcessPassCanonicalName(PostProcessPassId id);
} // namespace RVX
