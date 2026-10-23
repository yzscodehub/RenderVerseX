#pragma once

/**
 * @file VolumetricLightingFramePlan.h
 * @brief Immutable, bounded froxel policy for one render frame and main view.
 *
 * The plan contains no RHI objects. It derives the single froxel-grid shape,
 * shadow requirements, and temporal-history decision before a graph pass can
 * allocate or bind GPU resources. A zero-density or disabled policy is a
 * valid exact-identity plan, not a silent fallback to an unbounded ray march.
 */

#include "Core/Types.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <limits>
#include <string>

namespace RVX
{
    inline constexpr uint32 RVX_VOLUMETRIC_LIGHTING_FRAME_PLAN_SCHEMA_VERSION = 1;
    /** Upper bound keeps one rendered view inside the transient-memory budget. */
    inline constexpr uint64 RVX_VOLUMETRIC_LIGHTING_MAX_FROXEL_COUNT =
        16ULL * 1024ULL * 1024ULL;

    enum class FroxelGridResolutionCode : uint8
    {
        Ready = 0,
        InvalidExtent,
        InvalidQuality,
        ArithmeticOverflow,
        CapacityExceeded,
    };

    /**
     * @brief Exact logical dimensions of the renderer-owned froxel field.
     *
     * The XY dimensions use a ceil division of the full output extent by the
     * selected quality tile size. The Z count is fixed by quality so shader,
     * memory, and qualification budgets agree across backends.
     */
    struct FroxelGridMetadata
    {
        RenderOutputExtent outputExtent{};
        uint32 tileSizePixels = 0;
        uint32 froxelWidth = 0;
        uint32 froxelHeight = 0;
        uint32 depthSliceCount = 0;
        uint64 froxelCount = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            const bool knownShape =
                (tileSizePixels == 16U && depthSliceCount == 32U) ||
                (tileSizePixels == 12U && depthSliceCount == 48U) ||
                (tileSizePixels == 8U && depthSliceCount == 64U) ||
                (tileSizePixels == 8U && depthSliceCount == 96U);
            if (!outputExtent.IsValid() || !knownShape)
            {
                return false;
            }

            const auto ceilDivide = [](uint32 value, uint32 divisor) noexcept
            {
                return value / divisor + (value % divisor != 0 ? 1U : 0U);
            };
            if (froxelWidth != ceilDivide(outputExtent.width, tileSizePixels) ||
                froxelHeight != ceilDivide(outputExtent.height, tileSizePixels) ||
                froxelWidth == 0 || froxelHeight == 0 || depthSliceCount == 0)
            {
                return false;
            }

            constexpr uint64 kMax = std::numeric_limits<uint64>::max();
            if (static_cast<uint64>(froxelHeight) >
                    kMax / static_cast<uint64>(froxelWidth))
            {
                return false;
            }
            const uint64 xyCount =
                static_cast<uint64>(froxelWidth) * froxelHeight;
            if (static_cast<uint64>(depthSliceCount) > kMax / xyCount)
            {
                return false;
            }
            const uint64 expectedCount = xyCount * depthSliceCount;
            return expectedCount <= RVX_VOLUMETRIC_LIGHTING_MAX_FROXEL_COUNT &&
                   froxelCount == expectedCount;
        }

        friend bool operator==(const FroxelGridMetadata&,
                               const FroxelGridMetadata&) = default;
    };

    struct FroxelGridResolution
    {
        FroxelGridResolutionCode code = FroxelGridResolutionCode::InvalidExtent;
        FroxelGridMetadata metadata{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == FroxelGridResolutionCode::Ready && metadata.IsValid();
        }
    };

    /** Resolve the fixed, bounded grid for one output extent and quality tier. */
    [[nodiscard]] FroxelGridResolution ResolveFroxelGridMetadata(
        RenderOutputExtent outputExtent,
        RenderQualityTier quality) noexcept;

    /**
     * @brief Derived lighting and shadow facts frozen into a non-identity plan.
     *
     * A required shadow source may be unavailable while preparing a request,
     * but the builder then fails rather than publishing an unshadowed plan
     * under a shadowed contract. Identity plans clear all of these fields and
     * therefore consume no lighting or shadow resource.
     */
    struct VolumetricLightingInputReadiness
    {
        bool directionalLightAvailable = false;
        uint32 localLightCount = 0;
        uint32 shadowedLocalLightCount = 0;
        bool directionalShadowRequired = false;
        bool directionalShadowReady = false;
        uint64 directionalShadowIdentityHash = 0;
        bool localShadowRequired = false;
        bool localShadowReady = false;
        uint64 localShadowIdentityHash = 0;

        [[nodiscard]] bool IsStructurallyValid() const noexcept;

        friend bool operator==(const VolumetricLightingInputReadiness&,
                               const VolumetricLightingInputReadiness&) = default;
    };

    enum class VolumetricLightingFramePlanCode : uint8
    {
        Ready = 0,
        Identity,
        InvalidSchema,
        InvalidFrameIdentity,
        InvalidViewIdentity,
        InvalidOutputExtent,
        InvalidQuality,
        InvalidSettings,
        InvalidLightingPlan,
        LightingPlanIdentityMismatch,
        FroxelGridArithmeticOverflow,
        FroxelGridCapacityExceeded,
        InvalidHistoryResetReasons,
        InvalidHistoryIdentity,
        HistoryIdentityIncompatible,
        DirectionalShadowUnavailable,
        LocalShadowUnavailable,
        IdentityMismatch,
    };

    /** Complete value-only input to VolumetricLightingFramePlanBuilder. */
    struct VolumetricLightingFramePlanRequest
    {
        uint32 schemaVersion = RVX_VOLUMETRIC_LIGHTING_FRAME_PLAN_SCHEMA_VERSION;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        RenderOutputExtent outputExtent{};
        RenderQualityTier quality = RenderQualityTier::Medium;
        RenderVolumetricLightingSettings settings{};
        /** Optional stale-lighting guard. Zero accepts the sealed source plan. */
        uint64 expectedLightingPlanIdentity = 0;
        /** Exact source evidence for the directional CSM/shadow map. */
        bool directionalShadowReady = false;
        uint64 directionalShadowIdentityHash = 0;
        /** Exact source evidence for the local-light shadow atlas. */
        bool localShadowReady = false;
        uint64 localShadowIdentityHash = 0;
        /** Optional deterministic-output pin. Zero accepts a newly-derived plan. */
        uint64 expectedIdentityHash = 0;
    };

    class VolumetricLightingFramePlanBuilder;

    /**
     * @brief Sealed, backend-neutral froxel configuration for one frame/view.
     *
     * Accessors expose only values, so a pass cannot mutate a published plan
     * after composition has already selected its resources and history route.
     */
    class VolumetricLightingFramePlan final
    {
    public:
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsIdentityOutput() const noexcept;
        [[nodiscard]] bool UsesTemporalHistory() const noexcept;
        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept;
        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept;
        [[nodiscard]] const RenderViewHistoryIdentity& GetHistoryIdentity() const noexcept;
        [[nodiscard]] RenderHistoryResetReason GetHistoryResetReasons() const noexcept;
        [[nodiscard]] RenderQualityTier GetQuality() const noexcept;
        [[nodiscard]] const FroxelGridMetadata& GetFroxelGrid() const noexcept;
        [[nodiscard]] const VolumetricLightingInputReadiness& GetInputReadiness()
            const noexcept;
        [[nodiscard]] const RenderVolumetricLightingSettings& GetSettings()
            const noexcept;
        [[nodiscard]] uint64 GetSettingsHash() const noexcept;
        [[nodiscard]] uint64 GetLightingPlanIdentity() const noexcept;
        [[nodiscard]] uint64 GetIdentityHash() const noexcept;

    private:
        friend class VolumetricLightingFramePlanBuilder;

        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        RenderViewHistoryIdentity m_historyIdentity{};
        RenderHistoryResetReason m_historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        RenderQualityTier m_quality = RenderQualityTier::Medium;
        FroxelGridMetadata m_froxelGrid{};
        VolumetricLightingInputReadiness m_inputReadiness{};
        RenderVolumetricLightingSettings m_settings{};
        uint64 m_settingsHash = 0;
        uint64 m_lightingPlanIdentity = 0;
        uint64 m_identityHash = 0;
        bool m_identityOutput = true;
        bool m_usesTemporalHistory = false;
    };

    struct VolumetricLightingFramePlanBuildResult
    {
        VolumetricLightingFramePlanCode code =
            VolumetricLightingFramePlanCode::InvalidSchema;
        VolumetricLightingFramePlan plan{};
        std::string diagnostic{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return (code == VolumetricLightingFramePlanCode::Ready ||
                    code == VolumetricLightingFramePlanCode::Identity) &&
                   plan.IsValid();
        }
    };

    /** Stateless deterministic builder that seals froxel and history policy. */
    class VolumetricLightingFramePlanBuilder final
    {
    public:
        [[nodiscard]] VolumetricLightingFramePlanBuildResult Build(
            const LightingFramePlan& lightingPlan,
            const VolumetricLightingFramePlanRequest& request) const;
    };

    [[nodiscard]] const char* GetFroxelGridResolutionCodeName(
        FroxelGridResolutionCode code) noexcept;
    [[nodiscard]] const char* GetVolumetricLightingFramePlanCodeName(
        VolumetricLightingFramePlanCode code) noexcept;
} // namespace RVX
