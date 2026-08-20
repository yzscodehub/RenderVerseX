#pragma once

/**
 * @file IWorldEcsRuntimeServices.h
 * @brief Engine-owned services for one World ECS runtime.
 */

#include "Core/Types.h"
#include "AnimationSceneAdapters/ECS/EcsAnimationAssetService.h"
#include "Physics/Backend/IPhysicsBackend.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentLoadCoordinator.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"

#include <optional>
#include <string>

namespace RVX
{
    namespace SceneECS
    {
        struct SceneEntityRef;
    }

    /** @brief Flat owner-clock and lifecycle observation for one ECS World. */
    struct WorldEcsRuntimeServicesDiagnostics
    {
        bool available = false;
        bool initialized = false;
        bool shutdownBegun = false;
        bool shutdownComplete = false;
        bool lastTickSucceeded = false;
        uint64 sceneRuntimeId = 0;
        uint64 sceneFrameSequence = 0;
        uint64 sceneFixedStepSequence = 0;
        /** @brief Fixed-step sequence observed from the ECS-owned PhysicsWorld. */
        uint64 physicsFixedStepSequence = 0;
        uint64 requestedFixedStepCount = 0;
        uint64 executedFixedStepCount = 0;
        uint32 lastRequestedFixedStepCount = 0;
        uint32 lastExecutedFixedStepCount = 0;

        /** @brief Current PhysicsWorld identity and backend selection, never a PhysicsWorld pointer. */
        bool physicsInitialized = false;
        Physics::PhysicsBackendType requestedPhysicsBackend = Physics::PhysicsBackendType::Auto;
        Physics::PhysicsBackendType activePhysicsBackend = Physics::PhysicsBackendType::BuiltIn;
        bool physicsBackendFallbackActive = false;
        uint32 physicsBridgeBindingSideTableEntryCount = 0;
        uint32 activePhysicsBodyCount = 0;
        uint32 pendingPhysicsBridgeCleanupCount = 0;
        uint32 staticPhysicsBodyCount = 0;
        uint32 dynamicPhysicsBodyCount = 0;
        uint32 kinematicPhysicsBodyCount = 0;
        uint32 physicsColliderCount = 0;
        uint64 physicsBridgeStructuralContinuityLossCount = 0;
        uint64 physicsBridgeCleanupContinuityLossCount = 0;
        uint64 physicsBridgeAuthoritativeReconcileCount = 0;
        uint64 physicsRootMotionAppliedCount = 0;
        uint64 physicsRootMotionRejectedCount = 0;
        uint64 physicsRootMotionReplayCount = 0;
        uint64 physicsRootMotionGapCount = 0;

        /** @brief Value-derived runtime-instance census; unavailable optional bridges report false/zero. */
        bool animationBridgeAvailable = false;
        uint32 animationBindingSideTableEntryCount = 0;
        uint32 activeAnimationBindingCount = 0;
        uint32 pendingAnimationBridgeCleanupCount = 0;
        uint64 animationFixedEvaluationCount = 0;
        uint64 animationRejectedEvaluationCount = 0;
        uint64 animationRootMotionPublicationCount = 0;
        uint64 animationRootMotionReplayCount = 0;
        uint64 animationRootMotionGapCount = 0;
        bool resourceAnimationEvaluatorAvailable = false;
        uint32 resourceAnimationPlaybackSideTableEntryCount = 0;
        bool audioBridgeAvailable = false;
        uint32 audioPlaybackSideTableEntryCount = 0;
        uint32 activeAudioPlaybackCount = 0;
        uint32 outstandingAudioPlaybackCount = 0;
        uint32 pendingAudioBridgeCleanupCount = 0;
        bool scriptBridgeAvailable = false;
        uint32 scriptInstanceSideTableEntryCount = 0;
        uint32 activeScriptInstanceCount = 0;
        uint32 outstandingScriptInstanceCount = 0;
        uint32 pendingScriptBridgeCleanupCount = 0;
        /** @brief Feature bridges are per-World consumers of Engine-global gateway owners. */
        bool particleBridgeAvailable = false;
        uint32 particleBindingSideTableEntryCount = 0;
        uint32 particleOutstandingRuntimeCount = 0;
        uint32 particlePublishedSnapshotCount = 0;
        uint32 pendingParticleBridgeCleanupCount = 0;
        uint64 particleBridgeStructuralContinuityLossCount = 0;
        uint64 particleBridgeCleanupContinuityLossCount = 0;
        uint64 particleBridgeAuthoritativeReconcileCount = 0;
        bool waterBridgeAvailable = false;
        uint32 waterBindingSideTableEntryCount = 0;
        uint32 waterOutstandingRuntimeCount = 0;
        uint32 waterPublishedSnapshotCount = 0;
        uint32 pendingWaterBridgeCleanupCount = 0;
        uint64 waterBridgeStructuralContinuityLossCount = 0;
        uint64 waterBridgeCleanupContinuityLossCount = 0;
        uint64 waterBridgeAuthoritativeReconcileCount = 0;
        bool terrainBridgeAvailable = false;
        uint32 terrainBindingSideTableEntryCount = 0;
        uint32 terrainOutstandingRuntimeCount = 0;
        uint32 terrainPublishedSnapshotCount = 0;
        uint32 pendingTerrainBridgeCleanupCount = 0;
        uint64 terrainBridgeStructuralContinuityLossCount = 0;
        uint64 terrainBridgeCleanupContinuityLossCount = 0;
        uint64 terrainBridgeAuthoritativeReconcileCount = 0;
        bool featureSnapshotStoreAvailable = false;
        uint32 particleFeatureSnapshotCount = 0;
        uint32 waterFeatureSnapshotCount = 0;
        uint32 terrainFeatureSnapshotCount = 0;
        uint32 featureSnapshotInstanceCount = 0;

        uint32 trackedModelRequestCount = 0;
        uint32 trackedEnvironmentRequestCount = 0;
        uint32 trackedAnimationRequestCount = 0;
        uint32 activeAnimationRequestCount = 0;
        std::string lastDiagnostic;
    };

    /** @brief Value-only outcome for binding ECS root motion to the entity's physics body. */
    enum class WorldEcsAnimationPhysicsBindingCode : uint8
    {
        Applied = 0,
        ServiceUnavailable,
        ForeignScene,
        InvalidEntity,
        PhysicsBodyUnavailable,
        AnimationBindingUnavailable,
        Rejected,
    };

    /** @brief Public binding evidence which never exposes a Physics body handle. */
    struct WorldEcsAnimationPhysicsBindingResult
    {
        WorldEcsAnimationPhysicsBindingCode code =
            WorldEcsAnimationPhysicsBindingCode::ServiceUnavailable;
        std::string diagnostic;

        [[nodiscard]] bool IsApplied() const noexcept
        {
            return code == WorldEcsAnimationPhysicsBindingCode::Applied;
        }
    };

    /**
     * @brief Per-World value service implemented by Engine's private ECS composition.
     *
     * This is the only Engine product boundary for ECS model/environment
     * requests. It exposes Scene-qualified generation request values and flat
     * diagnostics; it never exposes legacy Scene objects or renderer state.
     */
    class IWorldEcsRuntimeServices
    {
    public:
        virtual ~IWorldEcsRuntimeServices() = default;

        [[nodiscard]] virtual ResourceSceneAdapters::EcsModelAssetLoadRef RequestModel(
            ResourceSceneAdapters::EcsModelAssetLoadDesc desc,
            std::string& outError) = 0;
        [[nodiscard]] virtual bool CancelModel(
            ResourceSceneAdapters::EcsModelAssetLoadRef request) = 0;
        [[nodiscard]] virtual std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>
        GetModelStatus(ResourceSceneAdapters::EcsModelAssetLoadRef request) const = 0;

        [[nodiscard]] virtual ResourceSceneAdapters::EcsEnvironmentLoadRef RequestEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadDesc desc,
            std::string& outError) = 0;
        [[nodiscard]] virtual bool CancelEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadRef request) = 0;
        [[nodiscard]] virtual std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
        GetEnvironmentStatus(ResourceSceneAdapters::EcsEnvironmentLoadRef request) const = 0;

        /** @brief Request one immutable animation asset for this exact ECS World. */
        [[nodiscard]] virtual AnimationSceneAdapters::EcsAnimationAssetLoadRef RequestAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadDesc desc,
            std::string& outError) = 0;
        [[nodiscard]] virtual bool CancelAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request) = 0;
        [[nodiscard]] virtual std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
        GetAnimationStatus(AnimationSceneAdapters::EcsAnimationAssetLoadRef request) const = 0;
        /**
         * @brief Prove compatibility against an Alive target entity's current binding, then replace it.
         *
         * The target SceneEntityRef prevents cross-World mutation. The implementation resolves
         * the target binding's exact published resource internally and never accepts caller
         * skeleton topology for a resource-backed entity.
         */
        [[nodiscard]] virtual AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request,
            SceneECS::SceneEntityRef targetEntity,
            uint32 animationClipOrdinal = 0) = 0;

        /**
         * @brief Bind one Alive animation entity to its exact ECS Physics body.
         *
         * The implementation owns both bridge side tables and repeats the
         * generation proof internally. Samples never receive the packed body
         * handle used by the Animation-to-Physics bridge.
         */
        [[nodiscard]] virtual WorldEcsAnimationPhysicsBindingResult
        BindAnimationRootMotionPhysics(SceneECS::SceneEntityRef targetEntity) = 0;

        [[nodiscard]] virtual WorldEcsRuntimeServicesDiagnostics
        GetRuntimeDiagnostics() const = 0;
    };
} // namespace RVX
