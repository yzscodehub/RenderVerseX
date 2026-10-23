#pragma once

/**
 * @file HybridRayTracingFramePlan.h
 * @brief Immutable per-view policy and execution evidence for hybrid RT.
 *
 * This contract deliberately contains no RHI objects.  Composition resolves
 * one frozen plan before any pass records work; submission then projects the
 * observed work into a receipt without re-evaluating optional fallbacks.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <string>

namespace RVX
{
    enum class HybridRayTracingRequirement : uint8
    {
        Optional = 0,
        Required,
    };

    enum class HybridRayTracingShadowRoute : uint8
    {
        Disabled = 0,
        RayTraced,
        RasterCSM,
        Failed,
    };

    enum class HybridRayTracingReflectionRoute : uint8
    {
        Disabled = 0,
        RayTraced,
        ScreenSpaceReflection,
        ReflectionProbe,
        ImageBasedLighting,
        Failed,
    };

    /**
     * @brief Independent truth dimensions for one ray-traced effect.
     *
     * `compiled` and `hardwareSupported` are intentionally separate from
     * `dispatchReady`: a compiled shader on compatible hardware is not an
     * implementation claim until scene inputs and the concrete pipeline are
     * ready for the frozen frame.
     */
    struct HybridRayTracingEffectReadiness
    {
        bool requested = false;
        bool compiled = false;
        bool hardwareSupported = false;
        bool implementationSupported = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool dispatchReady = false;
        std::string diagnostic{};

        [[nodiscard]] bool IsReadyToDispatch() const noexcept;
        friend bool operator==(const HybridRayTracingEffectReadiness&,
                               const HybridRayTracingEffectReadiness&) = default;
    };

    /** @brief Exact ceil(full / 2) extent used by the hybrid RT reflection path. */
    struct HybridRayTracingHalfResolution
    {
        RenderOutputExtent fullResolution{};
        RenderOutputExtent halfResolution{};

        [[nodiscard]] bool IsValid() const noexcept;
        friend bool operator==(const HybridRayTracingHalfResolution&,
                               const HybridRayTracingHalfResolution&) = default;
    };

    /**
     * @brief Maps a valid full-resolution extent to the mandatory RT extent.
     *
     * Odd dimensions deliberately round upward: a 1921x1081 view maps to
     * 961x541.  Zero dimensions stay invalid rather than being silently
     * coerced to one pixel.
     */
    [[nodiscard]] HybridRayTracingHalfResolution
    ResolveHybridRayTracingHalfResolution(RenderOutputExtent fullResolution) noexcept;

    struct HybridRayTracingFramePlanRequest
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderViewHistoryIdentity reflectionHistoryIdentity{};
        RenderHistoryResetReason reflectionHistoryReset =
            RenderHistoryResetReason::None;
        RenderOutputExtent outputExtent{};
        RenderPipelineRecipe recipe = RenderPipelineRecipe::HybridRayTracing;
        RenderQualityTier quality = RenderQualityTier::High;
        uint64 profileHash = 0;
        uint64 rayTracingScenePreparationGeneration = 0;
        uint64 rayTracingSceneStructuralHash = 0;
        HybridRayTracingRequirement shadowRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingRequirement reflectionRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingEffectReadiness shadow{};
        HybridRayTracingEffectReadiness reflection{};
        bool rasterCSMReady = false;
        bool screenSpaceReflectionReady = false;
        bool reflectionProbeReady = false;
        bool imageBasedLightingReady = false;
    };

    /**
     * @brief Frozen RT/fallback decision for exactly one frame and main view.
     *
     * Plans are value snapshots.  The only construction path evaluates all
     * fallback decisions once, making a required request fail closed and an
     * optional reflection resolve strictly SSR -> Probe -> IBL.
     */
    class HybridRayTracingFramePlan final
    {
    public:
        [[nodiscard]] static HybridRayTracingFramePlan Build(
            const HybridRayTracingFramePlanRequest& request);

        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept
        {
            return m_frameIdentity;
        }

        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept
        {
            return m_viewIdentity;
        }

        [[nodiscard]] const RenderViewHistoryIdentity& GetReflectionHistoryIdentity()
            const noexcept
        {
            return m_reflectionHistoryIdentity;
        }

        [[nodiscard]] RenderHistoryResetReason GetReflectionHistoryReset() const
            noexcept
        {
            return m_reflectionHistoryReset;
        }

        [[nodiscard]] const HybridRayTracingHalfResolution& GetReflectionExtent()
            const noexcept
        {
            return m_reflectionExtent;
        }

        [[nodiscard]] RenderPipelineRecipe GetRecipe() const noexcept
        {
            return m_recipe;
        }

        [[nodiscard]] RenderQualityTier GetQuality() const noexcept
        {
            return m_quality;
        }

        [[nodiscard]] uint64 GetProfileHash() const noexcept
        {
            return m_profileHash;
        }

        [[nodiscard]] uint64 GetRayTracingScenePreparationGeneration() const noexcept
        {
            return m_rayTracingScenePreparationGeneration;
        }

        [[nodiscard]] uint64 GetRayTracingSceneStructuralHash() const noexcept
        {
            return m_rayTracingSceneStructuralHash;
        }

        [[nodiscard]] uint64 GetIdentityHash() const noexcept
        {
            return m_identityHash;
        }

        [[nodiscard]] HybridRayTracingRequirement GetShadowRequirement() const noexcept
        {
            return m_shadowRequirement;
        }

        [[nodiscard]] HybridRayTracingRequirement GetReflectionRequirement() const noexcept
        {
            return m_reflectionRequirement;
        }

        [[nodiscard]] const HybridRayTracingEffectReadiness& GetShadowReadiness()
            const noexcept
        {
            return m_shadow;
        }

        [[nodiscard]] const HybridRayTracingEffectReadiness& GetReflectionReadiness()
            const noexcept
        {
            return m_reflection;
        }

        [[nodiscard]] bool IsRasterCSMReady() const noexcept
        {
            return m_rasterCSMReady;
        }

        [[nodiscard]] bool IsScreenSpaceReflectionReady() const noexcept
        {
            return m_screenSpaceReflectionReady;
        }

        [[nodiscard]] bool IsReflectionProbeReady() const noexcept
        {
            return m_reflectionProbeReady;
        }

        [[nodiscard]] bool IsImageBasedLightingReady() const noexcept
        {
            return m_imageBasedLightingReady;
        }

        [[nodiscard]] HybridRayTracingShadowRoute GetShadowRoute() const noexcept
        {
            return m_shadowRoute;
        }

        [[nodiscard]] HybridRayTracingReflectionRoute GetReflectionRoute() const noexcept
        {
            return m_reflectionRoute;
        }

        [[nodiscard]] bool HasRequiredFailure() const noexcept;
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        RenderViewHistoryIdentity m_reflectionHistoryIdentity{};
        RenderHistoryResetReason m_reflectionHistoryReset =
            RenderHistoryResetReason::None;
        HybridRayTracingHalfResolution m_reflectionExtent{};
        RenderPipelineRecipe m_recipe = RenderPipelineRecipe::HybridRayTracing;
        RenderQualityTier m_quality = RenderQualityTier::High;
        uint64 m_profileHash = 0;
        uint64 m_rayTracingScenePreparationGeneration = 0;
        uint64 m_rayTracingSceneStructuralHash = 0;
        uint64 m_identityHash = 0;
        HybridRayTracingRequirement m_shadowRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingRequirement m_reflectionRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingEffectReadiness m_shadow{};
        HybridRayTracingEffectReadiness m_reflection{};
        bool m_rasterCSMReady = false;
        bool m_screenSpaceReflectionReady = false;
        bool m_reflectionProbeReady = false;
        bool m_imageBasedLightingReady = false;
        HybridRayTracingShadowRoute m_shadowRoute =
            HybridRayTracingShadowRoute::Disabled;
        HybridRayTracingReflectionRoute m_reflectionRoute =
            HybridRayTracingReflectionRoute::Disabled;
    };

    /** @brief Inputs collected after recording and exact submission acceptance. */
    struct HybridRayTracingExecutionObservation
    {
        bool graphExecutionSucceeded = false;
        bool sceneBuildAccepted = false;
        bool shadowDispatchRecorded = false;
        bool rasterCSMExecuted = false;
        bool reflectionDispatchRecorded = false;
        bool reflectionSpatialDenoiseRecorded = false;
        bool reflectionTemporalDenoiseRecorded = false;
        bool reflectionCompositeRecorded = false;
        bool reflectionCompositeUsedDenoisedSource = false;
        bool screenSpaceReflectionExecuted = false;
        bool reflectionProbeResolved = false;
        bool imageBasedLightingResolved = false;
        bool submissionAccepted = false;
        bool presentationQualified = false;
        std::string diagnostic{};
    };

    /** @brief Observed, submission-qualified immutable outcome for a frozen RT plan. */
    class HybridRayTracingExecutionReceipt final
    {
    public:
        [[nodiscard]] const RenderFrameIdentity& GetFrameIdentity() const noexcept
        {
            return m_frameIdentity;
        }

        [[nodiscard]] const RenderViewIdentity& GetViewIdentity() const noexcept
        {
            return m_viewIdentity;
        }

        [[nodiscard]] const RenderViewHistoryIdentity& GetReflectionHistoryIdentity()
            const noexcept
        {
            return m_reflectionHistoryIdentity;
        }

        [[nodiscard]] uint64 GetPlanIdentityHash() const noexcept
        {
            return m_planIdentityHash;
        }

        [[nodiscard]] HybridRayTracingShadowRoute GetShadowRoute() const noexcept
        {
            return m_shadowRoute;
        }

        [[nodiscard]] HybridRayTracingReflectionRoute GetReflectionRoute() const noexcept
        {
            return m_reflectionRoute;
        }

        [[nodiscard]] bool WasShadowDispatchRecorded() const noexcept
        {
            return m_shadowDispatchRecorded;
        }

        [[nodiscard]] bool WasReflectionDispatchRecorded() const noexcept
        {
            return m_reflectionDispatchRecorded;
        }

        [[nodiscard]] bool WasReflectionSpatialDenoiseRecorded() const noexcept
        {
            return m_reflectionSpatialDenoiseRecorded;
        }

        [[nodiscard]] bool WasReflectionTemporalDenoiseRecorded() const noexcept
        {
            return m_reflectionTemporalDenoiseRecorded;
        }

        [[nodiscard]] bool WasSubmissionAccepted() const noexcept
        {
            return m_submissionAccepted;
        }

        [[nodiscard]] bool WasReflectionCompositeRecorded() const noexcept
        {
            return m_reflectionCompositeRecorded;
        }

        [[nodiscard]] bool WasPresentationQualified() const noexcept
        {
            return m_presentationQualified;
        }

        [[nodiscard]] const std::string& GetDiagnostic() const noexcept
        {
            return m_diagnostic;
        }

        [[nodiscard]] static HybridRayTracingExecutionReceipt FromPlan(
            const HybridRayTracingFramePlan& plan,
            HybridRayTracingExecutionObservation observation = {});
        /** @brief Value/provenance agreement independent of execution outcome. */
        [[nodiscard]] bool IsStructurallyConsistentWith(
            const HybridRayTracingFramePlan& plan) const noexcept;
        /** @brief Submission-qualified success for the route frozen by the plan. */
        [[nodiscard]] bool IsExecutionSuccessfulFor(
            const HybridRayTracingFramePlan& plan) const noexcept;
        /** @brief Successful execution whose exact frame was also presented. */
        [[nodiscard]] bool IsPresentationQualifiedFor(
            const HybridRayTracingFramePlan& plan) const noexcept;
        /** @brief Backward-compatible spelling for strict execution success. */
        [[nodiscard]] bool IsConsistentWith(
            const HybridRayTracingFramePlan& plan) const noexcept;

    private:
        RenderFrameIdentity m_frameIdentity{};
        RenderViewIdentity m_viewIdentity{};
        RenderViewHistoryIdentity m_reflectionHistoryIdentity{};
        RenderHistoryResetReason m_reflectionHistoryReset =
            RenderHistoryResetReason::None;
        HybridRayTracingHalfResolution m_reflectionExtent{};
        RenderPipelineRecipe m_recipe = RenderPipelineRecipe::HybridRayTracing;
        RenderQualityTier m_quality = RenderQualityTier::High;
        uint64 m_profileHash = 0;
        uint64 m_rayTracingScenePreparationGeneration = 0;
        uint64 m_rayTracingSceneStructuralHash = 0;
        uint64 m_planIdentityHash = 0;
        HybridRayTracingRequirement m_shadowRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingRequirement m_reflectionRequirement =
            HybridRayTracingRequirement::Optional;
        HybridRayTracingEffectReadiness m_shadowReadiness{};
        HybridRayTracingEffectReadiness m_reflectionReadiness{};
        bool m_rasterCSMReady = false;
        bool m_screenSpaceReflectionReady = false;
        bool m_reflectionProbeReady = false;
        bool m_imageBasedLightingReady = false;
        HybridRayTracingShadowRoute m_shadowRoute =
            HybridRayTracingShadowRoute::Disabled;
        HybridRayTracingReflectionRoute m_reflectionRoute =
            HybridRayTracingReflectionRoute::Disabled;
        bool m_shadowDispatchRecorded = false;
        bool m_rasterCSMExecuted = false;
        bool m_reflectionDispatchRecorded = false;
        bool m_reflectionSpatialDenoiseRecorded = false;
        bool m_reflectionTemporalDenoiseRecorded = false;
        bool m_reflectionCompositeRecorded = false;
        bool m_reflectionCompositeUsedDenoisedSource = false;
        bool m_screenSpaceReflectionExecuted = false;
        bool m_reflectionProbeResolved = false;
        bool m_imageBasedLightingResolved = false;
        bool m_submissionAccepted = false;
        bool m_graphExecutionSucceeded = false;
        bool m_sceneBuildAccepted = false;
        bool m_presentationQualified = false;
        std::string m_diagnostic{};
    };
} // namespace RVX
