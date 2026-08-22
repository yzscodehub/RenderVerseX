#pragma once

/**
 * @file RenderPipelineProfile.h
 * @brief Versioned, backend-neutral renderer recipe and quality contracts.
 */

#include "Core/Types.h"

#include <string_view>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION = 1;

    /** @brief Renderer composition selected independently of draw submission. */
    enum class RenderPipelineRecipe : uint8
    {
        Minimal = 0,
        ForwardStandard = 1,
        DeferredQuality = 2,
        HybridRayTracing = 3,
        Headless = 4,
        Count = 5,
    };

    /** @brief Stable renderer quality tier selected independently of recipe. */
    enum class RenderQualityTier : uint8
    {
        Low = 0,
        Medium = 1,
        High = 2,
        Ultra = 3,
        Count = 4,
    };

    /** @brief Return the stable machine spelling for one renderer recipe. */
    [[nodiscard]] inline constexpr const char* GetRenderPipelineRecipeName(
        RenderPipelineRecipe recipe) noexcept
    {
        switch (recipe)
        {
            case RenderPipelineRecipe::Minimal: return "minimal";
            case RenderPipelineRecipe::ForwardStandard: return "forward-plus";
            case RenderPipelineRecipe::DeferredQuality: return "deferred";
            case RenderPipelineRecipe::HybridRayTracing: return "hybrid-rt";
            case RenderPipelineRecipe::Headless: return "headless";
            case RenderPipelineRecipe::Count: return "invalid";
            default: return "invalid";
        }
    }

    /** @brief Parse one stable renderer recipe spelling. */
    [[nodiscard]] inline constexpr bool ParseRenderPipelineRecipe(
        std::string_view text,
        RenderPipelineRecipe& outRecipe) noexcept
    {
        if (text == "minimal")
        {
            outRecipe = RenderPipelineRecipe::Minimal;
            return true;
        }
        if (text == "forward-plus")
        {
            outRecipe = RenderPipelineRecipe::ForwardStandard;
            return true;
        }
        if (text == "deferred")
        {
            outRecipe = RenderPipelineRecipe::DeferredQuality;
            return true;
        }
        if (text == "hybrid-rt")
        {
            outRecipe = RenderPipelineRecipe::HybridRayTracing;
            return true;
        }
        if (text == "headless")
        {
            outRecipe = RenderPipelineRecipe::Headless;
            return true;
        }
        return false;
    }

    /** @brief Return the stable machine spelling for one renderer quality tier. */
    [[nodiscard]] inline constexpr const char* GetRenderQualityTierName(
        RenderQualityTier tier) noexcept
    {
        switch (tier)
        {
            case RenderQualityTier::Low: return "low";
            case RenderQualityTier::Medium: return "medium";
            case RenderQualityTier::High: return "high";
            case RenderQualityTier::Ultra: return "ultra";
            case RenderQualityTier::Count: return "invalid";
            default: return "invalid";
        }
    }

    /** @brief Parse one stable renderer quality tier spelling. */
    [[nodiscard]] inline constexpr bool ParseRenderQualityTier(
        std::string_view text,
        RenderQualityTier& outTier) noexcept
    {
        if (text == "low")
        {
            outTier = RenderQualityTier::Low;
            return true;
        }
        if (text == "medium")
        {
            outTier = RenderQualityTier::Medium;
            return true;
        }
        if (text == "high")
        {
            outTier = RenderQualityTier::High;
            return true;
        }
        if (text == "ultra")
        {
            outTier = RenderQualityTier::Ultra;
            return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool IsDeclaredRenderPipelineRecipe(
        RenderPipelineRecipe recipe) noexcept
    {
        return recipe >= RenderPipelineRecipe::Minimal &&
               recipe < RenderPipelineRecipe::Count;
    }

    [[nodiscard]] constexpr bool IsDeclaredRenderQualityTier(
        RenderQualityTier tier) noexcept
    {
        return tier >= RenderQualityTier::Low && tier < RenderQualityTier::Count;
    }

    /** @brief Value-only recipe/profile input copied into immutable frame state. */
    struct RenderPipelineRequest
    {
        uint32 schemaVersion = RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION;
        /** Current compatibility default: the existing Forward+ renderer. */
        RenderPipelineRecipe recipe = RenderPipelineRecipe::ForwardStandard;
        /** Current compatibility default for unqualified runtime rendering. */
        RenderQualityTier quality = RenderQualityTier::Medium;

        friend constexpr bool operator==(
            const RenderPipelineRequest&,
            const RenderPipelineRequest&) = default;
    };

    /** @brief Terminal outcome for a pure recipe/profile resolution attempt. */
    enum class RenderPipelineResolutionCode : uint8
    {
        Rejected = 0,
        Resolved = 1,
        Fallback = 2,
    };

    /**
     * @brief Value-only result for a recipe/profile resolution.
     *
     * Feature-level fallback admissibility remains renderer-owned. This
     * contract only records an already-authorized fallback selection.
     */
    struct RenderPipelineResolution
    {
        uint32 schemaVersion = RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION;
        RenderPipelineResolutionCode code = RenderPipelineResolutionCode::Rejected;
        RenderPipelineRequest requested{};
        RenderPipelineRecipe resolvedRecipe =
            RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier resolvedQuality = RenderQualityTier::Medium;
        uint64 profileHash = 0;

        [[nodiscard]] constexpr bool IsSuccessful() const noexcept
        {
            return code == RenderPipelineResolutionCode::Resolved ||
                   code == RenderPipelineResolutionCode::Fallback;
        }

        friend constexpr bool operator==(
            const RenderPipelineResolution&,
            const RenderPipelineResolution&) = default;
    };

    [[nodiscard]] constexpr bool IsValidRenderPipelineRequest(
        const RenderPipelineRequest& request) noexcept
    {
        return request.schemaVersion == RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION &&
               IsDeclaredRenderPipelineRecipe(request.recipe) &&
               IsDeclaredRenderQualityTier(request.quality);
    }

    namespace Detail
    {
        inline constexpr uint64 RenderPipelineProfileHashOffset =
            14695981039346656037ULL;
        inline constexpr uint64 RenderPipelineProfileHashPrime =
            1099511628211ULL;

        [[nodiscard]] constexpr uint64 MixRenderPipelineProfileHash(
            uint64 hash,
            uint64 value) noexcept
        {
            for (uint32 byte = 0; byte < sizeof(value); ++byte)
            {
                hash ^= (value >> (byte * 8U)) & 0xffU;
                hash *= RenderPipelineProfileHashPrime;
            }
            return hash;
        }
    } // namespace Detail

    /** @brief Build a stable non-zero identity for one resolved profile. */
    [[nodiscard]] constexpr uint64 ComputeRenderPipelineProfileHash(
        RenderPipelineRecipe recipe,
        RenderQualityTier quality) noexcept
    {
        if (!IsDeclaredRenderPipelineRecipe(recipe) ||
            !IsDeclaredRenderQualityTier(quality))
        {
            return 0;
        }

        uint64 hash = Detail::RenderPipelineProfileHashOffset;
        hash = Detail::MixRenderPipelineProfileHash(
            hash, RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION);
        hash = Detail::MixRenderPipelineProfileHash(
            hash, static_cast<uint64>(recipe));
        hash = Detail::MixRenderPipelineProfileHash(
            hash, static_cast<uint64>(quality));
        return hash == 0 ? 1 : hash;
    }

    /** @brief Build the no-fallback resolution for one valid request. */
    [[nodiscard]] constexpr RenderPipelineResolution
    ResolveRenderPipelineRequest(const RenderPipelineRequest& request) noexcept
    {
        RenderPipelineResolution resolution;
        resolution.requested = request;
        if (!IsValidRenderPipelineRequest(request))
            return resolution;

        resolution.code = RenderPipelineResolutionCode::Resolved;
        resolution.resolvedRecipe = request.recipe;
        resolution.resolvedQuality = request.quality;
        resolution.profileHash = ComputeRenderPipelineProfileHash(
            resolution.resolvedRecipe, resolution.resolvedQuality);
        return resolution;
    }

    [[nodiscard]] constexpr bool IsValidRenderPipelineResolution(
        const RenderPipelineResolution& resolution) noexcept
    {
        if (resolution.schemaVersion != RVX_RENDER_PIPELINE_PROFILE_SCHEMA_VERSION)
            return false;

        if (resolution.code == RenderPipelineResolutionCode::Rejected)
        {
            return !IsValidRenderPipelineRequest(resolution.requested) &&
                   resolution.profileHash == 0;
        }
        if (resolution.code != RenderPipelineResolutionCode::Resolved &&
            resolution.code != RenderPipelineResolutionCode::Fallback)
        {
            return false;
        }
        if (!IsValidRenderPipelineRequest(resolution.requested) ||
            !IsDeclaredRenderPipelineRecipe(resolution.resolvedRecipe) ||
            !IsDeclaredRenderQualityTier(resolution.resolvedQuality) ||
            resolution.profileHash == 0)
        {
            return false;
        }

        if (resolution.code == RenderPipelineResolutionCode::Resolved &&
            (resolution.resolvedRecipe != resolution.requested.recipe ||
             resolution.resolvedQuality != resolution.requested.quality))
        {
            return false;
        }
        if (resolution.code == RenderPipelineResolutionCode::Fallback &&
            resolution.resolvedRecipe == resolution.requested.recipe &&
            resolution.resolvedQuality == resolution.requested.quality)
        {
            return false;
        }
        return resolution.profileHash == ComputeRenderPipelineProfileHash(
                   resolution.resolvedRecipe, resolution.resolvedQuality);
    }

    static_assert(static_cast<uint8>(RenderPipelineRecipe::Minimal) == 0);
    static_assert(static_cast<uint8>(RenderPipelineRecipe::ForwardStandard) == 1);
    static_assert(static_cast<uint8>(RenderPipelineRecipe::DeferredQuality) == 2);
    static_assert(static_cast<uint8>(RenderPipelineRecipe::HybridRayTracing) == 3);
    static_assert(static_cast<uint8>(RenderPipelineRecipe::Headless) == 4);
    static_assert(static_cast<uint8>(RenderQualityTier::Low) == 0);
    static_assert(static_cast<uint8>(RenderQualityTier::Medium) == 1);
    static_assert(static_cast<uint8>(RenderQualityTier::High) == 2);
    static_assert(static_cast<uint8>(RenderQualityTier::Ultra) == 3);
    static_assert(ComputeRenderPipelineProfileHash(
                      RenderPipelineRecipe::ForwardStandard,
                      RenderQualityTier::Medium) != 0);
} // namespace RVX
