#pragma once

/**
 * @file RenderFeatureTypes.h
 * @brief Backend-neutral identities, policies, readiness, and receipts for render features.
 */

#include "Core/Types.h"
#include "Render/PostProcess/PostProcessTypes.h"
#include "RenderContracts/FeatureChannelContracts.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderFeatureChannelRegistry.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderPipelineProfile.h"
#include "RHI/RHIDefinitions.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
        /** Typed HDR-to-presentation pipeline; stable identity retained. */
        inline constexpr RenderFeatureId PostProcessPipeline{2};
        inline constexpr RenderFeatureId FrameSetup{3};
        /** Deferred opaque geometry publishes the canonical GBuffer surface. */
        inline constexpr RenderFeatureId DeferredGeometry{4};
        /** Deferred compute lighting consumes the GBuffer and produces HDR. */
        inline constexpr RenderFeatureId DeferredLighting{5};
        /** Legacy special surfaces retained after deferred opaque lighting. */
        inline constexpr RenderFeatureId DeferredForwardSpecialChain{6};
        /** GTAO publishes the canonical indirect-light visibility field. */
        inline constexpr RenderFeatureId AmbientOcclusion{7};
        /** Forward lighting/special-surface consumer after GTAO publication. */
        inline constexpr RenderFeatureId ForwardLighting{8};
        /** Recipe-neutral reflection resolve over the lit HDR scene surface. */
        inline constexpr RenderFeatureId ReflectionResolve{9};
        /** ForwardStandard special surfaces after the reflection resolve. */
        inline constexpr RenderFeatureId ForwardSpecialChain{10};
        /** Recipe-neutral mesh/particle translucency composition. */
        inline constexpr RenderFeatureId Translucency{11};
        /** Froxel volumetric integration shared by every translucent surface. */
        inline constexpr RenderFeatureId VolumetricLighting{12};
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
        LocalShadowOutputs,
        LightingOutputs,
        ReflectionOutputs,
        VolumetricLightingOutputs,
        PreTranslucencyOutputs,
        TranslucencyOutputs,
        PostProcessOutputs,
        AmbientOcclusionOutputs,
        /** Immutable, full-resolution D32 snapshot shared by special surfaces. */
        SceneDepthSnapshot,
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
        /**
         * @brief Permits a ready feature with declared typed outputs to finish
         *        its data contract without registering a RenderGraph pass.
         *
         * This is an explicit, narrow opt-in for producers whose current
         * frame has no GPU work. It never represents GPU execution: a feature
         * that registered any pass, including one later culled by the graph,
         * must still provide exact execution evidence for that pass.
         */
        bool allowsDataOnlyCompletion = false;
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
        /**
         * @brief A finalized typed-output contract completed with no graph
         *        pass registered; this is not GPU execution evidence.
         */
        bool contractCompletedWithoutGraphWork = false;
    };

    /**
     * @brief Registry-owned final execution evidence for one feature recording.
     *
     * This is delivered only after RenderFeatureRegistry has inspected the
     * exact RenderGraph that was composed for the frozen frame and view. A
     * feature may use it to finalize detailed receipts, but it must never
     * infer execution from scheduling alone.
     */
    struct RenderFeatureGraphExecutionEvidence
    {
        RenderFeatureId featureId{};
        uint64 graphIdentity = 0;
        uint64 recordingGeneration = 0;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint32 graphPassCount = 0;
        uint32 executedGraphPassCount = 0;
        bool semanticExecutionSucceeded = false;
        std::string diagnostic{};
        /**
         * @brief A finalized typed-output contract completed with no graph
         *        pass registered; this is not GPU execution evidence.
         */
        bool contractCompletedWithoutGraphWork = false;

        [[nodiscard]] bool HasExactExecution() const noexcept
        {
            return featureId.IsValid() && graphIdentity != 0 &&
                   recordingGeneration != 0 && frameIdentity.IsValid() &&
                   viewIdentity.IsValid() && graphPassCount != 0 &&
                   executedGraphPassCount == graphPassCount &&
                   semanticExecutionSucceeded && diagnostic.empty();
        }
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
     * @brief Execution-only evidence for the shared Forward/Deferred Toon path.
     *
     * This is populated by the renderer composition only after exact opaque
     * execution. It therefore cannot be accidentally populated from a
     * requested material or profile:
     * non-zero counts represent completed binding/draw work only. The program
     * family is a stable value identity, not a process-local string address.
     */
    struct RenderToonLightingExecutionReceipt
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        uint64 programFamilyHash = 0;
        uint32 materialCount = 0;
        uint32 drawCount = 0;
        uint32 forwardShadedDrawCount = 0;
        uint32 deferredShadedDrawCount = 0;
        uint32 standardLitFallbackCount = 0;
        uint32 materialFallbackCount = 0;
        bool bandsApplied = false;
        bool specularThresholdApplied = false;
        bool rimApplied = false;
        bool shadowTintApplied = false;
        uint32 materialTableSchemaVersion = 0;
        uint64 materialTableContentHash = 0;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !requested && !pipelineReady && !inputsReady && !executed &&
                   programFamilyHash == 0 && materialCount == 0 &&
                   drawCount == 0 && forwardShadedDrawCount == 0 &&
                   deferredShadedDrawCount == 0 &&
                   standardLitFallbackCount == 0 && materialFallbackCount == 0 &&
                   !bandsApplied && !specularThresholdApplied && !rimApplied &&
                   !shadowTintApplied && materialTableSchemaVersion == 0 &&
                   materialTableContentHash == 0;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!requested)
            {
                return IsEmpty();
            }
            // A Toon path never degrades to StandardLit. Fallback evidence
            // instead makes the receipt unusable for qualification.
            if (standardLitFallbackCount != 0 || materialFallbackCount != 0 ||
                forwardShadedDrawCount + deferredShadedDrawCount != drawCount)
            {
                return false;
            }
            const bool hasInputs = materialCount != 0 ||
                materialTableSchemaVersion != 0 || materialTableContentHash != 0;
            if (inputsReady != hasInputs ||
                (inputsReady &&
                 (programFamilyHash == 0 || materialCount == 0 ||
                  materialTableSchemaVersion == 0 ||
                  materialTableContentHash == 0)))
            {
                return false;
            }
            if (!executed)
            {
                return drawCount == 0 && !bandsApplied &&
                       !specularThresholdApplied && !rimApplied &&
                       !shadowTintApplied;
            }
            return pipelineReady && inputsReady && drawCount != 0 &&
                   (forwardShadedDrawCount == 0) !=
                       (deferredShadedDrawCount == 0) &&
                   bandsApplied && specularThresholdApplied && rimApplied &&
                   shadowTintApplied;
        }

        [[nodiscard]] bool IsExecutionReady() const noexcept
        {
            return IsStructurallyValid() && requested && pipelineReady &&
                   inputsReady && executed && materialCount != 0 &&
                   drawCount != 0 && standardLitFallbackCount == 0 &&
                   materialFallbackCount == 0;
        }
    };

    /**
     * @brief Presentation-bound evidence for the material-aware HDR outline.
     *
     * Graph execution and the optional qualification readback are distinct.
     * Ordinary frames can prove a real outline draw without mapping pixels;
     * qualification additionally requires the exact presented frame's
     * verifier classification to complete after its submission fence.
     */
    struct RenderToonOutlineExecutionReceipt
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool depthInputReady = false;
        bool worldNormalInputReady = false;
        bool materialIdentityInputReady = false;
        bool materialTableInputReady = false;
        bool temporalOrderingValidated = false;
        /** One-based public order; zero means that effect was not scheduled. */
        uint32 taaSequenceIndex = 0;
        uint32 outlineSequenceIndex = 0;
        uint32 toneMapSequenceIndex = 0;
        uint32 eligibleToonMaterialCount = 0;
        bool qualificationRequested = false;
        bool qualificationReady = false;
        uint64 outlinedPixelCount = 0;
        uint64 nonToonAffectedPixelCount = 0;
        bool resolutionAware = false;
        uint32 renderWidth = 0;
        uint32 renderHeight = 0;
        uint64 settingsHash = 0;
        uint64 materialTableContentHash = 0;
        bool fallbackUsed = false;
        std::string diagnostic{};

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !requested && !pipelineReady && !inputsReady && !executed &&
                   !depthInputReady && !worldNormalInputReady &&
                   !materialIdentityInputReady && !materialTableInputReady &&
                   !temporalOrderingValidated && taaSequenceIndex == 0 &&
                   outlineSequenceIndex == 0 && toneMapSequenceIndex == 0 &&
                   eligibleToonMaterialCount == 0 &&
                   !qualificationRequested && !qualificationReady &&
                   outlinedPixelCount == 0 && nonToonAffectedPixelCount == 0 &&
                   !resolutionAware && renderWidth == 0 && renderHeight == 0 &&
                   settingsHash == 0 && materialTableContentHash == 0 &&
                   !fallbackUsed && diagnostic.empty();
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!requested)
            {
                return IsEmpty();
            }
            const bool exactInputs = depthInputReady &&
                worldNormalInputReady && materialIdentityInputReady &&
                materialTableInputReady;
            if (inputsReady != exactInputs || settingsHash == 0 ||
                (qualificationRequested && !executed) ||
                (qualificationReady && !qualificationRequested))
            {
                return false;
            }
            if (!executed)
            {
                return outlineSequenceIndex == 0 &&
                       toneMapSequenceIndex == 0 &&
                       eligibleToonMaterialCount == 0 &&
                       !qualificationRequested && !qualificationReady &&
                       outlinedPixelCount == 0 &&
                       nonToonAffectedPixelCount == 0 &&
                       !resolutionAware && renderWidth == 0 &&
                       renderHeight == 0 &&
                       materialTableContentHash == 0;
            }
            const bool orderingValid =
                outlineSequenceIndex != 0 && toneMapSequenceIndex != 0 &&
                outlineSequenceIndex < toneMapSequenceIndex &&
                (taaSequenceIndex == 0 ||
                 taaSequenceIndex < outlineSequenceIndex);
            if (!pipelineReady || !inputsReady ||
                temporalOrderingValidated != orderingValid ||
                !temporalOrderingValidated ||
                materialTableContentHash == 0 || fallbackUsed)
            {
                return false;
            }
            if (!qualificationReady)
            {
                return outlinedPixelCount == 0 &&
                       nonToonAffectedPixelCount == 0 &&
                       !resolutionAware && renderWidth == 0 &&
                       renderHeight == 0;
            }
            return renderWidth != 0 && renderHeight != 0 &&
                   resolutionAware && diagnostic.empty();
        }

        [[nodiscard]] bool IsExecutionReady() const noexcept
        {
            return IsStructurallyValid() && requested && pipelineReady &&
                   inputsReady && executed &&
                   eligibleToonMaterialCount != 0 &&
                   outlineSequenceIndex != 0 &&
                   toneMapSequenceIndex != 0 && !fallbackUsed;
        }

        [[nodiscard]] bool IsQualificationReady() const noexcept
        {
            return IsExecutionReady() && qualificationRequested &&
                   qualificationReady && taaSequenceIndex != 0 &&
                   taaSequenceIndex < outlineSequenceIndex &&
                   outlineSequenceIndex < toneMapSequenceIndex &&
                   outlinedPixelCount != 0 &&
                   nonToonAffectedPixelCount == 0 && resolutionAware &&
                   renderWidth != 0 && renderHeight != 0;
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

    /**
     * @brief Immutable post-process plan and presentation evidence for one
     *        frame/view.
     *
     * This records the typed plan selected before graph mutation. A positive
     * count is not inferred from settings: it is filled only after the
     * corresponding graph work is observed by the feature registry.
     */
    struct RenderPostProcessExecutionReceipt
    {
        uint64 postProcessContentHash = 0;
        /** Immutable GPU exposure-state identity shared by every HDR consumer. */
        uint64 exposureStateContentHash = 0;
        uint64 toneMapExposureStateHash = 0;
        uint64 bloomExposureStateHash = 0;
        uint64 taaExposureStateHash = 0;
        RenderExposureMode exposureMode = RenderExposureMode::ManualEV100;
        bool exposureRequested = false;
        bool exposurePipelineReady = false;
        bool exposureInputsReady = false;
        bool exposureExecuted = false;
        /** Clear + build nodes for HistogramAuto; zero for ManualEV100. */
        uint32 exposureHistogramPassCount = 0;
        /** One GPU resolve/adaptation node for both exposure modes. */
        uint32 exposureAdaptationPassCount = 0;
        bool exposureHistoryAvailableBefore = false;
        bool exposureHistorySampled = false;
        bool exposureHistoryCandidateProduced = false;
        bool exposureHistoryCommitted = false;
        bool exposureHistoryAbandoned = false;
        RenderHistoryResetReason exposureHistoryResetReasons =
            RenderHistoryResetReason::None;
        /** Optional qualification-only state readback; never required at runtime. */
        bool exposureProbeRequested = false;
        bool exposureProbeReady = false;
        float32 targetEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 adaptedEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 exposureScale = std::numeric_limits<float32>::quiet_NaN();
        float32 preExposure = std::numeric_limits<float32>::quiet_NaN();
        float32 previousPreExposure =
            std::numeric_limits<float32>::quiet_NaN();
        uint32 toneMapPassCount = 0;
        uint32 bloomPassCount = 0;
        uint32 colorGradingPassCount = 0;
        uint32 fxaaPassCount = 0;
        /** One semantic temporal-AA effect, independent of physical graph nodes. */
        uint32 taaPassCount = 0;
        /** The sole AA mode resolved from the frozen render-frame settings. */
        RenderAntiAliasingMode antiAliasingMode =
            RenderAntiAliasingMode::None;
        /** TAA was selected by the canonical AA mode. */
        bool taaRequested = false;
        /** The cooked TAA resolve/store pipelines were ready. */
        bool taaPipelineReady = false;
        /** Required current-frame TAA inputs were valid. */
        bool taaInputsReady = false;
        /** Both physical TAA graph nodes executed successfully. */
        bool taaExecuted = false;
        /** Exact prior-presented temporal history was reusable before resolve. */
        bool historyAvailableBefore = false;
        /** The resolve sampled a compatible prior-presented history. */
        bool historySampled = false;
        /** This graph produced a candidate history resource for its exact view. */
        bool historyCandidateProduced = false;
        /** Candidate history committed only after a matching successful Present. */
        bool historyCommitted = false;
        /** Candidate history was discarded without changing committed history. */
        bool historyAbandoned = false;
        /** Exact reason that prior temporal history could not be sampled. */
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::None;
        /** Current and previous jitter expressed in physical pixels. */
        Vec2 currentJitterPixels = Vec2(0.0f);
        Vec2 previousJitterPixels = Vec2(0.0f);
        /** Physical graph evidence for the real resolve/store implementation. */
        uint32 taaResolveGraphPassCount = 0;
        uint32 taaHistoryStoreGraphPassCount = 0;
        /** Semantic domain consumed by the post-process presentation plan. */
        PostProcessColorDomain inputColorDomain =
            PostProcessColorDomain::Unknown;
        /** Shader-visible output domain selected while materializing the plan. */
        PostProcessColorDomain plannedOutputColorDomain =
            PostProcessColorDomain::Unknown;
        /** Domain stored in the presentable resource after any hardware encode. */
        PostProcessColorDomain storedOutputColorDomain =
            PostProcessColorDomain::Unknown;
        /** Exact RHI format of the presentable resource used by this plan. */
        RHIFormat finalOutputFormat = RHIFormat::Unknown;
        /** Transfer performed by the single required ToneMapping transform. */
        PostProcessTransfer toneMappingTransfer = PostProcessTransfer::None;
        /** Transfer from the planned output to the stored presentable result. */
        PostProcessTransfer presentationTransfer = PostProcessTransfer::None;
        bool hdrInputReady = false;
        bool displayLinearOutputReady = false;
        bool encodedOutputReady = false;
        bool fallbackUsed = false;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return postProcessContentHash == 0 && toneMapPassCount == 0 &&
                   exposureStateContentHash == 0 &&
                   toneMapExposureStateHash == 0 &&
                   bloomExposureStateHash == 0 &&
                   taaExposureStateHash == 0 &&
                   !exposureRequested && !exposurePipelineReady &&
                   !exposureInputsReady && !exposureExecuted &&
                   exposureHistogramPassCount == 0 &&
                   exposureAdaptationPassCount == 0 &&
                   !exposureHistoryAvailableBefore &&
                   !exposureHistorySampled &&
                   !exposureHistoryCandidateProduced &&
                   !exposureHistoryCommitted &&
                   !exposureHistoryAbandoned &&
                   exposureHistoryResetReasons ==
                       RenderHistoryResetReason::None &&
                   !exposureProbeRequested && !exposureProbeReady &&
                   bloomPassCount == 0 && colorGradingPassCount == 0 &&
                   fxaaPassCount == 0 && taaPassCount == 0 &&
                   antiAliasingMode == RenderAntiAliasingMode::None &&
                   !taaRequested && !taaPipelineReady && !taaInputsReady &&
                   !taaExecuted && !historyAvailableBefore && !historySampled &&
                   !historyCandidateProduced && !historyCommitted &&
                   !historyAbandoned &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   currentJitterPixels == Vec2(0.0f) &&
                   previousJitterPixels == Vec2(0.0f) &&
                   taaResolveGraphPassCount == 0 &&
                   taaHistoryStoreGraphPassCount == 0 &&
                   inputColorDomain == PostProcessColorDomain::Unknown &&
                   plannedOutputColorDomain ==
                       PostProcessColorDomain::Unknown &&
                   storedOutputColorDomain ==
                       PostProcessColorDomain::Unknown &&
                   finalOutputFormat == RHIFormat::Unknown &&
                   toneMappingTransfer == PostProcessTransfer::None &&
                   presentationTransfer == PostProcessTransfer::None &&
                   !hdrInputReady && !displayLinearOutputReady &&
                   !encodedOutputReady && !fallbackUsed;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            const bool finiteTemporalJitter =
                std::isfinite(currentJitterPixels.x) &&
                std::isfinite(currentJitterPixels.y) &&
                std::isfinite(previousJitterPixels.x) &&
                std::isfinite(previousJitterPixels.y);
            const bool exposureConsumersMatch =
                exposureStateContentHash != 0 &&
                toneMapExposureStateHash == exposureStateContentHash &&
                ((bloomPassCount == 0 && bloomExposureStateHash == 0) ||
                 (bloomPassCount != 0 &&
                  bloomExposureStateHash == exposureStateContentHash)) &&
                ((antiAliasingMode != RenderAntiAliasingMode::TAA &&
                  taaExposureStateHash == 0) ||
                 (antiAliasingMode == RenderAntiAliasingMode::TAA &&
                  taaExposureStateHash == exposureStateContentHash));
            const bool exposureProbeValid =
                !exposureProbeReady ||
                (exposureProbeRequested && exposureHistoryCommitted &&
                 !exposureHistoryAbandoned && std::isfinite(targetEV100) &&
                 targetEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
                 targetEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
                 std::isfinite(adaptedEV100) &&
                 adaptedEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
                 adaptedEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
                 IsValidRenderExposureScale(exposureScale) &&
                 IsValidRenderExposureScale(preExposure) &&
                 IsValidRenderExposureScale(previousPreExposure));
            if (postProcessContentHash == 0 || toneMapPassCount != 1 ||
                !IsValidRenderAntiAliasingMode(antiAliasingMode) ||
                !IsValidRenderExposureMode(exposureMode) ||
                !exposureRequested || !exposurePipelineReady ||
                !exposureInputsReady || !exposureExecuted ||
                !exposureConsumersMatch || !exposureProbeValid ||
                (exposureProbeReady && !exposureProbeRequested) ||
                !finiteTemporalJitter ||
                !hdrInputReady ||
                !encodedOutputReady || fallbackUsed ||
                inputColorDomain != PostProcessColorDomain::SceneLinearHDR ||
                storedOutputColorDomain !=
                    PostProcessColorDomain::DisplayEncodedLDR ||
                finalOutputFormat == RHIFormat::Unknown)
            {
                return false;
            }

            const bool histogramMode =
                exposureMode == RenderExposureMode::HistogramAuto;
            const uint32 expectedHistogramPassCount = histogramMode ? 2u : 0u;
            if (exposureHistogramPassCount != expectedHistogramPassCount ||
                exposureAdaptationPassCount != 1 ||
                !exposureHistoryCandidateProduced ||
                (exposureHistoryCommitted && exposureHistoryAbandoned) ||
                exposureHistoryAvailableBefore != exposureHistorySampled ||
                (exposureHistoryAvailableBefore &&
                 exposureHistoryResetReasons !=
                     RenderHistoryResetReason::None) ||
                (!exposureHistoryAvailableBefore &&
                 exposureHistoryResetReasons ==
                     RenderHistoryResetReason::None))
            {
                return false;
            }

            const bool temporalMode =
                antiAliasingMode == RenderAntiAliasingMode::TAA;
            const bool spatialMode =
                antiAliasingMode == RenderAntiAliasingMode::FXAA;
            if (taaRequested != temporalMode ||
                (spatialMode && fxaaPassCount != 1) ||
                (!spatialMode && fxaaPassCount != 0) ||
                (!temporalMode &&
                 (taaPassCount != 0 || taaPipelineReady || taaInputsReady ||
                  taaExecuted || historyAvailableBefore || historySampled ||
                  historyCandidateProduced || historyCommitted ||
                  historyAbandoned ||
                  historyResetReasons != RenderHistoryResetReason::None ||
                  currentJitterPixels != Vec2(0.0f) ||
                  previousJitterPixels != Vec2(0.0f) ||
                  taaResolveGraphPassCount != 0 ||
                  taaHistoryStoreGraphPassCount != 0)) ||
                (temporalMode &&
                 (taaPassCount != 1 || !taaPipelineReady || !taaInputsReady ||
                  !taaExecuted ||
                  !historyCandidateProduced ||
                  (historyAbandoned && historyCommitted) ||
                  taaResolveGraphPassCount != 1 ||
                  taaHistoryStoreGraphPassCount != 1 ||
                  (historyAvailableBefore != historySampled) ||
                  (historyAvailableBefore &&
                   historyResetReasons != RenderHistoryResetReason::None) ||
                  (!historyAvailableBefore &&
                   historyResetReasons == RenderHistoryResetReason::None))))
            {
                return false;
            }

            if (plannedOutputColorDomain ==
                PostProcessColorDomain::DisplayLinearLDR)
            {
                return IsSRGBFormat(finalOutputFormat) &&
                       toneMappingTransfer ==
                           PostProcessTransfer::SceneLinearToDisplayLinear &&
                       presentationTransfer ==
                           PostProcessTransfer::DisplayLinearToEncoded &&
                       displayLinearOutputReady;
            }

            if (plannedOutputColorDomain ==
                PostProcessColorDomain::DisplayEncodedLDR)
            {
                return !IsSRGBFormat(finalOutputFormat) &&
                       toneMappingTransfer ==
                           PostProcessTransfer::SceneLinearToDisplayEncoded &&
                       presentationTransfer ==
                           PostProcessTransfer::SceneLinearToDisplayEncoded &&
                       !displayLinearOutputReady;
            }

            return false;
        }
    };

    /**
     * @brief Exact GTAO execution and post-present qualification evidence.
     *
     * This receipt never describes an arbitrary scene-color darkening pass:
     * its closed consumer mask proves that only indirect lighting consumed the
     * generated R8 visibility field.
     */
    struct RenderAmbientOcclusionExecutionReceipt
    {
        bool requested = false;
        bool linearDepthInputReady = false;
        bool worldNormalInputReady = false;
        /** True only when the current view's RG16 motion-vector field is usable. */
        bool velocityInputReady = false;
        /** True only when the velocity producer executed for this exact view. */
        bool velocityProducerExecuted = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool outputPublished = false;
        bool executed = false;
        /** The selected opaque lighting route consumed this exact AO field. */
        bool lightingConsumerExecuted = false;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        /** True only when history sampling was reprojected with motion vectors. */
        bool motionReprojectionUsed = false;
        /** Current and previous unjittered camera transforms genuinely differ. */
        bool viewProjectionMotionObserved = false;
        bool historyCandidateProduced = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        /** GTAO has no scene-color fallback; true is always a contract failure. */
        bool fallbackUsed = false;
        bool probeRequested = false;
        bool probeReady = false;
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::None;
        RenderAmbientOcclusionConsumerMask consumerMask = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;
        uint32 horizonSearchGraphPassCount = 0;
        uint32 bilateralDenoiseGraphPassCount = 0;
        uint32 temporalResolveGraphPassCount = 0;
        uint32 probeSampleCount = 0;
        uint32 probeOccludedSampleCount = 0;
        uint64 planContentHash = 0;
        uint64 settingsHash = 0;
        uint64 aoFieldContentHash = 0;
        uint64 lightingAOFieldContentHash = 0;
        /** Stable identity of the exact velocity field consumed by GTAO. */
        uint64 velocityFieldIdentity = 0;
        float32 probeMinVisibility = 0.0f;
        float32 probeMeanVisibility = 0.0f;
        float32 probeMaxVisibility = 0.0f;
        std::string diagnostic{};

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !requested && !linearDepthInputReady &&
                   !worldNormalInputReady && !velocityInputReady &&
                   !velocityProducerExecuted &&
                   !pipelineReady && !inputsReady &&
                   !outputPublished && !executed &&
                   !lightingConsumerExecuted && !historyAvailableBefore &&
                   !historySampled && !motionReprojectionUsed &&
                   !viewProjectionMotionObserved &&
                   !historyCandidateProduced &&
                   !historyCommitted && !historyAbandoned && !fallbackUsed &&
                   !probeRequested &&
                   !probeReady &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   consumerMask == 0 && outputFormat == RHIFormat::Unknown &&
                   horizonSearchGraphPassCount == 0 &&
                   bilateralDenoiseGraphPassCount == 0 &&
                   temporalResolveGraphPassCount == 0 && probeSampleCount == 0 &&
                   probeOccludedSampleCount == 0 &&
                   planContentHash == 0 && settingsHash == 0 &&
                   aoFieldContentHash == 0 && lightingAOFieldContentHash == 0 &&
                   velocityFieldIdentity == 0 &&
                   probeMinVisibility == 0.0f && probeMeanVisibility == 0.0f &&
                   probeMaxVisibility == 0.0f && diagnostic.empty();
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!requested)
                return IsEmpty();

            const bool probeValuesEmpty = probeSampleCount == 0 &&
                probeOccludedSampleCount == 0 &&
                probeMinVisibility == 0.0f && probeMeanVisibility == 0.0f &&
                probeMaxVisibility == 0.0f;
            const bool probeValuesValid = probeSampleCount != 0 &&
                probeOccludedSampleCount <= probeSampleCount &&
                std::isfinite(probeMinVisibility) &&
                std::isfinite(probeMeanVisibility) &&
                std::isfinite(probeMaxVisibility) &&
                probeMinVisibility >= 0.0f && probeMinVisibility <= 1.0f &&
                probeMeanVisibility >= probeMinVisibility &&
                probeMeanVisibility <= probeMaxVisibility &&
                probeMaxVisibility <= 1.0f;
            if (planContentHash == 0 || settingsHash == 0 ||
                !IsValidRenderAmbientOcclusionConsumerMask(consumerMask) ||
                (inputsReady !=
                 (linearDepthInputReady && worldNormalInputReady &&
                  velocityInputReady)) ||
                (historySampled && !historyAvailableBefore) ||
                (motionReprojectionUsed !=
                 (historySampled && velocityInputReady &&
                  velocityProducerExecuted)) ||
                (viewProjectionMotionObserved && !motionReprojectionUsed) ||
                (historyCommitted && !historyCandidateProduced) ||
                (historyAbandoned && !historyCandidateProduced) ||
                (historyCommitted && historyAbandoned) ||
                fallbackUsed ||
                (probeReady && (!probeRequested || !probeValuesValid)) ||
                (!probeReady && !probeValuesEmpty))
            {
                return false;
            }

            if (!executed)
            {
                return !outputPublished && outputFormat == RHIFormat::Unknown &&
                       horizonSearchGraphPassCount == 0 &&
                       bilateralDenoiseGraphPassCount == 0 &&
                       temporalResolveGraphPassCount == 0 &&
                       aoFieldContentHash == 0 && lightingAOFieldContentHash == 0 &&
                       velocityFieldIdentity == 0 &&
                       !lightingConsumerExecuted &&
                       !historyAvailableBefore && !historySampled &&
                       !motionReprojectionUsed &&
                       !viewProjectionMotionObserved &&
                       !historyCandidateProduced && !historyCommitted &&
                       !historyAbandoned &&
                       !probeRequested && !probeReady &&
                       historyResetReasons == RenderHistoryResetReason::None;
            }

            return linearDepthInputReady && worldNormalInputReady &&
                   velocityInputReady &&
                   velocityProducerExecuted && velocityFieldIdentity != 0 &&
                   pipelineReady && inputsReady && outputPublished &&
                   outputFormat == RHIFormat::R8_UNORM &&
                   horizonSearchGraphPassCount == 1 &&
                   bilateralDenoiseGraphPassCount == 2 &&
                   temporalResolveGraphPassCount == 1 &&
                   aoFieldContentHash != 0 &&
                   ((!lightingConsumerExecuted &&
                     lightingAOFieldContentHash == 0) ||
                    (lightingConsumerExecuted &&
                     lightingAOFieldContentHash == aoFieldContentHash)) &&
                   historyCandidateProduced &&
                   (!historyAvailableBefore || historySampled) &&
                   (!historySampled ||
                    historyResetReasons == RenderHistoryResetReason::None);
        }
    };

    enum class RenderHybridRayTracingShadowRoute : uint8
    {
        None = 0,
        RayTraced,
        RasterCSM,
        Failed,
    };

    enum class RenderHybridRayTracingReflectionRoute : uint8
    {
        None = 0,
        RayTraced,
        ScreenSpaceReflection,
        ReflectionProbe,
        ImageBasedLighting,
        Failed,
    };

    /**
     * @brief Public, backend-neutral projection of one frozen Hybrid RT plan.
     *
     * The internal plan remains owned by RenderRayTracing. This POD projection
     * lets diagnostics and Samples consume exact graph/submission/presentation
     * evidence without creating a RenderFoundation -> feature-target cycle.
     */
    struct RenderHybridRayTracingExecutionReceipt
    {
        bool available = false;
        uint64 planIdentityHash = 0;
        uint64 scenePreparationGeneration = 0;
        uint64 sceneStructuralHash = 0;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        RenderOutputExtent fullResolution{};
        RenderOutputExtent halfResolution{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::None;
        RenderPipelineRecipe recipe = RenderPipelineRecipe::HybridRayTracing;
        RenderQualityTier quality = RenderQualityTier::High;
        uint64 profileHash = 0;
        RenderHybridRayTracingShadowRoute shadowRoute =
            RenderHybridRayTracingShadowRoute::None;
        RenderHybridRayTracingReflectionRoute reflectionRoute =
            RenderHybridRayTracingReflectionRoute::None;
        bool graphExecutionSucceeded = false;
        bool sceneBuildAccepted = false;
        bool shadowDispatchRecorded = false;
        bool reflectionDispatchRecorded = false;
        bool spatialDenoiseRecorded = false;
        bool temporalResolveRecorded = false;
        bool compositeRecorded = false;
        bool compositeUsedDenoisedSource = false;
        bool submissionAccepted = false;
        bool presentationQualified = false;
        bool succeeded = false;

        [[nodiscard]] bool IsEmpty() const noexcept
        {
            return !available && planIdentityHash == 0 &&
                   scenePreparationGeneration == 0 &&
                   sceneStructuralHash == 0 &&
                   !frameIdentity.IsValid() && !viewIdentity.IsValid() &&
                   !fullResolution.IsValid() && !halfResolution.IsValid() &&
                   historyResetReasons == RenderHistoryResetReason::None &&
                   profileHash == 0 &&
                   shadowRoute == RenderHybridRayTracingShadowRoute::None &&
                   reflectionRoute ==
                       RenderHybridRayTracingReflectionRoute::None &&
                   !graphExecutionSucceeded && !sceneBuildAccepted &&
                   !shadowDispatchRecorded && !reflectionDispatchRecorded &&
                   !spatialDenoiseRecorded && !temporalResolveRecorded &&
                   !compositeRecorded && !compositeUsedDenoisedSource &&
                   !submissionAccepted && !presentationQualified &&
                   !succeeded;
        }

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if (!available)
            {
                return IsEmpty();
            }
            const bool extentValid = fullResolution.IsValid() &&
                halfResolution.IsValid() &&
                halfResolution.width ==
                    fullResolution.width / 2U + fullResolution.width % 2U &&
                halfResolution.height ==
                    fullResolution.height / 2U + fullResolution.height % 2U;
            const bool frozenIdentityValid = planIdentityHash != 0 &&
                scenePreparationGeneration != 0 && sceneStructuralHash != 0 &&
                frameIdentity.IsValid() && viewIdentity.IsValid() &&
                extentValid && recipe == RenderPipelineRecipe::HybridRayTracing &&
                IsDeclaredRenderQualityTier(quality) &&
                profileHash == ComputeRenderPipelineProfileHash(recipe, quality) &&
                shadowRoute == RenderHybridRayTracingShadowRoute::RayTraced &&
                reflectionRoute ==
                    RenderHybridRayTracingReflectionRoute::RayTraced;
            const bool dispatchChainRecorded = IsGraphExecutionReady();
            const bool submissionConsistent = !submissionAccepted ||
                (dispatchChainRecorded && sceneBuildAccepted);
            const bool presentationConsistent = !presentationQualified ||
                submissionAccepted;
            const bool expectedSuccess = dispatchChainRecorded &&
                sceneBuildAccepted && submissionAccepted &&
                presentationQualified;
            return frozenIdentityValid && submissionConsistent &&
                   presentationConsistent && succeeded == expectedSuccess;
        }

        [[nodiscard]] bool IsGraphExecutionReady() const noexcept
        {
            return available && graphExecutionSucceeded &&
                   shadowRoute ==
                       RenderHybridRayTracingShadowRoute::RayTraced &&
                   reflectionRoute ==
                       RenderHybridRayTracingReflectionRoute::RayTraced &&
                   shadowDispatchRecorded && reflectionDispatchRecorded &&
                   spatialDenoiseRecorded && temporalResolveRecorded &&
                   compositeRecorded && compositeUsedDenoisedSource;
        }

        [[nodiscard]] bool IsSubmissionReady() const noexcept
        {
            return IsGraphExecutionReady() && sceneBuildAccepted &&
                   submissionAccepted;
        }

        [[nodiscard]] bool IsPresentationReady() const noexcept
        {
            return IsSubmissionReady() && presentationQualified && succeeded;
        }
    };

    /** @brief Current ABI of @ref RenderExecutionReceipt. */
    inline constexpr uint32 RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION = 12;

    /** @brief Candidate or presentation-qualified evidence for one frame/view. */
    struct RenderExecutionReceipt
    {
        uint32 schemaVersion = RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION;
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
        /** Typed HDR-to-presentation plan and exact stage evidence. */
        RenderPostProcessExecutionReceipt postProcess{};
        /** GTAO execution and indirect-light field provenance. */
        RenderAmbientOcclusionExecutionReceipt ambientOcclusion{};
        /** Exact shared Forward+/Deferred Toon material execution. */
        RenderToonLightingExecutionReceipt toonLighting{};
        /** Exact HDR outline execution and optional presented-frame readback. */
        RenderToonOutlineExecutionReceipt toonOutline{};
        /** Required Hybrid RT graph/submission/presentation evidence. */
        RenderHybridRayTracingExecutionReceipt hybridRayTracing{};
        std::vector<RenderFeatureExecutionReceipt> features{};
        std::string diagnostic{};

        [[nodiscard]] bool IsStructurallyValid() const noexcept
        {
            if ((schemaVersion != 1 && schemaVersion != 2 &&
                 schemaVersion != 4 && schemaVersion != 5 &&
                 schemaVersion != 6 && schemaVersion != 7 &&
                 schemaVersion != 8 && schemaVersion != 10 &&
                 schemaVersion != 11 && schemaVersion != 12) ||
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
            if (schemaVersion != 10 && schemaVersion != 11 &&
                schemaVersion != 12 &&
                !ambientOcclusion.IsEmpty())
            {
                return false;
            }
            if (schemaVersion != 11 && schemaVersion != 12 &&
                (!toonLighting.IsEmpty() || !toonOutline.IsEmpty()))
            {
                return false;
            }
            if (schemaVersion != 12 && !hybridRayTracing.IsEmpty())
            {
                return false;
            }
            const bool schemaOneInvalid = schemaVersion == 1 &&
                (!lighting.IsEmpty() || !deformation.IsEmpty() ||
                 !postProcess.IsEmpty() ||
                 deferredOpaque.available ||
                 deferredOpaque.status !=
                     RenderDeferredOpaqueExecutionStatus::NotAttempted ||
                 deferredOpaque.plannedPacketCount != 0 ||
                 deferredOpaque.executedPacketCount != 0);
            const bool schemaTwoInvalid = schemaVersion == 2 &&
                (!lighting.IsEmpty() || !deformation.IsEmpty() ||
                 !postProcess.IsEmpty() ||
                 !deferredOpaque.IsStructurallyValid());
            const bool schemaFourInvalid = schemaVersion == 4 &&
                (!deformation.IsEmpty() || !postProcess.IsEmpty() ||
                 !deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  !lighting.IsStructurallyValid()));
            const bool schemaFiveInvalid = schemaVersion == 5 &&
                (!postProcess.IsEmpty() ||
                 !deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid())));
            const bool postProcessCarriesTemporalEvidence =
                postProcess.antiAliasingMode == RenderAntiAliasingMode::TAA ||
                postProcess.taaPassCount != 0 || postProcess.taaRequested ||
                postProcess.taaPipelineReady || postProcess.taaInputsReady ||
                postProcess.taaExecuted ||
                postProcess.historyAvailableBefore || postProcess.historySampled ||
                postProcess.historyCandidateProduced ||
                postProcess.historyCommitted || postProcess.historyAbandoned ||
                postProcess.historyResetReasons !=
                    RenderHistoryResetReason::None ||
                postProcess.currentJitterPixels != Vec2(0.0f) ||
                postProcess.previousJitterPixels != Vec2(0.0f) ||
                postProcess.taaResolveGraphPassCount != 0 ||
                postProcess.taaHistoryStoreGraphPassCount != 0;
            const bool schemaSixInvalid = schemaVersion == 6 &&
                (!deferredOpaque.IsStructurallyValid() ||
                (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                (!postProcess.IsEmpty() &&
                  !postProcess.IsStructurallyValid()) ||
                 postProcessCarriesTemporalEvidence ||
                ((executionSucceeded || presented) &&
                 (!lighting.IsStructurallyValid() ||
                  !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())));
            const bool schemaSevenInvalid = schemaVersion == 7 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 (!postProcess.IsEmpty() &&
                  !postProcess.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())) ||
                 (presented && postProcess.taaRequested &&
                  !postProcess.historyCommitted));
            const bool schemaEightInvalid = schemaVersion == 8 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 (!postProcess.IsEmpty() &&
                  !postProcess.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())) ||
                 (presented && postProcess.taaRequested &&
                  !postProcess.historyCommitted) ||
                 (presented &&
                  !postProcess.exposureHistoryCommitted) ||
                 (postProcess.exposureProbeReady &&
                  (!presented || !postProcess.exposureHistoryCommitted ||
                   postProcess.exposureHistoryAbandoned)));
            const bool schemaTenInvalid = schemaVersion == 10 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 (!postProcess.IsEmpty() && !postProcess.IsStructurallyValid()) ||
                 (!ambientOcclusion.IsEmpty() &&
                  !ambientOcclusion.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())) ||
                 (presented && postProcess.taaRequested &&
                  !postProcess.historyCommitted) ||
                 (presented && !postProcess.exposureHistoryCommitted) ||
                 (postProcess.exposureProbeReady &&
                  (!presented || !postProcess.exposureHistoryCommitted ||
                   postProcess.exposureHistoryAbandoned)) ||
                 (ambientOcclusion.probeReady &&
                  (!presented || !ambientOcclusion.historyCommitted ||
                   ambientOcclusion.historyAbandoned)));
            const bool schemaElevenInvalid = schemaVersion == 11 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 (!postProcess.IsEmpty() &&
                  !postProcess.IsStructurallyValid()) ||
                 (!ambientOcclusion.IsEmpty() &&
                  !ambientOcclusion.IsStructurallyValid()) ||
                 (!toonLighting.IsEmpty() &&
                  !toonLighting.IsStructurallyValid()) ||
                 (!toonOutline.IsEmpty() &&
                  !toonOutline.IsStructurallyValid()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())) ||
                 (presented && postProcess.taaRequested &&
                  !postProcess.historyCommitted) ||
                 (presented && !postProcess.exposureHistoryCommitted) ||
                 (postProcess.exposureProbeReady &&
                  (!presented || !postProcess.exposureHistoryCommitted ||
                   postProcess.exposureHistoryAbandoned)) ||
                 (ambientOcclusion.probeReady &&
                  (!presented || !ambientOcclusion.historyCommitted ||
                   ambientOcclusion.historyAbandoned)) ||
                 (toonOutline.qualificationReady && !presented) ||
                 (toonOutline.qualificationRequested &&
                  !toonLighting.IsExecutionReady()) ||
                 (toonOutline.executed && toonLighting.executed &&
                  toonOutline.materialTableContentHash !=
                      toonLighting.materialTableContentHash));
            const bool schemaTwelveInvalid = schemaVersion == 12 &&
                (!deferredOpaque.IsStructurallyValid() ||
                 (!lighting.IsEmpty() && !lighting.IsStructurallyValid()) ||
                 (!deformation.IsEmpty() &&
                  !deformation.IsStructurallyValid()) ||
                 (!postProcess.IsEmpty() &&
                  !postProcess.IsStructurallyValid()) ||
                 (!ambientOcclusion.IsEmpty() &&
                  !ambientOcclusion.IsStructurallyValid()) ||
                 (!toonLighting.IsEmpty() &&
                  !toonLighting.IsStructurallyValid()) ||
                 (!toonOutline.IsEmpty() &&
                  !toonOutline.IsStructurallyValid()) ||
                 (!hybridRayTracing.IsEmpty() &&
                  !hybridRayTracing.IsStructurallyValid()) ||
                 (hybridRayTracing.available &&
                  (resolvedRecipe != RenderPipelineRecipe::HybridRayTracing ||
                   hybridRayTracing.frameIdentity != frameIdentity ||
                   hybridRayTracing.viewIdentity != viewIdentity ||
                   hybridRayTracing.recipe != resolvedRecipe ||
                   hybridRayTracing.quality != quality ||
                   hybridRayTracing.profileHash != profileHash)) ||
                 (resolvedRecipe != RenderPipelineRecipe::HybridRayTracing &&
                  !hybridRayTracing.IsEmpty()) ||
                 (resolvedRecipe == RenderPipelineRecipe::HybridRayTracing &&
                  executionSucceeded &&
                  !hybridRayTracing.IsGraphExecutionReady()) ||
                 ((executionSucceeded || presented) &&
                  (!lighting.IsStructurallyValid() ||
                   !deformation.IsStructurallyValid() ||
                   !postProcess.IsStructurallyValid())) ||
                 (presented && postProcess.taaRequested &&
                  !postProcess.historyCommitted) ||
                 (presented && !postProcess.exposureHistoryCommitted) ||
                 (postProcess.exposureProbeReady &&
                  (!presented || !postProcess.exposureHistoryCommitted ||
                   postProcess.exposureHistoryAbandoned)) ||
                 (ambientOcclusion.probeReady &&
                  (!presented || !ambientOcclusion.historyCommitted ||
                   ambientOcclusion.historyAbandoned)) ||
                 (toonOutline.qualificationReady && !presented) ||
                 (toonOutline.qualificationRequested &&
                  !toonLighting.IsExecutionReady()) ||
                 (toonOutline.executed && toonLighting.executed &&
                  toonOutline.materialTableContentHash !=
                      toonLighting.materialTableContentHash) ||
                 (presented &&
                  resolvedRecipe == RenderPipelineRecipe::HybridRayTracing &&
                  !hybridRayTracing.IsPresentationReady()) ||
                 (hybridRayTracing.presentationQualified && !presented) ||
                 (hybridRayTracing.succeeded &&
                  (!presented || !succeeded)));
            if (schemaOneInvalid || schemaTwoInvalid || schemaFourInvalid ||
                schemaFiveInvalid || schemaSixInvalid || schemaSevenInvalid ||
                schemaEightInvalid || schemaTenInvalid ||
                schemaElevenInvalid || schemaTwelveInvalid)
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
                    (feature.contractCompletedWithoutGraphWork &&
                     (!feature.readiness.executed ||
                      feature.graphPassCount != 0 ||
                      feature.executedGraphPassCount != 0 ||
                      feature.fallbackUsed ||
                      !feature.readiness.diagnostic.empty() ||
                      !feature.diagnostic.empty())) ||
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
                if ((schemaVersion == 10 || schemaVersion == 11 ||
                     schemaVersion == RVX_RENDER_EXECUTION_RECEIPT_SCHEMA_VERSION) &&
                    executionSucceeded &&
                    feature.readiness.executed &&
                    feature.graphPassCount == 0 &&
                    !feature.contractCompletedWithoutGraphWork)
                {
                    return false;
                }
            }
            return succeeded == (executionSucceeded && presented);
        }
    };
} // namespace RVX
