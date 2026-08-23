#pragma once

/**
 * @file RenderFeatureTypes.h
 * @brief Backend-neutral identities, policies, readiness, and receipts for render features.
 */

#include "Core/Types.h"
#include "RenderContracts/FeatureChannelContracts.h"
#include "RenderContracts/RenderFeatureChannelRegistry.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderPipelineProfile.h"
#include "RHI/RHIDefinitions.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
    /** @brief Stable composition identity. Zero is always invalid. */
    struct RenderFeatureId
    {
        uint32 value = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return value != 0;
        }

        friend constexpr bool operator==(
            const RenderFeatureId&,
            const RenderFeatureId&) = default;
    };

    namespace BuiltinRenderFeatureIds
    {
        inline constexpr RenderFeatureId LegacyForwardMainChain{1};
        inline constexpr RenderFeatureId LegacyPostProcess{2};
        inline constexpr RenderFeatureId FrameSetup{3};
        /** Deferred opaque geometry publishes the canonical GBuffer surface. */
        inline constexpr RenderFeatureId DeferredGeometry{4};
        /** Deferred compute lighting consumes the GBuffer and produces HDR. */
        inline constexpr RenderFeatureId DeferredLighting{5};
        /** Legacy special surfaces retained after deferred opaque lighting. */
        inline constexpr RenderFeatureId DeferredForwardSpecialChain{6};
    } // namespace BuiltinRenderFeatureIds

    /** @brief Legal feature bands. Resource edges determine actual execution order. */
    enum class RenderFeaturePhase : uint8
    {
        FrameSetup = 0,
        Visibility,
        Shadow,
        Geometry,
        Lighting,
        Reflection,
        Sky,
        Translucency,
        Temporal,
        HDRPost,
        ToneMap,
        LDRPost,
        Overlay,
        Present,
    };

    /** @brief Closed semantic slots carried by the typed graph blackboard. */
    enum class RenderBlackboardResourceId : uint8
    {
        SceneTextures = 0,
        VisibilityOutputs,
        ShadowOutputs,
        LightingOutputs,
        ReflectionOutputs,
        TranslucencyOutputs,
        PostProcessOutputs,
        Count,
    };

    struct RenderFeatureResourceContract
    {
        RenderBlackboardResourceId resource =
            RenderBlackboardResourceId::Count;
        uint32 schemaVersion = 0;

        friend constexpr bool operator==(
            const RenderFeatureResourceContract&,
            const RenderFeatureResourceContract&) = default;
    };

    enum class RenderFeatureRequirement : uint8
    {
        Optional = 0,
        Required,
    };

    enum class RenderFeatureFallbackPolicy : uint8
    {
        None = 0,
        ExplicitFeature,
        Skip,
    };

    enum class RenderFeatureCapability : uint32
    {
        None = 0,
        Graphics = 1u << 0u,
        Compute = 1u << 1u,
        DescriptorBindings = 1u << 2u,
        RayTracing = 1u << 3u,
        AccelerationStructureUpdate = 1u << 4u,
        IndependentFloatBlend = 1u << 5u,
        MultipleRenderTargets = 1u << 6u,
        StorageTexture = 1u << 7u,
    };

    using RenderFeatureCapabilityMask = uint32;
    using RenderPipelineRecipeMask = uint32;
    using RenderQualityTierMask = uint32;

    [[nodiscard]] constexpr RenderFeatureCapabilityMask ToMask(
        RenderFeatureCapability capability) noexcept
    {
        return static_cast<RenderFeatureCapabilityMask>(capability);
    }

    [[nodiscard]] constexpr RenderPipelineRecipeMask ToMask(
        RenderPipelineRecipe recipe) noexcept
    {
        return 1u << static_cast<uint8>(recipe);
    }

    [[nodiscard]] constexpr RenderQualityTierMask ToMask(
        RenderQualityTier quality) noexcept
    {
        return 1u << static_cast<uint8>(quality);
    }

    inline constexpr RenderPipelineRecipeMask RVX_ALL_RENDER_RECIPES =
        ToMask(RenderPipelineRecipe::Minimal) |
        ToMask(RenderPipelineRecipe::ForwardStandard) |
        ToMask(RenderPipelineRecipe::DeferredQuality) |
        ToMask(RenderPipelineRecipe::HybridRayTracing) |
        ToMask(RenderPipelineRecipe::Headless);

    inline constexpr RenderQualityTierMask RVX_ALL_RENDER_QUALITY_TIERS =
        ToMask(RenderQualityTier::Low) |
        ToMask(RenderQualityTier::Medium) |
        ToMask(RenderQualityTier::High) |
        ToMask(RenderQualityTier::Ultra);

    /** @brief Immutable construction-time declaration for one feature. */
    struct RenderFeatureDescriptor
    {
        RenderFeatureId featureId{};
        uint32 schemaVersion = 1;
        std::string name{};
        RenderFeaturePhase phase = RenderFeaturePhase::FrameSetup;
        std::vector<RenderFeatureResourceContract> consumedResources{};
        std::vector<RenderFeatureResourceContract> producedResources{};
        RenderFeatureCapabilityMask requiredCapabilities =
            ToMask(RenderFeatureCapability::Graphics);
        RenderPipelineRecipeMask requiredRecipes = 0;
        RenderPipelineRecipeMask optionalRecipes = 0;
        RenderQualityTierMask qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
        RenderFeatureFallbackPolicy fallbackPolicy =
            RenderFeatureFallbackPolicy::None;
        RenderFeatureId fallbackFeatureId{};
        RenderFeatureChannelId snapshotChannel =
            RenderFeatureChannelId::Invalid;
        uint32 snapshotChannelSchemaVersion = 0;
        std::string shaderPipelineFamily{};
    };

    /** @brief Distinct truth dimensions; support never implies execution. */
    struct RenderFeatureReadiness
    {
        bool compiled = false;
        bool hardwareSupported = false;
        bool implementationSupported = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        std::string diagnostic{};

        [[nodiscard]] bool IsReadyToExecute() const noexcept
        {
            return compiled && hardwareSupported &&
                   implementationSupported && pipelineReady && inputsReady;
        }
    };

    /**
     * @brief Immutable, typed feature-input routing for one frozen render frame.
     *
     * The view deliberately exposes only const, concrete channel payloads. It
     * retains the snapshot and its sealed endpoint registry for the lifetime of
     * a feature context, so feature code cannot observe a dangling or
     * type-erased input route.
     */
    class RenderFeatureSnapshotView final
    {
    public:
        RenderFeatureSnapshotView() = default;

        RenderFeatureSnapshotView(
            std::shared_ptr<const RenderFeatureSnapshot> snapshot,
            std::shared_ptr<const RenderFeatureChannelRegistry> channelRegistry,
            RenderFeatureProducerEndpointId producerEndpoint =
                RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
            RenderFeatureConsumerEndpointId consumerEndpoint =
                RenderFeatureConsumerEndpointId::RenderSceneDatabase) noexcept
            : m_snapshot(std::move(snapshot))
            , m_channelRegistry(std::move(channelRegistry))
            , m_producerEndpoint(producerEndpoint)
            , m_consumerEndpoint(consumerEndpoint)
        {
        }

        [[nodiscard]] const RenderFeatureSnapshot* GetSnapshot() const noexcept
        {
            return m_snapshot.get();
        }

        [[nodiscard]] const RenderFeatureChannelRegistry* GetChannelRegistry()
            const noexcept
        {
            return m_channelRegistry.get();
        }

        [[nodiscard]] const ParticleRenderSnapshot* GetParticle() const noexcept
        {
            return m_snapshot != nullptr ? &m_snapshot->particles : nullptr;
        }

        [[nodiscard]] const WaterRenderSnapshot* GetWater() const noexcept
        {
            return m_snapshot != nullptr ? &m_snapshot->water : nullptr;
        }

        [[nodiscard]] const TerrainRenderSnapshot* GetTerrain() const noexcept
        {
            return m_snapshot != nullptr ? &m_snapshot->terrain : nullptr;
        }

        [[nodiscard]] RenderFeatureProducerEndpointId GetProducerEndpoint()
            const noexcept
        {
            return m_producerEndpoint;
        }

        [[nodiscard]] RenderFeatureConsumerEndpointId GetConsumerEndpoint()
            const noexcept
        {
            return m_consumerEndpoint;
        }

    private:
        std::shared_ptr<const RenderFeatureSnapshot> m_snapshot{};
        std::shared_ptr<const RenderFeatureChannelRegistry> m_channelRegistry{};
        RenderFeatureProducerEndpointId m_producerEndpoint =
            RenderFeatureProducerEndpointId::Invalid;
        RenderFeatureConsumerEndpointId m_consumerEndpoint =
            RenderFeatureConsumerEndpointId::Invalid;
    };

    /** @brief Frozen per-frame inputs passed to every selected feature. */
    struct RenderFeatureContext
    {
        RenderFrameIdentity frameIdentity{};
        RenderFrameStamp frameStamp{};
        EffectFrameTime effectTime{};
        RenderOutputExtent outputExtent{};
        uint64 viewFamilyId = 0;
        uint32 primaryViewIndex = 0;
        uint32 viewCount = 0;
        RenderViewIdentity viewIdentity{};
        /** @brief Renderer-owned ordinal of this graph recording in the family. */
        uint32 recordingViewOrdinal = 0;
        /** @brief Monotonic renderer-owned epoch for this graph recording. */
        uint64 recordEpoch = 0;
        RHIBackendType backend = RHIBackendType::None;
        RenderPipelineRequest pipeline{};
        RenderPipelineRecipe resolvedRecipe =
            RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier resolvedQuality = RenderQualityTier::Medium;
        uint64 profileHash = 0;
        RenderFeatureCapabilityMask hardwareCapabilities = 0;
        /** @brief Immutable family proving the recorded ordinal and view identity. */
        std::shared_ptr<const RenderViewFamilySnapshot> viewFamily{};
        /** @brief Immutable typed feature channels retained through preparation. */
        RenderFeatureSnapshotView snapshotInputs{};
    };

    /** @brief One feature's graph-registration and execution evidence. */
    struct RenderFeatureExecutionReceipt
    {
        RenderFeatureId featureId{};
        uint32 featureSchemaVersion = 0;
        RenderFeaturePhase phase = RenderFeaturePhase::FrameSetup;
        RenderFeatureRequirement requirement =
            RenderFeatureRequirement::Optional;
        RenderFeatureReadiness readiness{};
        bool selected = false;
        bool fallbackUsed = false;
        /** Fallback selected by this unavailable optional feature. */
        RenderFeatureId selectedFallbackFeatureId{};
        /** Optional feature for which this entry was selected as fallback. */
        RenderFeatureId fallbackForFeatureId{};
        uint32 graphPassCount = 0;
        uint32 executedGraphPassCount = 0;
        std::vector<std::string> graphPassNames{};
        std::string diagnostic{};
    };

    /** @brief Build/validation identity supplied by the product composition. */
    struct RenderExecutionBuildIdentity
    {
        std::string sourceCommit{};
        std::string preset{};

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return sourceCommit.empty() && preset.empty();
        }

        [[nodiscard]] bool IsAvailable() const noexcept
        {
            return !sourceCommit.empty() && !preset.empty();
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            const bool validCommitLength =
                sourceCommit.size() == 40 || sourceCommit.size() == 64;
            const bool validCommit = validCommitLength &&
                std::all_of(
                    sourceCommit.begin(), sourceCommit.end(), [](char value)
                    {
                        return (value >= '0' && value <= '9') ||
                               (value >= 'a' && value <= 'f');
                    });
            const bool validPreset = !preset.empty() && preset.size() <= 128 &&
                std::all_of(preset.begin(), preset.end(), [](char value)
                {
                    return (value >= 'a' && value <= 'z') ||
                           (value >= 'A' && value <= 'Z') ||
                           (value >= '0' && value <= '9') || value == '-' ||
                           value == '_' || value == '.';
                });
            return validCommit && validPreset;
        }

        friend bool operator==(const RenderExecutionBuildIdentity&,
                               const RenderExecutionBuildIdentity&) = default;
    };

    /** @brief Completion status for the DeferredQuality opaque GBuffer route. */
    enum class RenderDeferredOpaqueExecutionStatus : uint8
    {
        NotAttempted = 0,
        Completed = 1,
        Failed = 2,
    };

    inline const char* GetRenderDeferredOpaqueExecutionStatusName(
        RenderDeferredOpaqueExecutionStatus status) noexcept
    {
        switch (status)
        {
            case RenderDeferredOpaqueExecutionStatus::NotAttempted:
                return "not-attempted";
            case RenderDeferredOpaqueExecutionStatus::Completed:
                return "completed";
            case RenderDeferredOpaqueExecutionStatus::Failed:
                return "failed";
            default: return "invalid";
        }
    }

    /**
     * @brief Presentation-bound evidence for the DeferredQuality opaque route.
     *
     * A false @ref available value is the expected state for recipes that do
     * not select the deferred route. A completed route may legally contain no
     * packets; qualifications that require visible deferred geometry must
     * impose their own non-zero planned-work gate.
     */
    struct RenderDeferredOpaqueExecutionReceipt
    {
        bool available = false;
        RenderDeferredOpaqueExecutionStatus status =
            RenderDeferredOpaqueExecutionStatus::NotAttempted;
        uint32 plannedPacketCount = 0;
        uint32 executedPacketCount = 0;

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!available)
            {
                return status ==
                           RenderDeferredOpaqueExecutionStatus::NotAttempted &&
                       plannedPacketCount == 0 && executedPacketCount == 0;
            }
            return (status == RenderDeferredOpaqueExecutionStatus::Completed ||
                    status == RenderDeferredOpaqueExecutionStatus::Failed) &&
                   executedPacketCount <= plannedPacketCount;
        }
    };

    /**
     * @brief Presentation-bound lighting-plan evidence for one frame/view.
     *
     * The immutable LightingFramePlan is the single source of truth for light
     * selection.  This receipt deliberately records only normalized selection
     * and readiness evidence so diagnostics and sample reports do not need to
     * infer results from mutable renderer state.  A zero primary directional
     * light id is valid: local-light-only and unlit views have no primary
     * directional light by design.
     */
    struct RenderLightingExecutionReceipt
    {
        uint64 lightingPlanIdentity = 0;
        uint64 sceneLightStateHash = 0;
        uint64 primaryDirectionalLightId = 0;
        uint32 pointLightRequestedCount = 0;
        uint32 pointLightAdmittedCount = 0;
        uint32 pointLightCapacity = 0;
        uint32 pointLightOverflowCount = 0;
        uint32 spotLightRequestedCount = 0;
        uint32 spotLightAdmittedCount = 0;
        uint32 spotLightCapacity = 0;
        uint32 spotLightOverflowCount = 0;
        bool csmRequested = false;
        bool csmInputsReady = false;
        bool csmExecutionReady = false;
        bool iblRequested = false;
        bool iblInputsReady = false;
        bool iblExecutionReady = false;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return lightingPlanIdentity == 0 && sceneLightStateHash == 0 &&
                   primaryDirectionalLightId == 0 &&
                   pointLightRequestedCount == 0 &&
                   pointLightAdmittedCount == 0 && pointLightCapacity == 0 &&
                   pointLightOverflowCount == 0 &&
                   spotLightRequestedCount == 0 &&
                   spotLightAdmittedCount == 0 && spotLightCapacity == 0 &&
                   spotLightOverflowCount == 0 && !csmRequested &&
                   !csmInputsReady && !csmExecutionReady && !iblRequested &&
                   !iblInputsReady && !iblExecutionReady;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            const auto hasConsistentCapacityEvidence =
                [](uint32 requested,
                   uint32 admitted,
                   uint32 capacity,
                   uint32 overflow) noexcept
            {
                return admitted <= requested && admitted <= capacity &&
                       overflow == requested - admitted;
            };

            return lightingPlanIdentity != 0 && sceneLightStateHash != 0 &&
                   hasConsistentCapacityEvidence(
                       pointLightRequestedCount, pointLightAdmittedCount,
                       pointLightCapacity, pointLightOverflowCount) &&
                   hasConsistentCapacityEvidence(
                       spotLightRequestedCount, spotLightAdmittedCount,
                       spotLightCapacity, spotLightOverflowCount) &&
                   (!csmInputsReady || csmRequested) &&
                   (!csmExecutionReady || csmInputsReady) &&
                   (!iblInputsReady || iblRequested) &&
                   (!iblExecutionReady || iblInputsReady);
        }
    };

    /**
     * @brief Presentation-bound proof that skinned raster work consumed the
     *        immutable deformation resources for this exact frame/view.
     *
     * The resource counts describe the complete recording snapshot. Draw
     * counts are incremented only after the corresponding DrawIndexed command
     * is recorded, so a non-zero value is execution evidence rather than a
     * declaration of intent.
     */
    struct RenderDeformationExecutionReceipt
    {
        uint64 deformationContentHash = 0;
        uint32 skinnedObjectCount = 0;
        uint32 paletteMatrixCount = 0;
        uint32 previousPaletteObjectCount = 0;
        bool currentPaletteBufferReady = false;
        bool previousPaletteBufferReady = false;
        uint32 opaqueSkinnedDrawCount = 0;
        uint32 shadowSkinnedDrawCount = 0;
        uint32 velocitySkinnedDrawCount = 0;
        uint32 velocityPreviousPaletteDrawCount = 0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return deformationContentHash == 0 && skinnedObjectCount == 0 &&
                   paletteMatrixCount == 0 && previousPaletteObjectCount == 0 &&
                   !currentPaletteBufferReady && !previousPaletteBufferReady &&
                   opaqueSkinnedDrawCount == 0 && shadowSkinnedDrawCount == 0 &&
                   velocitySkinnedDrawCount == 0 &&
                   velocityPreviousPaletteDrawCount == 0;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (deformationContentHash == 0 ||
                previousPaletteObjectCount > skinnedObjectCount ||
                velocityPreviousPaletteDrawCount !=
                    velocitySkinnedDrawCount)
            {
                return false;
            }

            const bool recordedSkinnedDraw = opaqueSkinnedDrawCount != 0 ||
                shadowSkinnedDrawCount != 0 ||
                velocitySkinnedDrawCount != 0;
            if (paletteMatrixCount == 0)
            {
                return skinnedObjectCount == 0 &&
                       previousPaletteObjectCount == 0 &&
                       !currentPaletteBufferReady &&
                       !previousPaletteBufferReady && !recordedSkinnedDraw;
            }

            return skinnedObjectCount != 0 && currentPaletteBufferReady &&
                   previousPaletteBufferReady &&
                   (velocityPreviousPaletteDrawCount == 0 ||
                    previousPaletteObjectCount != 0);
        }
    };

    /** @brief Candidate or presentation-qualified evidence for one frame/view. */
    struct RenderExecutionReceipt
    {
        uint32 schemaVersion = 5;
        bool available = false;
        bool graphComposed = false;
        bool executionSucceeded = false;
        bool presented = false;
        bool succeeded = false;
        RenderExecutionBuildIdentity build{};
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RHIBackendType backend = RHIBackendType::None;
        RenderPipelineRecipe requestedRecipe =
            RenderPipelineRecipe::ForwardStandard;
        RenderPipelineRecipe resolvedRecipe =
            RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier quality = RenderQualityTier::Medium;
        uint64 profileHash = 0;
        /** Exact route evidence when DeferredQuality records opaque GBuffer work. */
        RenderDeferredOpaqueExecutionReceipt deferredOpaque{};
        /** Immutable LightingFramePlan selection and readiness evidence. */
        RenderLightingExecutionReceipt lighting{};
        /** Immutable palette readiness and actual skinned raster execution. */
        RenderDeformationExecutionReceipt deformation{};
        std::vector<RenderFeatureExecutionReceipt> features{};
        std::string diagnostic{};

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if ((schemaVersion != 1 && schemaVersion != 2 &&
                 schemaVersion != 4 && schemaVersion != 5) ||
                !frameIdentity.IsValid() ||
                !viewIdentity.IsValid() || backend == RHIBackendType::None ||
                !IsDeclaredRenderPipelineRecipe(requestedRecipe) ||
                !IsDeclaredRenderPipelineRecipe(resolvedRecipe) ||
                !IsDeclaredRenderQualityTier(quality) ||
                profileHash != ComputeRenderPipelineProfileHash(
                                   resolvedRecipe, quality))
            {
                return false;
            }
            if (!build.IsEmpty() && !build.IsStructurallyValid())
            {
                return false;
            }
            const bool schemaOneInvalid = schemaVersion == 1 &&
                (!lighting.IsEmpty() || !deformation.IsEmpty() ||
                 deferredOpaque.available ||
                 deferredOpaque.status !=
                     RenderDeferredOpaqueExecutionStatus::NotAttempted ||
                 deferredOpaque.plannedPacketCount != 0 ||
                 deferredOpaque.executedPacketCount != 0);
            const bool schemaTwoInvalid = schemaVersion == 2 &&
                (!lighting.IsEmpty() || !deformation.IsEmpty() ||
                 !deferredOpaque.IsStructurallyValid());
            const bool schemaFourInvalid = schemaVersion == 4 &&
                (!deformation.IsEmpty() ||
                 !deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  !lighting.IsStructurallyValid()));
            const bool schemaFiveInvalid = schemaVersion == 5 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid())));
            if (schemaOneInvalid || schemaTwoInvalid || schemaFourInvalid ||
                schemaFiveInvalid)
            {
                return false;
            }
            if (graphComposed && !available)
            {
                return false;
            }
            if (executionSucceeded && (!available || !graphComposed))
            {
                return false;
            }
            std::vector<RenderFeatureId> seenFeatureIds;
            seenFeatureIds.reserve(features.size());
            for (const RenderFeatureExecutionReceipt& feature : features)
            {
                if (!feature.featureId.IsValid() ||
                    feature.featureSchemaVersion == 0 || !feature.selected ||
                    feature.executedGraphPassCount > feature.graphPassCount ||
                    feature.graphPassNames.size() != feature.graphPassCount ||
                    feature.selectedFallbackFeatureId == feature.featureId ||
                    feature.fallbackForFeatureId == feature.featureId ||
                    (!feature.fallbackUsed &&
                     (feature.selectedFallbackFeatureId.IsValid() ||
                      feature.fallbackForFeatureId.IsValid())) ||
                    (feature.requirement == RenderFeatureRequirement::Required &&
                     feature.fallbackUsed) ||
                    (feature.readiness.executed &&
                     !feature.readiness.IsReadyToExecute()) ||
                    std::find(seenFeatureIds.begin(),
                              seenFeatureIds.end(),
                              feature.featureId) != seenFeatureIds.end())
                {
                    return false;
                }
                seenFeatureIds.push_back(feature.featureId);
            }
            if (executionSucceeded && features.empty())
            {
                return false;
            }
            for (const RenderFeatureExecutionReceipt& feature : features)
            {
                const auto findFeature = [this](RenderFeatureId featureId)
                {
                    return std::find_if(
                        features.begin(), features.end(),
                        [featureId](const auto& candidate)
                        {
                            return candidate.featureId == featureId;
                        });
                };
                if (feature.selectedFallbackFeatureId.IsValid())
                {
                    const auto fallback = findFeature(
                        feature.selectedFallbackFeatureId);
                    if (fallback == features.end() ||
                        fallback->fallbackForFeatureId != feature.featureId)
                    {
                        return false;
                    }
                }
                if (feature.fallbackForFeatureId.IsValid())
                {
                    const auto parent = findFeature(
                        feature.fallbackForFeatureId);
                    if (parent == features.end() ||
                        parent->selectedFallbackFeatureId != feature.featureId)
                    {
                        return false;
                    }
                }
                if (executionSucceeded && !feature.readiness.executed &&
                    (feature.requirement != RenderFeatureRequirement::Optional ||
                     !feature.fallbackUsed || feature.graphPassCount != 0 ||
                     feature.executedGraphPassCount != 0))
                {
                    return false;
                }
                if (executionSucceeded && feature.readiness.executed &&
                    feature.graphPassCount != 0 &&
                    feature.executedGraphPassCount != feature.graphPassCount)
                {
                    return false;
                }
            }
            return succeeded == (executionSucceeded && presented);
        }
    };
} // namespace RVX
