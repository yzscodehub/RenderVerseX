#include "WorldEcsRuntimeComposition.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Particle/ECS/ParticleEcsBridge.h"
#include "Resource/ResourceSubsystem.h"
#include "Terrain/ECS/TerrainEcsBridge.h"
#include "Water/ECS/WaterEcsBridge.h"

#include <algorithm>
#include <utility>

namespace RVX
{
namespace
{
    template<typename T>
    void EraseInvalidHandles(std::vector<T>& handles, const auto& coordinator)
    {
        handles.erase(
            std::remove_if(handles.begin(),
                           handles.end(),
                           [&coordinator](T handle) { return !coordinator.IsValid(handle); }),
            handles.end());
    }

    [[nodiscard]] bool IsSceneDrained(const SceneECS::SceneEcsDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.entityCount == 0 && diagnostics.pendingDestroyCount == 0 &&
               diagnostics.cleanupRequiredCount == 0 && diagnostics.retiringCount == 0 &&
               diagnostics.recyclableCount == 0;
    }

    [[nodiscard]] bool IsSceneDrainedExceptPresentationCamera(
        const SceneECS::SceneEcsDiagnosticsSnapshot& diagnostics,
        bool presentationCameraAlive)
    {
        return presentationCameraAlive && diagnostics.entityCount == 1 &&
               diagnostics.pendingDestroyCount == 0 && diagnostics.cleanupRequiredCount == 0 &&
               diagnostics.retiringCount == 0 && diagnostics.recyclableCount == 0;
    }

    [[nodiscard]] bool IsPhysicsDrained(
        const PhysicsSceneAdapters::PhysicsEcsBridgeDiagnosticsSnapshot& bridge,
        const Physics::PhysicsRuntimeDiagnosticsSnapshot& physics)
    {
        return bridge.activeBodyCount == 0 && bridge.pendingCleanupCount == 0 &&
               physics.staticBodyCount == 0 && physics.dynamicBodyCount == 0 &&
               physics.kinematicBodyCount == 0 && physics.colliderCount == 0;
    }

    [[nodiscard]] bool IsAnimationDrained(
        const AnimationSceneAdapters::AnimationEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.activeBindingCount == 0 && diagnostics.pendingCleanupCount == 0;
    }

    [[nodiscard]] bool IsAudioDrained(const Audio::AudioEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.activePlaybackCount == 0 && diagnostics.outstandingPlaybackCount == 0 &&
               diagnostics.pendingCleanupCount == 0;
    }

    [[nodiscard]] bool IsScriptDrained(const Scripting::ScriptEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.activeInstanceCount == 0 && diagnostics.outstandingInstanceCount == 0 &&
               diagnostics.pendingCleanupCount == 0;
    }

    [[nodiscard]] bool IsParticleDrained(
        const Particle::ParticleEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.bindingCount == 0 && diagnostics.outstandingRuntimeCount == 0 &&
               diagnostics.publishedSnapshotCount == 0 && diagnostics.pendingCleanupCount == 0;
    }

    [[nodiscard]] bool IsWaterDrained(const Water::WaterEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.bindingCount == 0 && diagnostics.outstandingRuntimeCount == 0 &&
               diagnostics.publishedSnapshotCount == 0 && diagnostics.pendingCleanupCount == 0;
    }

    [[nodiscard]] bool IsTerrainDrained(
        const Terrain::TerrainEcsBridgeDiagnosticsSnapshot& diagnostics)
    {
        return diagnostics.bindingCount == 0 && diagnostics.outstandingRuntimeCount == 0 &&
               diagnostics.publishedSnapshotCount == 0 && diagnostics.pendingCleanupCount == 0;
    }

    template<typename TFeatureDiagnostics>
    [[nodiscard]] WorldEcsFeatureBridgeDiagnostics ToFeatureBridgeDiagnostics(
        const TFeatureDiagnostics& diagnostics)
    {
        return {
            .bindingCount = diagnostics.bindingCount,
            .outstandingRuntimeCount = diagnostics.outstandingRuntimeCount,
            .publishedSnapshotCount = diagnostics.publishedSnapshotCount,
            .pendingCleanupCount = diagnostics.pendingCleanupCount,
            .structuralContinuityLossCount = diagnostics.structuralContinuityLossCount,
            .cleanupContinuityLossCount = diagnostics.cleanupContinuityLossCount,
            .authoritativeReconcileCount = diagnostics.authoritativeReconcileCount,
        };
    }
} // namespace

WorldEcsRuntimeComposition::WorldEcsRuntimeComposition(
    SceneECS::SceneEcsRuntime& runtime,
    EcsRenderFramePipeline& renderPipeline,
    Resource::ResourceSubsystem* resources,
    WorldEcsRuntimeCompositionOptions options) noexcept
    : m_runtime(runtime)
    , m_renderPipeline(renderPipeline)
    , m_resources(resources)
    , m_options(std::move(options))
{
}

WorldEcsRuntimeComposition::~WorldEcsRuntimeComposition() noexcept
{
    if (m_initialized && !m_shutdownComplete)
    {
        static_cast<void>(PrepareForShutdown());
    }
    RVX_ASSERT_MSG(!m_initialized || m_shutdownComplete,
                   "WorldEcsRuntimeComposition destruction requires a completed ECS shutdown drain.");
}

bool WorldEcsRuntimeComposition::Initialize()
{
    if (m_initialized)
    {
        return true;
    }
    if (m_shutdownBegun || m_shutdownComplete)
    {
        SetDiagnostic("Cannot initialize an ECS World composition after shutdown has begun.");
        return false;
    }

    try
    {
        Physics::PhysicsWorldConfig config = m_options.physicsConfig;
        config.backend = Physics::PhysicsBackendType::BuiltIn;
        if (!m_physicsWorld.Initialize(config))
        {
            RollBackInitialization("Built-in PhysicsWorld initialization failed.");
            return false;
        }

        m_physicsBridge = std::make_unique<PhysicsSceneAdapters::PhysicsEcsBridge>(
            m_runtime, m_physicsWorld);
        AnimationSceneAdapters::AnimationEcsPoseEvaluator animationEvaluator =
            m_options.animationEvaluator;
        AnimationSceneAdapters::AnimationEcsCleanupCallback animationCleanup;
        if (m_options.resourceAnimationEvaluator != nullptr)
        {
            if (m_options.resourceAnimationEvaluator->IsShutdown())
            {
                RollBackInitialization(
                    "Injected ResourceAnimationEcsEvaluator was already shut down.");
                return false;
            }
            animationEvaluator = m_options.resourceAnimationEvaluator->CreatePoseEvaluator();
            animationCleanup = m_options.resourceAnimationEvaluator->CreateCleanupCallback();
        }
        if (m_options.animationAssetService != nullptr &&
            m_options.animationAssetService->IsShutdown())
        {
            RollBackInitialization("Injected EcsAnimationAssetService was already shut down.");
            return false;
        }
        m_animationBridge = std::make_unique<AnimationSceneAdapters::AnimationEcsBridge>(
            m_runtime,
            std::move(animationEvaluator),
            [this](ECS::SceneRuntimeId sceneRuntimeId,
                   ECS::EntityHandle entity,
                   uint64 packedPhysicsBody) noexcept
            {
                if (m_physicsBridge == nullptr)
                {
                    return false;
                }
                const Physics::BodyHandle body = m_physicsBridge->FindBody(sceneRuntimeId, entity);
                return body.IsValid() && body.GetPackedValue() == packedPhysicsBody;
            },
            std::move(animationCleanup));
        if (m_options.audioGateway != nullptr)
        {
            m_audioBridge = std::make_unique<Audio::AudioEcsBridge>(
                m_runtime, *m_options.audioGateway);
        }
        if (m_options.scriptGateway != nullptr)
        {
            m_scriptBridge = std::make_unique<Scripting::ScriptEcsBridge>(
                m_runtime, *m_options.scriptGateway);
        }
        if (m_options.particleGateway != nullptr)
        {
            m_particleBridge = std::make_unique<Particle::ParticleEcsBridge>(
                m_runtime, *m_options.particleGateway);
        }
        if (m_options.waterGateway != nullptr)
        {
            m_waterBridge = std::make_unique<Water::WaterEcsBridge>(
                m_runtime, *m_options.waterGateway);
        }
        if (m_options.terrainGateway != nullptr)
        {
            m_terrainBridge = std::make_unique<Terrain::TerrainEcsBridge>(
                m_runtime, *m_options.terrainGateway);
        }
        m_processorRegistrationScope = m_runtime.BeginProcessorRegistrationScope();
        if (!m_processorRegistrationScope.has_value())
        {
            RollBackInitialization(
                "Could not acquire the exclusive Scene processor registration scope.");
            return false;
        }
        m_genericRetirement = std::make_unique<EcsRenderEntityRetirementCoordinator>(
            m_runtime, m_renderPipeline.GetAssetRetirementProofGateway());
        if (!m_genericRetirement->GetDiagnostics().processorRegistered || !RegisterProcessors() ||
            !m_runtime.CompileProcessors() ||
            !m_runtime.CloseProcessorRegistrationScope(*m_processorRegistrationScope))
        {
            RollBackInitialization("ECS bridge processor registration or unified compilation failed.");
            return false;
        }
        if (m_resources != nullptr)
        {
            m_modelCoordinator =
                std::make_unique<ResourceSceneAdapters::EcsSceneAssetLoadCoordinator>(
                    m_runtime,
                    *m_resources,
                    &m_renderPipeline.GetAssetRetirementProofGateway());
            m_environmentCoordinator =
                std::make_unique<ResourceSceneAdapters::EcsEnvironmentLoadCoordinator>(
                    m_runtime,
                    *m_resources,
                    &m_renderPipeline.GetAssetRetirementProofGateway());
        }
    }
    catch (...)
    {
        RollBackInitialization("ECS World composition allocation failed during initialization.");
        return false;
    }

    m_initialized = true;
    m_processorsCleared = false;
    return true;
}

WorldEcsRuntimeCompositionTickResult WorldEcsRuntimeComposition::Tick(
    const WorldEcsRuntimeCompositionTickRequest& request)
{
    WorldEcsRuntimeCompositionTickResult result;
    if (!m_initialized || m_shutdownComplete)
    {
        SetDiagnostic("Tick requested while the ECS World composition is not active.");
        return result;
    }

    const ResourceCoordinatorUpdateResult initialUpdate = UpdateResourceCoordinators();
    result.modelCoordinatorUpdated = initialUpdate.modelUpdated;
    result.environmentCoordinatorUpdated = initialUpdate.environmentUpdated;
    result.animationAssetServiceUpdated = UpdateAnimationAssetService();
    if (!result.animationAssetServiceUpdated)
    {
        m_lastTickSucceeded = false;
        SetDiagnostic("Shared animation asset service did not update the exact ECS Scene before tick.");
        return result;
    }
    result.scene = m_runtime.Tick(request.sceneTick);
    m_lastRequestedFixedStepCount = request.sceneTick.fixedStepCount;
    m_requestedFixedStepCount += request.sceneTick.fixedStepCount;
    m_lastExecutedFixedStepCount = result.scene.fixedStepsExecuted;
    m_executedFixedStepCount += result.scene.fixedStepsExecuted;
    if (!result.scene.succeeded)
    {
        m_lastTickSucceeded = false;
        std::string diagnostic =
            "Scene ECS tick failed before presentation proof observation.";
        if (result.scene.processorFailure.has_value())
        {
            const ECS::ProcessorExecutionFailure& failure =
                *result.scene.processorFailure;
            diagnostic = "Scene ECS processor '" + failure.processorName +
                         "' failed at frame " +
                         std::to_string(failure.frameSequence) +
                         ", fixed step " +
                         std::to_string(failure.fixedStepSequence) + ": " +
                         failure.reason;
        }
        if (diagnostic != m_lastDiagnostic)
        {
            RVX_CORE_ERROR("{}", diagnostic);
        }
        SetDiagnostic(std::move(diagnostic));
        return result;
    }

    const bool presentationsConfirmed = ConfirmPresentations(result);
    const ResourceCoordinatorUpdateResult postPresentationUpdate = UpdateResourceCoordinators();
    result.modelCoordinatorUpdated =
        result.modelCoordinatorUpdated && postPresentationUpdate.modelUpdated;
    result.environmentCoordinatorUpdated =
        result.environmentCoordinatorUpdated && postPresentationUpdate.environmentUpdated;
    PruneInvalidTrackedHandles();
    if (!result.modelCoordinatorUpdated || !result.environmentCoordinatorUpdated)
    {
        SetDiagnostic("A Resource-owned ECS coordinator update failed; its state remains retained for retry.");
    }
    result.succeeded = result.modelCoordinatorUpdated && result.environmentCoordinatorUpdated &&
                       result.animationAssetServiceUpdated && result.scene.succeeded &&
                       presentationsConfirmed;
    m_lastTickSucceeded = result.succeeded;
    return result;
}

ResourceSceneAdapters::EcsModelAssetLoadRef WorldEcsRuntimeComposition::RequestModel(
    ResourceSceneAdapters::EcsModelAssetLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    if (!m_initialized || m_shutdownBegun || m_modelCoordinator == nullptr)
    {
        outError = "ECS model requests require an active composition with ResourceSubsystem support.";
        return {};
    }

    const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
    if (!desc.expectedSceneRuntimeId.IsValid())
    {
        desc.expectedSceneRuntimeId = sceneRuntimeId;
    }
    if (desc.expectedSceneRuntimeId != sceneRuntimeId)
    {
        outError = "ECS model request specified a foreign Scene runtime id.";
        return {};
    }

    ResourceSceneAdapters::EcsSceneAssetLoadHandle handle =
        m_modelCoordinator->RequestModel(std::move(desc), outError);
    if (handle.IsValid())
    {
        m_modelHandles.push_back(handle);
        return {.sceneRuntimeId = sceneRuntimeId, .handle = handle};
    }
    return {};
}

bool WorldEcsRuntimeComposition::CancelModel(
    ResourceSceneAdapters::EcsModelAssetLoadRef request)
{
    return m_modelCoordinator != nullptr && request.IsValid() &&
           request.sceneRuntimeId == m_runtime.GetSceneRuntimeId() &&
           m_modelCoordinator->Cancel(request.handle);
}

std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>
WorldEcsRuntimeComposition::GetModelStatus(
    ResourceSceneAdapters::EcsModelAssetLoadRef request) const
{
    return m_modelCoordinator != nullptr && request.IsValid() &&
                   request.sceneRuntimeId == m_runtime.GetSceneRuntimeId() ?
               m_modelCoordinator->GetStatus(request.handle) :
               std::nullopt;
}

ResourceSceneAdapters::EcsEnvironmentLoadRef WorldEcsRuntimeComposition::RequestEnvironment(
    ResourceSceneAdapters::EcsEnvironmentLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    if (!m_initialized || m_shutdownBegun || m_environmentCoordinator == nullptr)
    {
        outError = "ECS environment requests require an active composition with ResourceSubsystem support.";
        return {};
    }

    const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
    if (!desc.expectedSceneRuntimeId.IsValid())
    {
        desc.expectedSceneRuntimeId = sceneRuntimeId;
    }
    if (desc.expectedSceneRuntimeId != sceneRuntimeId)
    {
        outError = "ECS environment request specified a foreign Scene runtime id.";
        return {};
    }

    ResourceSceneAdapters::EcsEnvironmentLoadHandle handle =
        m_environmentCoordinator->RequestEnvironment(std::move(desc), outError);
    if (handle.IsValid())
    {
        m_environmentHandles.push_back(handle);
        return {.sceneRuntimeId = sceneRuntimeId, .handle = handle};
    }
    return {};
}

bool WorldEcsRuntimeComposition::CancelEnvironment(
    ResourceSceneAdapters::EcsEnvironmentLoadRef request)
{
    return m_environmentCoordinator != nullptr && request.IsValid() &&
           request.sceneRuntimeId == m_runtime.GetSceneRuntimeId() &&
           m_environmentCoordinator->Cancel(request.handle);
}

std::optional<ResourceSceneAdapters::EcsEnvironmentLoadStatus>
WorldEcsRuntimeComposition::GetEnvironmentStatus(
    ResourceSceneAdapters::EcsEnvironmentLoadRef request) const
{
    return m_environmentCoordinator != nullptr && request.IsValid() &&
                   request.sceneRuntimeId == m_runtime.GetSceneRuntimeId() ?
               m_environmentCoordinator->GetStatus(request.handle) :
               std::nullopt;
}

AnimationSceneAdapters::EcsAnimationAssetLoadRef
WorldEcsRuntimeComposition::RequestAnimation(
    AnimationSceneAdapters::EcsAnimationAssetLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    if (!m_initialized || m_shutdownBegun || m_options.animationAssetService == nullptr)
    {
        outError = "ECS animation requests require an active shared animation asset service.";
        return {};
    }

    const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
    if (!desc.expectedSceneRuntimeId.IsValid())
    {
        desc.expectedSceneRuntimeId = sceneRuntimeId;
    }
    if (desc.expectedSceneRuntimeId != sceneRuntimeId)
    {
        outError = "ECS animation request specified a foreign Scene runtime id.";
        return {};
    }

    const AnimationSceneAdapters::EcsAnimationAssetLoadRef request =
        m_options.animationAssetService->Request(std::move(desc), outError);
    if (request.IsValid())
    {
        m_animationAssetRequests.push_back(request);
    }
    return request;
}

bool WorldEcsRuntimeComposition::CancelAnimation(
    AnimationSceneAdapters::EcsAnimationAssetLoadRef request)
{
    if (m_options.animationAssetService == nullptr ||
        request.sceneRuntimeId != m_runtime.GetSceneRuntimeId() ||
        !m_options.animationAssetService->Cancel(request))
    {
        return false;
    }

    m_animationAssetRequests.erase(
        std::remove(m_animationAssetRequests.begin(),
                    m_animationAssetRequests.end(),
                    request),
        m_animationAssetRequests.end());
    return true;
}

std::optional<AnimationSceneAdapters::EcsAnimationAssetLoadStatus>
WorldEcsRuntimeComposition::GetAnimationStatus(
    AnimationSceneAdapters::EcsAnimationAssetLoadRef request) const
{
    return m_options.animationAssetService != nullptr &&
                   request.sceneRuntimeId == m_runtime.GetSceneRuntimeId() ?
               m_options.animationAssetService->GetStatus(request) :
               std::nullopt;
}

AnimationSceneAdapters::EcsAnimationBindingPreparationResult
WorldEcsRuntimeComposition::PrepareCompatibleAnimationBinding(
    AnimationSceneAdapters::EcsAnimationAssetLoadRef request,
    SceneECS::SceneEntityRef targetEntity,
    uint32 animationClipOrdinal)
{
    AnimationSceneAdapters::EcsAnimationBindingPreparationResult result;
    if (!m_initialized || m_shutdownBegun || m_options.animationAssetService == nullptr)
    {
        result.code = AnimationSceneAdapters::EcsAnimationBindingPreparationCode::ServiceShutDown;
        result.diagnostic = "Animation binding preparation requires an active shared service.";
        return result;
    }

    const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
    const SceneECS::EntityLifecycleState* const lifecycle =
        m_runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(targetEntity.entity);
    if (request.sceneRuntimeId != sceneRuntimeId || !targetEntity.IsValid() ||
        targetEntity.sceneRuntimeId != sceneRuntimeId ||
        m_runtime.GetEntityRef(targetEntity.entity) != targetEntity || lifecycle == nullptr ||
        lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive)
    {
        result.code = AnimationSceneAdapters::EcsAnimationBindingPreparationCode::InvalidTargetEntity;
        result.diagnostic =
            "Animation binding preparation rejected a stale, foreign, or non-Alive target entity.";
        return result;
    }

    const SceneECS::AnimationSkeletonBinding* const targetBinding =
        m_runtime.GetRegistry().TryGet<SceneECS::AnimationSkeletonBinding>(targetEntity.entity);
    if (targetBinding == nullptr)
    {
        result.code =
            AnimationSceneAdapters::EcsAnimationBindingPreparationCode::TargetBindingUnavailable;
        result.diagnostic = "Animation binding preparation target has no current skeleton binding.";
        return result;
    }

    result = m_options.animationAssetService->PrepareCompatibleAnimationBinding(
        request, *targetBinding, animationClipOrdinal);
    if (result.IsPrepared() &&
        !m_runtime.SetFragment<SceneECS::AnimationSkeletonBinding>(targetEntity.entity,
                                                                    result.binding))
    {
        result.code =
            AnimationSceneAdapters::EcsAnimationBindingPreparationCode::TargetBindingMutationRejected;
        result.diagnostic = "Animation binding target stopped accepting an exact fragment replacement.";
    }
    return result;
}

WorldEcsAnimationPhysicsBindingResult
WorldEcsRuntimeComposition::BindAnimationRootMotionPhysics(
    SceneECS::SceneEntityRef targetEntity)
{
    WorldEcsAnimationPhysicsBindingResult result;
    if (!m_initialized || m_shutdownBegun || m_physicsBridge == nullptr ||
        m_animationBridge == nullptr)
    {
        result.code = WorldEcsAnimationPhysicsBindingCode::ServiceUnavailable;
        result.diagnostic =
            "Animation-to-Physics binding requires an active ECS World composition.";
        return result;
    }

    const ECS::SceneRuntimeId sceneRuntimeId = m_runtime.GetSceneRuntimeId();
    if (!targetEntity.IsValid() || targetEntity.sceneRuntimeId != sceneRuntimeId)
    {
        result.code = WorldEcsAnimationPhysicsBindingCode::ForeignScene;
        result.diagnostic =
            "Animation-to-Physics binding rejected a foreign Scene identity.";
        return result;
    }

    const SceneECS::EntityLifecycleState* const lifecycle =
        m_runtime.GetRegistry().TryGet<SceneECS::EntityLifecycleState>(targetEntity.entity);
    if (m_runtime.GetEntityRef(targetEntity.entity) != targetEntity || lifecycle == nullptr ||
        lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive)
    {
        result.code = WorldEcsAnimationPhysicsBindingCode::InvalidEntity;
        result.diagnostic =
            "Animation-to-Physics binding rejected a stale or non-Alive entity.";
        return result;
    }

    const Physics::BodyHandle body =
        m_physicsBridge->FindBody(sceneRuntimeId, targetEntity.entity);
    if (!body.IsValid())
    {
        result.code = WorldEcsAnimationPhysicsBindingCode::PhysicsBodyUnavailable;
        result.diagnostic =
            "Animation-to-Physics binding is waiting for an active ECS Physics body.";
        return result;
    }

    const AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult bridgeResult =
        m_animationBridge->BindRootMotionPhysicsBody(
            sceneRuntimeId, targetEntity.entity, body.GetPackedValue());
    switch (bridgeResult)
    {
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::Applied:
        result.code = WorldEcsAnimationPhysicsBindingCode::Applied;
        result.diagnostic = "Animation root motion is bound to the entity's exact Physics body.";
        return result;
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::ForeignScene:
        result.code = WorldEcsAnimationPhysicsBindingCode::ForeignScene;
        result.diagnostic = "Animation bridge rejected the Scene identity.";
        return result;
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidEntity:
        result.code = WorldEcsAnimationPhysicsBindingCode::InvalidEntity;
        result.diagnostic = "Animation bridge rejected the entity generation.";
        return result;
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::InvalidPhysicsHandle:
        result.code = WorldEcsAnimationPhysicsBindingCode::PhysicsBodyUnavailable;
        result.diagnostic = "Physics body generation changed before the binding was applied.";
        return result;
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::AnimationEntityNotBound:
        result.code = WorldEcsAnimationPhysicsBindingCode::AnimationBindingUnavailable;
        result.diagnostic =
            "Animation bridge has not reconciled an active binding for the entity.";
        return result;
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::PhysicsValidationUnavailable:
    case AnimationSceneAdapters::AnimationRootMotionPhysicsBindingResult::BindingMismatch:
    default:
        result.code = WorldEcsAnimationPhysicsBindingCode::Rejected;
        result.diagnostic = "Animation bridge rejected the exact Physics binding proof.";
        return result;
    }
}

WorldEcsRuntimeServicesDiagnostics
WorldEcsRuntimeComposition::GetRuntimeDiagnostics() const
{
    const WorldEcsRuntimeCompositionDiagnostics composition = GetDiagnostics();
    WorldEcsRuntimeServicesDiagnostics diagnostics;
    diagnostics.available = true;
    diagnostics.initialized = composition.initialized;
    diagnostics.shutdownBegun = composition.shutdownBegun;
    diagnostics.shutdownComplete = composition.shutdownComplete;
    diagnostics.lastTickSucceeded = m_lastTickSucceeded;
    diagnostics.sceneRuntimeId = m_runtime.GetSceneRuntimeId().GetValue();
    diagnostics.sceneFrameSequence = composition.scene.frameSequence;
    diagnostics.sceneFixedStepSequence = composition.scene.fixedStepSequence;
    diagnostics.physicsFixedStepSequence = composition.physics.fixedStepSequence;
    diagnostics.requestedFixedStepCount = m_requestedFixedStepCount;
    diagnostics.executedFixedStepCount = m_executedFixedStepCount;
    diagnostics.lastRequestedFixedStepCount = m_lastRequestedFixedStepCount;
    diagnostics.lastExecutedFixedStepCount = m_lastExecutedFixedStepCount;
    diagnostics.physicsInitialized = composition.physicsInitialized;
    diagnostics.requestedPhysicsBackend = m_physicsWorld.GetRequestedBackendType();
    diagnostics.activePhysicsBackend = m_physicsWorld.GetActiveBackendType();
    diagnostics.physicsBackendFallbackActive = m_physicsWorld.IsBackendFallbackActive();
    diagnostics.physicsBridgeBindingSideTableEntryCount =
        static_cast<uint32>(composition.physicsBridge.bindings.size());
    diagnostics.activePhysicsBodyCount = composition.physicsBridge.activeBodyCount;
    diagnostics.pendingPhysicsBridgeCleanupCount = composition.physicsBridge.pendingCleanupCount;
    diagnostics.staticPhysicsBodyCount = static_cast<uint32>(composition.physics.staticBodyCount);
    diagnostics.dynamicPhysicsBodyCount = static_cast<uint32>(composition.physics.dynamicBodyCount);
    diagnostics.kinematicPhysicsBodyCount =
        static_cast<uint32>(composition.physics.kinematicBodyCount);
    diagnostics.physicsColliderCount = static_cast<uint32>(composition.physics.colliderCount);
    diagnostics.physicsBridgeStructuralContinuityLossCount =
        composition.physicsBridge.structuralContinuityLossCount;
    diagnostics.physicsBridgeCleanupContinuityLossCount =
        composition.physicsBridge.cleanupContinuityLossCount;
    diagnostics.physicsBridgeAuthoritativeReconcileCount =
        composition.physicsBridge.authoritativeReconcileCount;
    diagnostics.physicsRootMotionAppliedCount =
        composition.physicsBridge.rootMotionAppliedCount;
    diagnostics.physicsRootMotionRejectedCount =
        composition.physicsBridge.rootMotionRejectedCount;
    diagnostics.physicsRootMotionReplayCount =
        composition.physicsBridge.rootMotionReplayCount;
    diagnostics.physicsRootMotionGapCount =
        composition.physicsBridge.rootMotionGapCount;
    diagnostics.animationBridgeAvailable = m_animationBridge != nullptr;
    diagnostics.animationBindingSideTableEntryCount =
        static_cast<uint32>(composition.animationBridge.bindings.size());
    diagnostics.activeAnimationBindingCount = composition.animationBridge.activeBindingCount;
    diagnostics.pendingAnimationBridgeCleanupCount =
        composition.animationBridge.pendingCleanupCount;
    diagnostics.animationFixedEvaluationCount =
        composition.animationBridge.fixedEvaluationCount;
    diagnostics.animationRejectedEvaluationCount =
        composition.animationBridge.rejectedEvaluationCount;
    diagnostics.animationRootMotionPublicationCount =
        composition.animationBridge.rootMotionPublicationCount;
    diagnostics.animationRootMotionReplayCount =
        composition.animationBridge.rootMotionReplayCount;
    diagnostics.animationRootMotionGapCount =
        composition.animationBridge.rootMotionGapCount;
    diagnostics.resourceAnimationEvaluatorAvailable =
        composition.resourceAnimationEvaluator.has_value();
    diagnostics.resourceAnimationPlaybackSideTableEntryCount =
        composition.resourceAnimationEvaluator.has_value() ?
            composition.resourceAnimationEvaluator->activePlaybackCount :
            0;
    diagnostics.audioBridgeAvailable = composition.audioBridge.has_value();
    if (composition.audioBridge.has_value())
    {
        diagnostics.audioPlaybackSideTableEntryCount =
            static_cast<uint32>(composition.audioBridge->bindings.size());
        diagnostics.activeAudioPlaybackCount = composition.audioBridge->activePlaybackCount;
        diagnostics.outstandingAudioPlaybackCount =
            composition.audioBridge->outstandingPlaybackCount;
        diagnostics.pendingAudioBridgeCleanupCount =
            composition.audioBridge->pendingCleanupCount;
    }
    diagnostics.scriptBridgeAvailable = composition.scriptBridge.has_value();
    if (composition.scriptBridge.has_value())
    {
        diagnostics.scriptInstanceSideTableEntryCount =
            static_cast<uint32>(composition.scriptBridge->bindings.size());
        diagnostics.activeScriptInstanceCount = composition.scriptBridge->activeInstanceCount;
        diagnostics.outstandingScriptInstanceCount =
            composition.scriptBridge->outstandingInstanceCount;
        diagnostics.pendingScriptBridgeCleanupCount =
            composition.scriptBridge->pendingCleanupCount;
    }
    diagnostics.particleBridgeAvailable = composition.particleBridge.has_value();
    if (composition.particleBridge.has_value())
    {
        diagnostics.particleBindingSideTableEntryCount = composition.particleBridge->bindingCount;
        diagnostics.particleOutstandingRuntimeCount =
            composition.particleBridge->outstandingRuntimeCount;
        diagnostics.particlePublishedSnapshotCount =
            composition.particleBridge->publishedSnapshotCount;
        diagnostics.pendingParticleBridgeCleanupCount =
            composition.particleBridge->pendingCleanupCount;
        diagnostics.particleBridgeStructuralContinuityLossCount =
            composition.particleBridge->structuralContinuityLossCount;
        diagnostics.particleBridgeCleanupContinuityLossCount =
            composition.particleBridge->cleanupContinuityLossCount;
        diagnostics.particleBridgeAuthoritativeReconcileCount =
            composition.particleBridge->authoritativeReconcileCount;
    }
    diagnostics.waterBridgeAvailable = composition.waterBridge.has_value();
    if (composition.waterBridge.has_value())
    {
        diagnostics.waterBindingSideTableEntryCount = composition.waterBridge->bindingCount;
        diagnostics.waterOutstandingRuntimeCount = composition.waterBridge->outstandingRuntimeCount;
        diagnostics.waterPublishedSnapshotCount = composition.waterBridge->publishedSnapshotCount;
        diagnostics.pendingWaterBridgeCleanupCount = composition.waterBridge->pendingCleanupCount;
        diagnostics.waterBridgeStructuralContinuityLossCount =
            composition.waterBridge->structuralContinuityLossCount;
        diagnostics.waterBridgeCleanupContinuityLossCount =
            composition.waterBridge->cleanupContinuityLossCount;
        diagnostics.waterBridgeAuthoritativeReconcileCount =
            composition.waterBridge->authoritativeReconcileCount;
    }
    diagnostics.terrainBridgeAvailable = composition.terrainBridge.has_value();
    if (composition.terrainBridge.has_value())
    {
        diagnostics.terrainBindingSideTableEntryCount = composition.terrainBridge->bindingCount;
        diagnostics.terrainOutstandingRuntimeCount =
            composition.terrainBridge->outstandingRuntimeCount;
        diagnostics.terrainPublishedSnapshotCount =
            composition.terrainBridge->publishedSnapshotCount;
        diagnostics.pendingTerrainBridgeCleanupCount =
            composition.terrainBridge->pendingCleanupCount;
        diagnostics.terrainBridgeStructuralContinuityLossCount =
            composition.terrainBridge->structuralContinuityLossCount;
        diagnostics.terrainBridgeCleanupContinuityLossCount =
            composition.terrainBridge->cleanupContinuityLossCount;
        diagnostics.terrainBridgeAuthoritativeReconcileCount =
            composition.terrainBridge->authoritativeReconcileCount;
    }
    const SceneECS::SceneFeatureSnapshotStoreCounts featureSnapshots =
        m_runtime.GetFeatureSnapshotCounts();
    diagnostics.featureSnapshotStoreAvailable =
        m_runtime.GetSceneRuntimeId().IsValid();
    diagnostics.particleFeatureSnapshotCount = featureSnapshots.particleCount;
    diagnostics.waterFeatureSnapshotCount = featureSnapshots.waterCount;
    diagnostics.terrainFeatureSnapshotCount = featureSnapshots.terrainCount;
    diagnostics.featureSnapshotInstanceCount = featureSnapshots.particleCount +
                                               featureSnapshots.waterCount +
                                               featureSnapshots.terrainCount;
    diagnostics.trackedModelRequestCount = composition.trackedModelRequestCount;
    diagnostics.trackedEnvironmentRequestCount = composition.trackedEnvironmentRequestCount;
    diagnostics.trackedAnimationRequestCount =
        static_cast<uint32>(m_animationAssetRequests.size());
    if (m_options.animationAssetService != nullptr)
    {
        diagnostics.activeAnimationRequestCount =
            m_options.animationAssetService
                ->GetSceneDiagnosticsSnapshot(m_runtime.GetSceneRuntimeId())
                .activeRequestCount;
    }
    diagnostics.lastDiagnostic = composition.lastDiagnostic;
    return diagnostics;
}

bool WorldEcsRuntimeComposition::BeginShutdown()
{
    if (m_shutdownComplete)
    {
        return true;
    }
    if (!m_initialized)
    {
        SetDiagnostic("Cannot begin ECS World shutdown before initialization.");
        return false;
    }
    if (!m_shutdownBegun)
    {
        const std::shared_ptr<const SceneECS::FrozenSceneSnapshot> snapshot =
            m_runtime.GetLatestFrozenSnapshot();
        if (snapshot != nullptr && snapshot->sceneRuntimeId == m_runtime.GetSceneRuntimeId() &&
            snapshot->selectedCamera.has_value() &&
            IsAlivePresentationCamera(snapshot->selectedCamera->entity))
        {
            m_shutdownPresentationCamera = snapshot->selectedCamera->entity;
        }
        else
        {
            m_shutdownPresentationCamera = ECS::EntityHandle::Invalid();
        }
        m_runtime.BeginShutdown();
        m_shutdownBegun = true;
        CancelTrackedRequests();
    }
    return true;
}

bool WorldEcsRuntimeComposition::NeedsRenderPublicationDuringShutdown() const noexcept
{
    return m_shutdownBegun && !m_shutdownComplete && !m_cameraDestroyRequested &&
           m_shutdownPresentationCamera.IsValid() &&
           IsAlivePresentationCamera(m_shutdownPresentationCamera);
}

bool WorldEcsRuntimeComposition::PrepareForShutdown(
    const WorldEcsRuntimeCompositionTickRequest& request)
{
    if (m_shutdownComplete)
    {
        return true;
    }
    if (!BeginShutdown())
    {
        return false;
    }

    // Keep all bridge processors active while the owner-thread drain advances exact proof and
    // cleanup state. A failed Tick deliberately leaves every coordinator and receipt intact.
    const WorldEcsRuntimeCompositionTickResult tick = Tick(request);
    if (!tick.succeeded)
    {
        if (!tick.scene.succeeded)
        {
            SetDiagnostic("ECS World shutdown drain stopped because the Scene tick failed.");
        }
        else if (!tick.modelCoordinatorUpdated || !tick.environmentCoordinatorUpdated)
        {
            SetDiagnostic("ECS World shutdown drain stopped because a Resource coordinator update failed.");
        }
        else
        {
            SetDiagnostic("ECS World shutdown drain stopped because exact presentation confirmation failed.");
        }
        return false;
    }
    ClearInvalidShutdownPresentationCamera();

    const bool modelDrained = m_modelCoordinator == nullptr ||
                              m_modelCoordinator->PrepareForHostShutdown();
    const bool environmentDrained = m_environmentCoordinator == nullptr ||
                                    m_environmentCoordinator->PrepareForHostShutdown();
    PruneInvalidTrackedHandles();
    if (!modelDrained || !environmentDrained)
    {
        SetDiagnostic("Resource-owned ECS retirement is still retaining exact Render or closure evidence.");
        return false;
    }

    // The Resource coordinators have now released every specialized entity. This broad request
    // therefore applies only to the remaining direct ECS entities, without bypassing their
    // specialized Render retirement ownership.
    if (!RequestDestroyRemainingAlive())
    {
        SetDiagnostic("Could not atomically request destruction for remaining direct ECS entities.");
        return false;
    }

    const bool audioDrained = m_audioBridge == nullptr || m_audioBridge->PrepareForShutdown();
    const bool scriptDrained = m_scriptBridge == nullptr || m_scriptBridge->PrepareForShutdown();
    const bool particleDrained =
        m_particleBridge == nullptr || m_particleBridge->PrepareForShutdown();
    const bool waterDrained = m_waterBridge == nullptr || m_waterBridge->PrepareForShutdown();
    const bool terrainDrained =
        m_terrainBridge == nullptr || m_terrainBridge->PrepareForShutdown();
    if (!audioDrained)
    {
        SetDiagnostic("Audio ECS shutdown drain remains incomplete; continue owner-thread ticks.");
        return false;
    }
    if (!scriptDrained)
    {
        SetDiagnostic("Script ECS shutdown drain remains incomplete; continue owner-thread ticks.");
        return false;
    }
    if (!particleDrained)
    {
        SetDiagnostic("Particle ECS shutdown drain remains incomplete; continue owner-thread ticks.");
        return false;
    }
    if (!waterDrained)
    {
        SetDiagnostic("Water ECS shutdown drain remains incomplete; continue owner-thread ticks.");
        return false;
    }
    if (!terrainDrained)
    {
        SetDiagnostic("Terrain ECS shutdown drain remains incomplete; continue owner-thread ticks.");
        return false;
    }
    if (!IsShutdownDrained())
    {
        return false;
    }

    if (NeedsRenderPublicationDuringShutdown())
    {
        const SceneECS::DestroyRequestResult cameraDestroy =
            m_runtime.RequestDestroy(
                m_shutdownPresentationCamera,
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None));
        if (cameraDestroy != SceneECS::DestroyRequestResult::Accepted &&
            cameraDestroy != SceneECS::DestroyRequestResult::AlreadyPending)
        {
            SetDiagnostic("Captured presentation camera could not enter final Scene retirement.");
            return false;
        }
        m_cameraDestroyRequested = true;
        // RequestDestroy defaults to every cleanup domain for callers that do not know the
        // entity's ownership. The captured presentation camera is exact and has no inferred
        // cleanup owner, so use None and let Scene inference add only real fragment domains.
        // The next owner-thread Scene tick can therefore recycle it without asking the global
        // render pipeline to publish a camera-less frozen snapshot.
        return false;
    }

    return ReleaseRuntimeStateAfterDrain();
}

WorldEcsRuntimeCompositionDiagnostics WorldEcsRuntimeComposition::GetDiagnostics() const
{
    WorldEcsRuntimeCompositionDiagnostics diagnostics;
    diagnostics.initialized = m_initialized;
    diagnostics.shutdownBegun = m_shutdownBegun;
    diagnostics.shutdownComplete = m_shutdownComplete;
    diagnostics.processorsCleared = m_processorsCleared;
    diagnostics.physicsInitialized = m_physicsWorld.IsInitialized();
    diagnostics.trackedModelRequestCount = static_cast<uint32>(m_modelHandles.size());
    diagnostics.trackedEnvironmentRequestCount = static_cast<uint32>(m_environmentHandles.size());
    diagnostics.scene = m_runtime.GetDiagnosticsSnapshot();
    diagnostics.physics = m_physicsWorld.GetRuntimeDiagnosticsSnapshot();
    if (m_physicsBridge != nullptr)
    {
        diagnostics.physicsBridge = m_physicsBridge->GetDiagnosticsSnapshot();
    }
    if (m_animationBridge != nullptr)
    {
        diagnostics.animationBridge = m_animationBridge->GetDiagnosticsSnapshot();
    }
    if (m_audioBridge != nullptr)
    {
        diagnostics.audioBridge = m_audioBridge->GetDiagnosticsSnapshot();
    }
    if (m_scriptBridge != nullptr)
    {
        diagnostics.scriptBridge = m_scriptBridge->GetDiagnosticsSnapshot();
    }
    if (m_particleBridge != nullptr)
    {
        diagnostics.particleBridge = ToFeatureBridgeDiagnostics(
            m_particleBridge->GetDiagnosticsSnapshot());
    }
    if (m_waterBridge != nullptr)
    {
        diagnostics.waterBridge = ToFeatureBridgeDiagnostics(
            m_waterBridge->GetDiagnosticsSnapshot());
    }
    if (m_terrainBridge != nullptr)
    {
        diagnostics.terrainBridge = ToFeatureBridgeDiagnostics(
            m_terrainBridge->GetDiagnosticsSnapshot());
    }
    if (m_genericRetirement != nullptr)
    {
        diagnostics.genericRetirement = m_genericRetirement->GetDiagnostics();
    }
    if (m_resources != nullptr)
    {
        diagnostics.resources = m_resources->GetDiagnosticsSnapshot();
    }
    if (m_options.animationAssetService != nullptr)
    {
        diagnostics.animationAssetService =
            m_options.animationAssetService->GetSceneDiagnosticsSnapshot(
                m_runtime.GetSceneRuntimeId());
    }
    if (m_options.resourceAnimationEvaluator != nullptr)
    {
        diagnostics.resourceAnimationEvaluator =
            m_options.resourceAnimationEvaluator->GetDiagnosticsSnapshot();
    }
    diagnostics.lastDiagnostic = m_lastDiagnostic;
    return diagnostics;
}

bool WorldEcsRuntimeComposition::RegisterProcessors()
{
    return m_physicsBridge != nullptr && m_animationBridge != nullptr &&
           m_physicsBridge->RegisterProcessors() ==
               PhysicsSceneAdapters::PhysicsEcsBridgeRegistrationResult::Registered &&
           m_animationBridge->RegisterProcessors() ==
               AnimationSceneAdapters::AnimationEcsBridgeRegistrationResult::Registered &&
           (m_audioBridge == nullptr ||
            m_audioBridge->RegisterProcessors() == Audio::AudioEcsBridgeRegistrationResult::Registered) &&
           (m_scriptBridge == nullptr ||
            m_scriptBridge->RegisterProcessors() ==
                Scripting::ScriptEcsBridgeRegistrationResult::Registered) &&
           (m_particleBridge == nullptr ||
            m_particleBridge->RegisterProcessors() ==
                Particle::ParticleEcsBridgeRegistrationResult::Registered) &&
           (m_waterBridge == nullptr ||
            m_waterBridge->RegisterProcessors() ==
                Water::WaterEcsBridgeRegistrationResult::Registered) &&
           (m_terrainBridge == nullptr ||
            m_terrainBridge->RegisterProcessors() ==
                Terrain::TerrainEcsBridgeRegistrationResult::Registered);
}

void WorldEcsRuntimeComposition::RollBackInitialization(std::string diagnostic) noexcept
{
    const bool processorsRemoved = RemoveCompositionProcessorScope();
    m_modelCoordinator.reset();
    m_environmentCoordinator.reset();
    m_genericRetirement.reset();
    m_terrainBridge.reset();
    m_waterBridge.reset();
    m_particleBridge.reset();
    m_scriptBridge.reset();
    m_audioBridge.reset();
    m_animationBridge.reset();
    m_physicsBridge.reset();
    m_physicsWorld.Shutdown();
    m_initialized = false;
    m_processorsCleared = processorsRemoved;
    if (!processorsRemoved)
    {
        diagnostic += " Composition-owned Scene processors could not be atomically removed.";
    }
    SetDiagnostic(std::move(diagnostic));
}

WorldEcsRuntimeComposition::ResourceCoordinatorUpdateResult
WorldEcsRuntimeComposition::UpdateResourceCoordinators()
{
    return {
        .modelUpdated = m_modelCoordinator == nullptr || m_modelCoordinator->Update(),
        .environmentUpdated =
            m_environmentCoordinator == nullptr || m_environmentCoordinator->Update(),
    };
}

bool WorldEcsRuntimeComposition::UpdateAnimationAssetService()
{
    return m_options.animationAssetService == nullptr ||
           m_options.animationAssetService->Update(m_runtime.GetSceneRuntimeId());
}

bool WorldEcsRuntimeComposition::ConfirmPresentations(
    WorldEcsRuntimeCompositionTickResult& result)
{
    bool succeeded = true;
    if (m_modelCoordinator != nullptr)
    {
        for (const ResourceSceneAdapters::EcsSceneAssetLoadHandle handle : m_modelHandles)
        {
            const auto status = m_modelCoordinator->GetStatus(handle);
            if (!status.has_value() || status->state !=
                                           ResourceSceneAdapters::EcsSceneAssetLoadState::
                                               MinimumResidentPendingPresentation)
            {
                continue;
            }
            const auto receipt = m_renderPipeline.BuildMinimumResidentPresentationReceipt(
                status->sceneRuntimeId,
                status->rootEntity,
                status->renderableVisibilityVersions);
            if (receipt.has_value())
            {
                const bool confirmed =
                    m_modelCoordinator->ConfirmMinimumResidentPresentation(handle, *receipt);
                if (confirmed)
                {
                    ++result.confirmedModelPresentations;
                }
                else
                {
                    SetDiagnostic(
                        "Model presentation receipt was rejected by its exact Resource coordinator.");
                }
                succeeded = confirmed && succeeded;
            }
        }
    }
    if (m_environmentCoordinator != nullptr)
    {
        for (const ResourceSceneAdapters::EcsEnvironmentLoadHandle handle : m_environmentHandles)
        {
            const auto status = m_environmentCoordinator->GetStatus(handle);
            if (!status.has_value() || status->state !=
                                           ResourceSceneAdapters::EcsEnvironmentLoadState::
                                               PendingPresentation)
            {
                continue;
            }
            const auto receipt = m_renderPipeline.BuildEnvironmentPresentationReceipt(
                status->sceneRuntimeId,
                status->skyboxEntity,
                status->skyboxWriteVersion);
            if (receipt.has_value())
            {
                const bool confirmed = m_environmentCoordinator->ConfirmPresentation(handle, *receipt);
                if (confirmed)
                {
                    ++result.confirmedEnvironmentPresentations;
                }
                else
                {
                    SetDiagnostic(
                        "Environment presentation receipt was rejected by its exact Resource coordinator.");
                }
                succeeded = confirmed && succeeded;
            }
        }
    }
    return succeeded;
}

void WorldEcsRuntimeComposition::PruneInvalidTrackedHandles()
{
    if (m_modelCoordinator != nullptr)
    {
        EraseInvalidHandles(m_modelHandles, *m_modelCoordinator);
    }
    if (m_environmentCoordinator != nullptr)
    {
        EraseInvalidHandles(m_environmentHandles, *m_environmentCoordinator);
    }
}

void WorldEcsRuntimeComposition::CancelTrackedRequests() noexcept
{
    if (m_modelCoordinator != nullptr)
    {
        for (const ResourceSceneAdapters::EcsSceneAssetLoadHandle handle : m_modelHandles)
        {
            if (m_modelCoordinator->IsValid(handle))
            {
                static_cast<void>(m_modelCoordinator->Cancel(handle));
            }
        }
    }
    if (m_environmentCoordinator != nullptr)
    {
        for (const ResourceSceneAdapters::EcsEnvironmentLoadHandle handle : m_environmentHandles)
        {
            if (m_environmentCoordinator->IsValid(handle))
            {
                static_cast<void>(m_environmentCoordinator->Cancel(handle));
            }
        }
    }
    if (m_options.animationAssetService != nullptr &&
        m_options.animationAssetService->PrepareForSceneShutdown(
            m_runtime.GetSceneRuntimeId()))
    {
        m_animationAssetRequests.clear();
    }
}

bool WorldEcsRuntimeComposition::IsAlivePresentationCamera(ECS::EntityHandle entity) const noexcept
{
    if (!entity.IsValid())
    {
        return false;
    }

    const ECS::Registry& registry = m_runtime.GetRegistry();
    const SceneECS::EntityLifecycleState* lifecycle =
        registry.TryGet<SceneECS::EntityLifecycleState>(entity);
    return lifecycle != nullptr && lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive &&
           registry.TryGet<SceneECS::Camera>(entity) != nullptr;
}

void WorldEcsRuntimeComposition::ClearInvalidShutdownPresentationCamera() noexcept
{
    if (m_shutdownPresentationCamera.IsValid() &&
        !IsAlivePresentationCamera(m_shutdownPresentationCamera))
    {
        m_shutdownPresentationCamera = ECS::EntityHandle::Invalid();
        m_cameraDestroyRequested = false;
        SetDiagnostic(
            "Captured presentation camera was no longer an exact Alive Camera; shutdown will destroy all entities.");
    }
}

bool WorldEcsRuntimeComposition::RequestDestroyRemainingAlive()
{
    const SceneECS::DestroyAllRequestResult destroy = NeedsRenderPublicationDuringShutdown() ?
        m_runtime.RequestDestroyAllExcept(std::span(&m_shutdownPresentationCamera, 1u)) :
        m_runtime.RequestDestroyAll();
    return destroy.IsAccepted();
}

bool WorldEcsRuntimeComposition::IsShutdownDrained() noexcept
{
    const SceneECS::SceneEcsDiagnosticsSnapshot scene = m_runtime.GetDiagnosticsSnapshot();
    const bool genericDrained =
        m_genericRetirement != nullptr && m_genericRetirement->PrepareForShutdown();
    const bool physicsDrained = m_physicsBridge != nullptr &&
                                IsPhysicsDrained(m_physicsBridge->GetDiagnosticsSnapshot(),
                                                 m_physicsWorld.GetRuntimeDiagnosticsSnapshot());
    const bool animationDrained = m_animationBridge != nullptr &&
                                  IsAnimationDrained(m_animationBridge->GetDiagnosticsSnapshot());
    const bool audioDrained = m_audioBridge == nullptr ||
                              IsAudioDrained(m_audioBridge->GetDiagnosticsSnapshot());
    const bool scriptDrained = m_scriptBridge == nullptr ||
                               IsScriptDrained(m_scriptBridge->GetDiagnosticsSnapshot());
    const bool particleDrained = m_particleBridge == nullptr ||
                                 IsParticleDrained(m_particleBridge->GetDiagnosticsSnapshot());
    const bool waterDrained = m_waterBridge == nullptr ||
                              IsWaterDrained(m_waterBridge->GetDiagnosticsSnapshot());
    const bool terrainDrained = m_terrainBridge == nullptr ||
                                IsTerrainDrained(m_terrainBridge->GetDiagnosticsSnapshot());
    const bool resourceAnimationDrained =
        m_options.resourceAnimationEvaluator == nullptr ||
        m_options.resourceAnimationEvaluator
                ->GetSceneDiagnosticsSnapshot(m_runtime.GetSceneRuntimeId())
                .activePlaybackCount == 0;
    const auto animationAssetDiagnostics = m_options.animationAssetService != nullptr ?
        std::optional(m_options.animationAssetService->GetSceneDiagnosticsSnapshot(
            m_runtime.GetSceneRuntimeId())) :
        std::nullopt;
    const bool animationAssetRequestsDrained = !animationAssetDiagnostics.has_value() ||
        (animationAssetDiagnostics->activeRequestCount == 0 &&
         animationAssetDiagnostics->requestCount == 0);
    const bool sceneDrained = !m_cameraDestroyRequested && NeedsRenderPublicationDuringShutdown() ?
                                  IsSceneDrainedExceptPresentationCamera(scene, true) :
                                  IsSceneDrained(scene);
    if (!m_modelHandles.empty() || !m_environmentHandles.empty())
    {
        SetDiagnostic("Tracked resource request handles remain during ECS World shutdown drain.");
        return false;
    }
    if (!genericDrained)
    {
        SetDiagnostic("Generic Render retirement has not completed an empty owner-thread cleanup run.");
        return false;
    }
    if (!physicsDrained)
    {
        SetDiagnostic("Physics ECS bridge or BuiltIn PhysicsWorld remains active during shutdown drain.");
        return false;
    }
    if (!animationDrained)
    {
        SetDiagnostic("Animation ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!audioDrained)
    {
        SetDiagnostic("Audio ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!scriptDrained)
    {
        SetDiagnostic("Script ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!particleDrained)
    {
        SetDiagnostic("Particle ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!waterDrained)
    {
        SetDiagnostic("Water ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!terrainDrained)
    {
        SetDiagnostic("Terrain ECS bridge cleanup remains active during shutdown drain.");
        return false;
    }
    if (!resourceAnimationDrained)
    {
        SetDiagnostic("Resource animation evaluator retains active playback during shutdown drain.");
        return false;
    }
    if (!animationAssetRequestsDrained)
    {
        SetDiagnostic("Shared animation asset service retains active requests for this ECS World.");
        return false;
    }
    if (!sceneDrained)
    {
        SetDiagnostic("Scene ECS lifecycle cleanup remains active during shutdown drain.");
        return false;
    }
    return true;
}

bool WorldEcsRuntimeComposition::ReleaseRuntimeStateAfterDrain() noexcept
{
    // Each owner has already demonstrated a zero-outstanding diagnostic. Remove only this
    // composition's processor scope, preserving caller-owned Scene processors registered before
    // or after composition startup.
    if (!RemoveCompositionProcessorScope())
    {
        SetDiagnostic(
            "ECS World shutdown drain could not atomically remove its Scene processor scope.");
        return false;
    }

    m_modelCoordinator.reset();
    m_environmentCoordinator.reset();
    m_genericRetirement.reset();
    m_terrainBridge.reset();
    m_waterBridge.reset();
    m_particleBridge.reset();
    m_scriptBridge.reset();
    m_audioBridge.reset();
    if (m_options.resourceAnimationEvaluator != nullptr)
    {
        static_cast<void>(m_options.resourceAnimationEvaluator->RemoveScene(
            m_runtime.GetSceneRuntimeId()));
    }
    m_animationBridge.reset();
    m_physicsBridge.reset();
    m_physicsWorld.Shutdown();
    m_modelHandles.clear();
    m_environmentHandles.clear();
    m_animationAssetRequests.clear();
    m_processorsCleared = true;
    m_shutdownComplete = true;
    m_initialized = false;
    return true;
}

bool WorldEcsRuntimeComposition::RemoveCompositionProcessorScope() noexcept
{
    if (!m_processorRegistrationScope.has_value())
    {
        return true;
    }
    if (!m_runtime.RemoveProcessorRegistrationScope(*m_processorRegistrationScope))
    {
        return false;
    }
    m_processorRegistrationScope.reset();
    return true;
}

void WorldEcsRuntimeComposition::SetDiagnostic(std::string diagnostic) noexcept
{
    try
    {
        m_lastDiagnostic = std::move(diagnostic);
    }
    catch (...)
    {
        m_lastDiagnostic.clear();
    }
}
} // namespace RVX
