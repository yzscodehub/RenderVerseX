#pragma once

/**
 * @file WorldEcsRuntimeComposition.h
 * @brief Isolated pure-ECS runtime assembly owned by a future World host.
 */

#include "EcsRenderEntityRetirementCoordinator.h"
#include "EcsRenderFramePipeline.h"
#include "AnimationSceneAdapters/ECS/AnimationEcsBridge.h"
#include "AnimationSceneAdapters/ECS/EcsAnimationAssetService.h"
#include "AnimationSceneAdapters/ECS/ResourceAnimationEcsEvaluator.h"
#include "Audio/ECS/AudioEcsBridge.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Physics/PhysicsWorld.h"
#include "PhysicsSceneAdapters/ECS/PhysicsEcsBridge.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentLoadCoordinator.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"
#include "Scene/ECS/SceneEcsRuntime.h"
#include "Scripting/ECS/ScriptEcsBridge.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RVX::Resource
{
    class ResourceSubsystem;
}

namespace RVX
{
    namespace Particle
    {
        class IParticleEcsGateway;
        class ParticleEcsBridge;
    }

    namespace Water
    {
        class IWaterEcsGateway;
        class WaterEcsBridge;
    }

    namespace Terrain
    {
        class ITerrainEcsGateway;
        class TerrainEcsBridge;
    }

    /** @brief Flat non-owning feature bridge diagnostics safe to expose from this assembly. */
    struct WorldEcsFeatureBridgeDiagnostics
    {
        uint32 bindingCount = 0;
        uint32 outstandingRuntimeCount = 0;
        uint32 publishedSnapshotCount = 0;
        uint32 pendingCleanupCount = 0;
        uint64 structuralContinuityLossCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 authoritativeReconcileCount = 0;
    };

    /** @brief Construction-only dependencies for one isolated ECS World runtime. */
    struct WorldEcsRuntimeCompositionOptions
    {
        Physics::PhysicsWorldConfig physicsConfig{};
        AnimationSceneAdapters::AnimationEcsPoseEvaluator animationEvaluator{};
        /** Optional shared owner whose callback is injected into AnimationEcsBridge. */
        std::shared_ptr<AnimationSceneAdapters::EcsAnimationAssetService>
            animationAssetService;
        /** Optional shared owner whose resolver must be created by animationAssetService. */
        std::shared_ptr<AnimationSceneAdapters::ResourceAnimationEcsEvaluator>
            resourceAnimationEvaluator;
        /** Optional gateway which must outlive this composition. */
        Audio::IAudioEcsPlaybackGateway* audioGateway = nullptr;
        /** Optional gateway which must outlive this composition. */
        Scripting::IScriptEcsExecutionGateway* scriptGateway = nullptr;
        /** Optional Engine-global gateway which must outlive this composition. */
        Particle::IParticleEcsGateway* particleGateway = nullptr;
        /** Optional Engine-global gateway which must outlive this composition. */
        Water::IWaterEcsGateway* waterGateway = nullptr;
        /** Optional Engine-global gateway which must outlive this composition. */
        Terrain::ITerrainEcsGateway* terrainGateway = nullptr;
    };

    /** @brief Per-frame Scene input owned by the World clock. Render publishes separately. */
    struct WorldEcsRuntimeCompositionTickRequest
    {
        SceneECS::SceneEcsTickRequest sceneTick;
    };

    /** @brief Value outcome from one composition tick. */
    struct WorldEcsRuntimeCompositionTickResult
    {
        bool succeeded = false;
        bool modelCoordinatorUpdated = true;
        bool environmentCoordinatorUpdated = true;
        bool animationAssetServiceUpdated = true;
        SceneECS::SceneEcsTickResult scene;
        uint32 confirmedModelPresentations = 0;
        uint32 confirmedEnvironmentPresentations = 0;
    };

    /** @brief Shutdown-visible composition state; values retain failure evidence. */
    struct WorldEcsRuntimeCompositionDiagnostics
    {
        bool initialized = false;
        bool shutdownBegun = false;
        bool shutdownComplete = false;
        bool processorsCleared = false;
        bool physicsInitialized = false;
        uint32 trackedModelRequestCount = 0;
        uint32 trackedEnvironmentRequestCount = 0;
        std::optional<AnimationSceneAdapters::EcsAnimationAssetServiceSceneDiagnosticsSnapshot>
            animationAssetService;
        SceneECS::SceneEcsDiagnosticsSnapshot scene;
        Physics::PhysicsRuntimeDiagnosticsSnapshot physics;
        PhysicsSceneAdapters::PhysicsEcsBridgeDiagnosticsSnapshot physicsBridge;
        AnimationSceneAdapters::AnimationEcsBridgeDiagnosticsSnapshot animationBridge;
        std::optional<Audio::AudioEcsBridgeDiagnosticsSnapshot> audioBridge;
        std::optional<Scripting::ScriptEcsBridgeDiagnosticsSnapshot> scriptBridge;
        std::optional<WorldEcsFeatureBridgeDiagnostics> particleBridge;
        std::optional<WorldEcsFeatureBridgeDiagnostics> waterBridge;
        std::optional<WorldEcsFeatureBridgeDiagnostics> terrainBridge;
        std::optional<AnimationSceneAdapters::ResourceAnimationEcsEvaluatorDiagnosticsSnapshot>
            resourceAnimationEvaluator;
        std::optional<EcsRenderEntityRetirementDiagnostics> genericRetirement;
        std::optional<Resource::ResourceDiagnosticsSnapshot> resources;
        std::string lastDiagnostic;
    };

    /**
     * @brief Non-owning pure-ECS World assembly which does not alter legacy World composition.
     *
     * The caller owns the Scene runtime, render pipeline, optional Resource subsystem, and
     * optional Audio/Script gateways. This owner creates bridge state only after the Built-in
     * PhysicsWorld has initialized, compiles all registrations as one transaction, and leaves
     * failed shutdown evidence intact for the host to continue ticking.
     */
    class WorldEcsRuntimeComposition final : public IWorldEcsRuntimeServices
    {
    public:
        WorldEcsRuntimeComposition(
            SceneECS::SceneEcsRuntime& runtime,
            EcsRenderFramePipeline& renderPipeline,
            Resource::ResourceSubsystem* resources = nullptr,
            WorldEcsRuntimeCompositionOptions options = {}) noexcept;
        ~WorldEcsRuntimeComposition() noexcept;

        WorldEcsRuntimeComposition(const WorldEcsRuntimeComposition&) = delete;
        WorldEcsRuntimeComposition& operator=(const WorldEcsRuntimeComposition&) = delete;
        WorldEcsRuntimeComposition(WorldEcsRuntimeComposition&&) = delete;
        WorldEcsRuntimeComposition& operator=(WorldEcsRuntimeComposition&&) = delete;

        /** @brief Register every enabled bridge, then atomically compile their processors. */
        [[nodiscard]] bool Initialize();
        [[nodiscard]] bool IsInitialized() const noexcept { return m_initialized; }

        /** @brief Advance Resource adoption, Scene ECS, then exact proof-backed presentation. */
        [[nodiscard]] WorldEcsRuntimeCompositionTickResult Tick(
            const WorldEcsRuntimeCompositionTickRequest& request = {});

        [[nodiscard]] ResourceSceneAdapters::EcsModelAssetLoadRef RequestModel(
            ResourceSceneAdapters::EcsModelAssetLoadDesc desc,
            std::string& outError) override;
        [[nodiscard]] bool CancelModel(
            ResourceSceneAdapters::EcsModelAssetLoadRef request) override;
        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus> GetModelStatus(
            ResourceSceneAdapters::EcsModelAssetLoadRef request) const override;

        [[nodiscard]] ResourceSceneAdapters::EcsEnvironmentLoadRef RequestEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadDesc desc,
            std::string& outError) override;
        [[nodiscard]] bool CancelEnvironment(
            ResourceSceneAdapters::EcsEnvironmentLoadRef request) override;
        [[nodiscard]] std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
        GetEnvironmentStatus(
            ResourceSceneAdapters::EcsEnvironmentLoadRef request) const override;

        [[nodiscard]] AnimationSceneAdapters::EcsAnimationAssetLoadRef RequestAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadDesc desc,
            std::string& outError) override;
        [[nodiscard]] bool CancelAnimation(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request) override;
        [[nodiscard]] std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
        GetAnimationStatus(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request) const override;
        [[nodiscard]] AnimationSceneAdapters::EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            AnimationSceneAdapters::EcsAnimationAssetLoadRef request,
            SceneECS::SceneEntityRef targetEntity,
            uint32 animationClipOrdinal = 0) override;
        [[nodiscard]] WorldEcsAnimationPhysicsBindingResult
        BindAnimationRootMotionPhysics(
            SceneECS::SceneEntityRef targetEntity) override;

        [[nodiscard]] WorldEcsRuntimeServicesDiagnostics
        GetRuntimeDiagnostics() const override;

        /** @brief Close Scene submissions and cancel every tracked Resource-owned request once. */
        [[nodiscard]] bool BeginShutdown();
        /**
         * @brief Continue the host drain until all cleanup and external closure evidence resolves.
         *
         * A false return deliberately preserves processors, bridge state, and exact receipts for
         * a later owner-thread Tick. Processor clearing occurs only after every drain predicate.
         */
        [[nodiscard]] bool PrepareForShutdown(
            const WorldEcsRuntimeCompositionTickRequest& request = {});

        [[nodiscard]] bool IsShutdownComplete() const noexcept { return m_shutdownComplete; }
        /**
         * @brief True while the captured presentation camera is retained for external Render proof.
         *
         * The global render-runtime owner should publish the latest frozen Scene only while this
         * is true. Once false during shutdown, the final camera retirement is intentionally a
         * no-publication Scene cleanup step.
         */
        [[nodiscard]] bool NeedsRenderPublicationDuringShutdown() const noexcept;
        [[nodiscard]] WorldEcsRuntimeCompositionDiagnostics GetDiagnostics() const;

        [[nodiscard]] Physics::PhysicsWorld& GetPhysicsWorld() noexcept { return m_physicsWorld; }
        [[nodiscard]] const Physics::PhysicsWorld& GetPhysicsWorld() const noexcept
        {
            return m_physicsWorld;
        }
        [[nodiscard]] PhysicsSceneAdapters::PhysicsEcsBridge* GetPhysicsBridge() noexcept
        {
            return m_physicsBridge.get();
        }
        [[nodiscard]] AnimationSceneAdapters::AnimationEcsBridge* GetAnimationBridge() noexcept
        {
            return m_animationBridge.get();
        }

    private:
        struct ResourceCoordinatorUpdateResult
        {
            bool modelUpdated = true;
            bool environmentUpdated = true;
        };

        [[nodiscard]] bool RegisterProcessors();
        void RollBackInitialization(std::string diagnostic) noexcept;
        [[nodiscard]] ResourceCoordinatorUpdateResult UpdateResourceCoordinators();
        [[nodiscard]] bool UpdateAnimationAssetService();
        [[nodiscard]] bool ConfirmPresentations(
            WorldEcsRuntimeCompositionTickResult& result);
        void PruneInvalidTrackedHandles();
        void CancelTrackedRequests() noexcept;
        [[nodiscard]] bool IsAlivePresentationCamera(ECS::EntityHandle entity) const noexcept;
        void ClearInvalidShutdownPresentationCamera() noexcept;
        [[nodiscard]] bool RequestDestroyRemainingAlive();
        [[nodiscard]] bool IsShutdownDrained() noexcept;
        [[nodiscard]] bool ReleaseRuntimeStateAfterDrain() noexcept;
        [[nodiscard]] bool RemoveCompositionProcessorScope() noexcept;
        void SetDiagnostic(std::string diagnostic) noexcept;

        // Non-owning World/Engine services outlive this isolated assembly.
        SceneECS::SceneEcsRuntime& m_runtime;
        EcsRenderFramePipeline& m_renderPipeline;
        Resource::ResourceSubsystem* m_resources = nullptr;
        WorldEcsRuntimeCompositionOptions m_options;

        // PhysicsWorld precedes all bridges so it outlives PhysicsEcsBridge destruction.
        Physics::PhysicsWorld m_physicsWorld;
        std::unique_ptr<PhysicsSceneAdapters::PhysicsEcsBridge> m_physicsBridge;
        std::unique_ptr<AnimationSceneAdapters::AnimationEcsBridge> m_animationBridge;
        std::unique_ptr<Audio::AudioEcsBridge> m_audioBridge;
        std::unique_ptr<Scripting::ScriptEcsBridge> m_scriptBridge;
        std::unique_ptr<Particle::ParticleEcsBridge> m_particleBridge;
        std::unique_ptr<Water::WaterEcsBridge> m_waterBridge;
        std::unique_ptr<Terrain::TerrainEcsBridge> m_terrainBridge;
        std::unique_ptr<EcsRenderEntityRetirementCoordinator> m_genericRetirement;
        std::unique_ptr<ResourceSceneAdapters::EcsSceneAssetLoadCoordinator> m_modelCoordinator;
        std::unique_ptr<ResourceSceneAdapters::EcsEnvironmentLoadCoordinator>
            m_environmentCoordinator;

        std::optional<SceneECS::ProcessorRegistrationScope> m_processorRegistrationScope;
        std::vector<ResourceSceneAdapters::EcsSceneAssetLoadHandle> m_modelHandles;
        std::vector<ResourceSceneAdapters::EcsEnvironmentLoadHandle> m_environmentHandles;
        std::vector<AnimationSceneAdapters::EcsAnimationAssetLoadRef> m_animationAssetRequests;
        ECS::EntityHandle m_shutdownPresentationCamera = ECS::EntityHandle::Invalid();
        bool m_initialized = false;
        bool m_shutdownBegun = false;
        bool m_shutdownComplete = false;
        bool m_processorsCleared = false;
        bool m_cameraDestroyRequested = false;
        bool m_lastTickSucceeded = false;
        uint64 m_requestedFixedStepCount = 0;
        uint64 m_executedFixedStepCount = 0;
        uint32 m_lastRequestedFixedStepCount = 0;
        uint32 m_lastExecutedFixedStepCount = 0;
        std::string m_lastDiagnostic;
    };
} // namespace RVX
