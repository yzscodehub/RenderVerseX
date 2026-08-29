#include "Render/RayTracing/HybridRayTracingFramePlan.h"

#include <string_view>
#include <type_traits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_HYBRID_RT_FNV_OFFSET = 14695981039346656037ull;
    constexpr uint64 RVX_HYBRID_RT_FNV_PRIME = 1099511628211ull;

    void HashBytes(uint64& hash, const void* data, size_t size) noexcept
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= RVX_HYBRID_RT_FNV_PRIME;
        }
    }

    template<typename T>
    void HashScalar(uint64& hash, T value) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T>);
        HashBytes(hash, &value, sizeof(value));
    }

    template<typename T>
    void HashEnum(uint64& hash, T value) noexcept
    {
        using Underlying = std::underlying_type_t<T>;
        HashScalar(hash, static_cast<Underlying>(value));
    }

    void HashString(uint64& hash, std::string_view value) noexcept
    {
        HashScalar(hash, static_cast<uint64>(value.size()));
        HashBytes(hash, value.data(), value.size());
    }

    void HashFrameIdentity(uint64& hash,
                           const RenderFrameIdentity& identity) noexcept
    {
        HashScalar(hash, identity.frameSequence);
        HashScalar(hash, identity.sceneRevision);
        HashScalar(hash, identity.surfaceGeneration);
        HashScalar(hash, identity.configurationRevision);
    }

    void HashViewIdentity(uint64& hash,
                          const RenderViewIdentity& identity) noexcept
    {
        HashScalar(hash, identity.sceneRuntimeId);
        HashScalar(hash, identity.cameraEntityPacked);
        HashScalar(hash, identity.subViewIndex);
        HashEnum(hash, identity.role);
    }

    void HashHistoryIdentity(uint64& hash,
                             const RenderViewHistoryIdentity& identity) noexcept
    {
        HashFrameIdentity(hash, identity.previousFrame);
        HashViewIdentity(hash, identity.view);
        HashScalar(hash, identity.extent.width);
        HashScalar(hash, identity.extent.height);
        HashScalar(hash, identity.familyId);
        HashScalar(hash, identity.worldRevision);
        HashScalar(hash, identity.temporalEpoch);
        HashScalar(hash, identity.formatRevision);
        HashScalar(hash, identity.surfaceGeneration);
        HashScalar(hash, identity.cameraCutRevision);
    }

    void HashEffectReadiness(
        uint64& hash,
        const HybridRayTracingEffectReadiness& readiness) noexcept
    {
        HashScalar(hash, static_cast<uint8>(readiness.requested));
        HashScalar(hash, static_cast<uint8>(readiness.compiled));
        HashScalar(hash, static_cast<uint8>(readiness.hardwareSupported));
        HashScalar(hash, static_cast<uint8>(readiness.implementationSupported));
        HashScalar(hash, static_cast<uint8>(readiness.pipelineReady));
        HashScalar(hash, static_cast<uint8>(readiness.inputsReady));
        HashScalar(hash, static_cast<uint8>(readiness.dispatchReady));
        HashString(hash, readiness.diagnostic);
    }

    [[nodiscard]] uint64 ComputeHybridRayTracingPlanIdentity(
        const HybridRayTracingFramePlanRequest& request,
        const HybridRayTracingHalfResolution& reflectionExtent,
        HybridRayTracingShadowRoute shadowRoute,
        HybridRayTracingReflectionRoute reflectionRoute) noexcept
    {
        uint64 hash = RVX_HYBRID_RT_FNV_OFFSET;
        HashFrameIdentity(hash, request.frameIdentity);
        HashViewIdentity(hash, request.viewIdentity);
        HashHistoryIdentity(hash, request.reflectionHistoryIdentity);
        HashEnum(hash, request.reflectionHistoryReset);
        HashScalar(hash, reflectionExtent.fullResolution.width);
        HashScalar(hash, reflectionExtent.fullResolution.height);
        HashScalar(hash, reflectionExtent.halfResolution.width);
        HashScalar(hash, reflectionExtent.halfResolution.height);
        HashEnum(hash, request.recipe);
        HashEnum(hash, request.quality);
        HashScalar(hash, request.profileHash);
        HashScalar(hash, request.rayTracingScenePreparationGeneration);
        HashScalar(hash, request.rayTracingSceneStructuralHash);
        HashEnum(hash, request.shadowRequirement);
        HashEnum(hash, request.reflectionRequirement);
        HashEffectReadiness(hash, request.shadow);
        HashEffectReadiness(hash, request.reflection);
        HashScalar(hash, static_cast<uint8>(request.rasterCSMReady));
        HashScalar(hash, static_cast<uint8>(request.screenSpaceReflectionReady));
        HashScalar(hash, static_cast<uint8>(request.reflectionProbeReady));
        HashScalar(hash, static_cast<uint8>(request.imageBasedLightingReady));
        HashEnum(hash, shadowRoute);
        HashEnum(hash, reflectionRoute);
        return hash == 0 ? 1 : hash;
    }
} // namespace

    bool HybridRayTracingEffectReadiness::IsReadyToDispatch() const noexcept
    {
        return requested && compiled && hardwareSupported &&
               implementationSupported && pipelineReady && inputsReady &&
               dispatchReady;
    }

    bool HybridRayTracingHalfResolution::IsValid() const noexcept
    {
        return fullResolution.IsValid() && halfResolution.IsValid() &&
               halfResolution.width ==
                   fullResolution.width / 2U + fullResolution.width % 2U &&
               halfResolution.height ==
                   fullResolution.height / 2U + fullResolution.height % 2U;
    }

    HybridRayTracingHalfResolution ResolveHybridRayTracingHalfResolution(
        RenderOutputExtent fullResolution) noexcept
    {
        HybridRayTracingHalfResolution result;
        result.fullResolution = fullResolution;
        if (!fullResolution.IsValid())
        {
            return result;
        }

        // Divide first so UINT32_MAX does not overflow while preserving ceil.
        result.halfResolution.width =
            fullResolution.width / 2U + fullResolution.width % 2U;
        result.halfResolution.height =
            fullResolution.height / 2U + fullResolution.height % 2U;
        return result;
    }

    HybridRayTracingFramePlan HybridRayTracingFramePlan::Build(
        const HybridRayTracingFramePlanRequest& request)
    {
        HybridRayTracingFramePlan result;
        result.m_frameIdentity = request.frameIdentity;
        result.m_viewIdentity = request.viewIdentity;
        result.m_reflectionHistoryIdentity = request.reflectionHistoryIdentity;
        result.m_reflectionHistoryReset = request.reflectionHistoryReset;
        result.m_reflectionExtent =
            ResolveHybridRayTracingHalfResolution(request.outputExtent);
        result.m_recipe = request.recipe;
        result.m_quality = request.quality;
        result.m_profileHash = request.profileHash;
        result.m_rayTracingScenePreparationGeneration =
            request.rayTracingScenePreparationGeneration;
        result.m_rayTracingSceneStructuralHash =
            request.rayTracingSceneStructuralHash;
        result.m_shadowRequirement = request.shadowRequirement;
        result.m_reflectionRequirement = request.reflectionRequirement;
        result.m_shadow = request.shadow;
        result.m_reflection = request.reflection;
        result.m_rasterCSMReady = request.rasterCSMReady;
        result.m_screenSpaceReflectionReady =
            request.screenSpaceReflectionReady;
        result.m_reflectionProbeReady = request.reflectionProbeReady;
        result.m_imageBasedLightingReady = request.imageBasedLightingReady;

        if (request.shadowRequirement == HybridRayTracingRequirement::Required &&
            !request.shadow.requested)
        {
            result.m_shadowRoute = HybridRayTracingShadowRoute::Failed;
        }
        else if (!request.shadow.requested)
        {
            result.m_shadowRoute = HybridRayTracingShadowRoute::Disabled;
        }
        else if (request.shadow.IsReadyToDispatch())
        {
            result.m_shadowRoute = HybridRayTracingShadowRoute::RayTraced;
        }
        else if (request.shadowRequirement ==
                     HybridRayTracingRequirement::Optional &&
                 request.rasterCSMReady)
        {
            result.m_shadowRoute = HybridRayTracingShadowRoute::RasterCSM;
        }
        else
        {
            result.m_shadowRoute = HybridRayTracingShadowRoute::Failed;
        }

        if (request.reflectionRequirement ==
                HybridRayTracingRequirement::Required &&
            !request.reflection.requested)
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::Failed;
        }
        else if (!request.reflection.requested)
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::Disabled;
        }
        else if (request.reflection.IsReadyToDispatch())
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::RayTraced;
        }
        else if (request.reflectionRequirement ==
                     HybridRayTracingRequirement::Optional &&
                 request.screenSpaceReflectionReady)
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::ScreenSpaceReflection;
        }
        else if (request.reflectionRequirement ==
                     HybridRayTracingRequirement::Optional &&
                 request.reflectionProbeReady)
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::ReflectionProbe;
        }
        else if (request.reflectionRequirement ==
                     HybridRayTracingRequirement::Optional &&
                 request.imageBasedLightingReady)
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::ImageBasedLighting;
        }
        else
        {
            result.m_reflectionRoute =
                HybridRayTracingReflectionRoute::Failed;
        }
        result.m_identityHash = ComputeHybridRayTracingPlanIdentity(
            request,
            result.m_reflectionExtent,
            result.m_shadowRoute,
            result.m_reflectionRoute);
        return result;
    }

    bool HybridRayTracingFramePlan::HasRequiredFailure() const noexcept
    {
        return (m_shadowRequirement == HybridRayTracingRequirement::Required &&
                m_shadowRoute != HybridRayTracingShadowRoute::RayTraced) ||
               (m_reflectionRequirement ==
                    HybridRayTracingRequirement::Required &&
                m_reflectionRoute !=
                    HybridRayTracingReflectionRoute::RayTraced);
    }

    bool HybridRayTracingFramePlan::IsValid() const noexcept
    {
        HybridRayTracingFramePlanRequest canonicalRequest;
        canonicalRequest.frameIdentity = m_frameIdentity;
        canonicalRequest.viewIdentity = m_viewIdentity;
        canonicalRequest.reflectionHistoryIdentity = m_reflectionHistoryIdentity;
        canonicalRequest.reflectionHistoryReset = m_reflectionHistoryReset;
        canonicalRequest.outputExtent = m_reflectionExtent.fullResolution;
        canonicalRequest.recipe = m_recipe;
        canonicalRequest.quality = m_quality;
        canonicalRequest.profileHash = m_profileHash;
        canonicalRequest.rayTracingScenePreparationGeneration =
            m_rayTracingScenePreparationGeneration;
        canonicalRequest.rayTracingSceneStructuralHash =
            m_rayTracingSceneStructuralHash;
        canonicalRequest.shadowRequirement = m_shadowRequirement;
        canonicalRequest.reflectionRequirement = m_reflectionRequirement;
        canonicalRequest.shadow = m_shadow;
        canonicalRequest.reflection = m_reflection;
        canonicalRequest.rasterCSMReady = m_rasterCSMReady;
        canonicalRequest.screenSpaceReflectionReady =
            m_screenSpaceReflectionReady;
        canonicalRequest.reflectionProbeReady = m_reflectionProbeReady;
        canonicalRequest.imageBasedLightingReady = m_imageBasedLightingReady;
        const bool identityMatches = m_identityHash != 0 &&
            m_identityHash == ComputeHybridRayTracingPlanIdentity(
                canonicalRequest,
                m_reflectionExtent,
                m_shadowRoute,
                m_reflectionRoute);
        const bool rayTracedReflection =
            m_reflectionRoute == HybridRayTracingReflectionRoute::RayTraced;
        const bool rayTracedShadow =
            m_shadowRoute == HybridRayTracingShadowRoute::RayTraced;
        const bool resetReflectionHistory =
            m_reflectionHistoryReset != RenderHistoryResetReason::None;
        const bool compatibleReflectionHistory =
            m_reflectionHistoryIdentity.IsValid() &&
            m_reflectionHistoryIdentity.previousFrame.frameSequence <
                m_frameIdentity.frameSequence &&
            m_reflectionHistoryIdentity.previousFrame.surfaceGeneration ==
                m_frameIdentity.surfaceGeneration &&
            m_reflectionHistoryIdentity.previousFrame.configurationRevision ==
                m_frameIdentity.configurationRevision &&
            m_reflectionHistoryIdentity.view == m_viewIdentity &&
            m_reflectionHistoryIdentity.extent ==
                m_reflectionExtent.halfResolution &&
            m_reflectionHistoryIdentity.surfaceGeneration ==
                m_frameIdentity.surfaceGeneration;
        const bool historyIdentityConsistent = !rayTracedReflection ||
            resetReflectionHistory || compatibleReflectionHistory;
        const bool shadowRouteConsistent =
            !m_shadow.requested
                ? (m_shadowRequirement == HybridRayTracingRequirement::Required
                    ? m_shadowRoute == HybridRayTracingShadowRoute::Failed
                    : m_shadowRoute == HybridRayTracingShadowRoute::Disabled)
                : m_shadow.IsReadyToDispatch()
                    ? m_shadowRoute == HybridRayTracingShadowRoute::RayTraced
                    : m_shadowRequirement == HybridRayTracingRequirement::Optional
                        ? (m_rasterCSMReady
                            ? m_shadowRoute == HybridRayTracingShadowRoute::RasterCSM
                            : m_shadowRoute == HybridRayTracingShadowRoute::Failed)
                        : m_shadowRoute == HybridRayTracingShadowRoute::Failed;
        const bool reflectionRouteConsistent =
            !m_reflection.requested
                ? (m_reflectionRequirement == HybridRayTracingRequirement::Required
                    ? m_reflectionRoute == HybridRayTracingReflectionRoute::Failed
                    : m_reflectionRoute == HybridRayTracingReflectionRoute::Disabled)
                : m_reflection.IsReadyToDispatch()
                    ? m_reflectionRoute ==
                        HybridRayTracingReflectionRoute::RayTraced
                    : m_reflectionRequirement ==
                            HybridRayTracingRequirement::Optional
                        ? (m_screenSpaceReflectionReady
                            ? m_reflectionRoute ==
                                HybridRayTracingReflectionRoute::ScreenSpaceReflection
                            : m_reflectionProbeReady
                                ? m_reflectionRoute ==
                                    HybridRayTracingReflectionRoute::ReflectionProbe
                                : m_imageBasedLightingReady
                                    ? m_reflectionRoute ==
                                        HybridRayTracingReflectionRoute::ImageBasedLighting
                                    : m_reflectionRoute ==
                                        HybridRayTracingReflectionRoute::Failed)
                        : m_reflectionRoute ==
                            HybridRayTracingReflectionRoute::Failed;
        const bool requiredRequestConsistent =
            (m_shadowRequirement != HybridRayTracingRequirement::Required ||
             m_shadow.requested) &&
            (m_reflectionRequirement != HybridRayTracingRequirement::Required ||
             m_reflection.requested);
        const bool profileConsistent =
            m_recipe == RenderPipelineRecipe::HybridRayTracing &&
            IsDeclaredRenderQualityTier(m_quality) &&
            m_profileHash == ComputeRenderPipelineProfileHash(m_recipe, m_quality);
        const bool sceneIdentityConsistent =
            (!(rayTracedShadow || rayTracedReflection)) ||
            (m_rayTracingScenePreparationGeneration != 0 &&
             m_rayTracingSceneStructuralHash != 0);
        return identityMatches && profileConsistent && sceneIdentityConsistent &&
               m_frameIdentity.IsValid() && m_viewIdentity.IsValid() &&
               m_reflectionExtent.IsValid() && historyIdentityConsistent &&
               requiredRequestConsistent && shadowRouteConsistent &&
               reflectionRouteConsistent && !HasRequiredFailure();
    }

    HybridRayTracingExecutionReceipt HybridRayTracingExecutionReceipt::FromPlan(
        const HybridRayTracingFramePlan& plan,
        HybridRayTracingExecutionObservation observation)
    {
        HybridRayTracingExecutionReceipt result;
        result.m_frameIdentity = plan.GetFrameIdentity();
        result.m_viewIdentity = plan.GetViewIdentity();
        result.m_reflectionHistoryIdentity = plan.GetReflectionHistoryIdentity();
        result.m_reflectionHistoryReset = plan.GetReflectionHistoryReset();
        result.m_reflectionExtent = plan.GetReflectionExtent();
        result.m_recipe = plan.GetRecipe();
        result.m_quality = plan.GetQuality();
        result.m_profileHash = plan.GetProfileHash();
        result.m_rayTracingScenePreparationGeneration =
            plan.GetRayTracingScenePreparationGeneration();
        result.m_rayTracingSceneStructuralHash =
            plan.GetRayTracingSceneStructuralHash();
        result.m_planIdentityHash = plan.GetIdentityHash();
        result.m_shadowRequirement = plan.GetShadowRequirement();
        result.m_reflectionRequirement = plan.GetReflectionRequirement();
        result.m_shadowReadiness = plan.GetShadowReadiness();
        result.m_reflectionReadiness = plan.GetReflectionReadiness();
        result.m_rasterCSMReady = plan.IsRasterCSMReady();
        result.m_screenSpaceReflectionReady =
            plan.IsScreenSpaceReflectionReady();
        result.m_reflectionProbeReady = plan.IsReflectionProbeReady();
        result.m_imageBasedLightingReady = plan.IsImageBasedLightingReady();
        result.m_shadowRoute = plan.GetShadowRoute();
        result.m_reflectionRoute = plan.GetReflectionRoute();
        result.m_shadowDispatchRecorded = observation.shadowDispatchRecorded;
        result.m_rasterCSMExecuted = observation.rasterCSMExecuted;
        result.m_reflectionDispatchRecorded =
            observation.reflectionDispatchRecorded;
        result.m_reflectionSpatialDenoiseRecorded =
            observation.reflectionSpatialDenoiseRecorded;
        result.m_reflectionTemporalDenoiseRecorded =
            observation.reflectionTemporalDenoiseRecorded;
        result.m_reflectionCompositeRecorded =
            observation.reflectionCompositeRecorded;
        result.m_reflectionCompositeUsedDenoisedSource =
            observation.reflectionCompositeUsedDenoisedSource;
        result.m_screenSpaceReflectionExecuted =
            observation.screenSpaceReflectionExecuted;
        result.m_reflectionProbeResolved =
            observation.reflectionProbeResolved;
        result.m_imageBasedLightingResolved =
            observation.imageBasedLightingResolved;
        result.m_submissionAccepted = observation.submissionAccepted;
        result.m_graphExecutionSucceeded = observation.graphExecutionSucceeded;
        result.m_sceneBuildAccepted = observation.sceneBuildAccepted;
        result.m_presentationQualified = observation.presentationQualified;
        result.m_diagnostic = std::move(observation.diagnostic);
        return result;
    }

    bool HybridRayTracingExecutionReceipt::IsStructurallyConsistentWith(
        const HybridRayTracingFramePlan& plan) const noexcept
    {
        return plan.IsValid() && m_planIdentityHash != 0 &&
            m_planIdentityHash == plan.GetIdentityHash() &&
            m_frameIdentity == plan.GetFrameIdentity() &&
            m_viewIdentity == plan.GetViewIdentity() &&
            m_reflectionHistoryIdentity == plan.GetReflectionHistoryIdentity() &&
            m_reflectionHistoryReset == plan.GetReflectionHistoryReset() &&
            m_reflectionExtent == plan.GetReflectionExtent() &&
            m_recipe == plan.GetRecipe() &&
            m_quality == plan.GetQuality() &&
            m_profileHash == plan.GetProfileHash() &&
            m_rayTracingScenePreparationGeneration ==
                plan.GetRayTracingScenePreparationGeneration() &&
            m_rayTracingSceneStructuralHash ==
                plan.GetRayTracingSceneStructuralHash() &&
            m_shadowRequirement == plan.GetShadowRequirement() &&
            m_reflectionRequirement == plan.GetReflectionRequirement() &&
            m_shadowReadiness == plan.GetShadowReadiness() &&
            m_reflectionReadiness == plan.GetReflectionReadiness() &&
            m_rasterCSMReady == plan.IsRasterCSMReady() &&
            m_screenSpaceReflectionReady ==
                plan.IsScreenSpaceReflectionReady() &&
            m_reflectionProbeReady == plan.IsReflectionProbeReady() &&
            m_imageBasedLightingReady == plan.IsImageBasedLightingReady() &&
            m_shadowRoute == plan.GetShadowRoute() &&
            m_reflectionRoute == plan.GetReflectionRoute();
    }

    bool HybridRayTracingExecutionReceipt::IsExecutionSuccessfulFor(
        const HybridRayTracingFramePlan& plan) const noexcept
    {
        if (!IsStructurallyConsistentWith(plan) ||
            !m_graphExecutionSucceeded || !m_submissionAccepted)
        {
            return false;
        }

        const bool shadowDispatchExpected =
            m_shadowRoute == HybridRayTracingShadowRoute::RayTraced;
        const bool reflectionDispatchExpected =
            m_reflectionRoute == HybridRayTracingReflectionRoute::RayTraced;
        const bool rasterCSMExpected =
            m_shadowRoute == HybridRayTracingShadowRoute::RasterCSM;
        const bool screenSpaceReflectionExpected =
            m_reflectionRoute ==
                HybridRayTracingReflectionRoute::ScreenSpaceReflection;
        const bool reflectionProbeExpected =
            m_reflectionRoute ==
                HybridRayTracingReflectionRoute::ReflectionProbe;
        const bool imageBasedLightingExpected =
            m_reflectionRoute ==
                HybridRayTracingReflectionRoute::ImageBasedLighting;
        if (m_shadowDispatchRecorded != shadowDispatchExpected ||
            m_reflectionDispatchRecorded != reflectionDispatchExpected ||
            m_rasterCSMExecuted != rasterCSMExpected ||
            m_screenSpaceReflectionExecuted !=
                screenSpaceReflectionExpected ||
            m_reflectionProbeResolved != reflectionProbeExpected ||
            m_imageBasedLightingResolved != imageBasedLightingExpected ||
            m_reflectionSpatialDenoiseRecorded !=
                reflectionDispatchExpected ||
            m_reflectionTemporalDenoiseRecorded !=
                reflectionDispatchExpected ||
            m_reflectionCompositeRecorded != reflectionDispatchExpected ||
            m_reflectionCompositeUsedDenoisedSource !=
                reflectionDispatchExpected)
        {
            return false;
        }

        const bool sceneBuildAccepted =
            !(shadowDispatchExpected || reflectionDispatchExpected) ||
            m_sceneBuildAccepted;
        return sceneBuildAccepted &&
               m_shadowRoute != HybridRayTracingShadowRoute::Failed &&
               m_reflectionRoute != HybridRayTracingReflectionRoute::Failed;
    }

    bool HybridRayTracingExecutionReceipt::IsPresentationQualifiedFor(
        const HybridRayTracingFramePlan& plan) const noexcept
    {
        return m_presentationQualified && IsExecutionSuccessfulFor(plan);
    }

    bool HybridRayTracingExecutionReceipt::IsConsistentWith(
        const HybridRayTracingFramePlan& plan) const noexcept
    {
        return IsExecutionSuccessfulFor(plan);
    }
} // namespace RVX
