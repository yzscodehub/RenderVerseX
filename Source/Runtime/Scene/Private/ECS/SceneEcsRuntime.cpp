#include "Scene/ECS/SceneEcsRuntime.h"

#include "ECS/Query.h"
#include "Scene/ECS/AnimationFragments.h"
#include "Scene/ECS/AudioFragments.h"
#include "Scene/ECS/PhysicsFragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/ScriptFragments.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RVX::SceneECS
{
namespace
{
    void SortAndUnique(std::vector<ECS::EntityHandle>& entities)
    {
        std::sort(entities.begin(), entities.end());
        entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    }

    bool ContainsHandle(const std::vector<ECS::EntityHandle>& entities,
                        ECS::EntityHandle entity)
    {
        return std::find(entities.begin(), entities.end(), entity) != entities.end();
    }

    bool IsStepModeValidForPhase(ECS::ProcessorPhase phase,
                                 ECS::ProcessorStepMode stepMode)
    {
        const uint8 phaseValue = static_cast<uint8>(phase);
        const bool isFixedPhase =
            phaseValue >= static_cast<uint8>(ECS::ProcessorPhase::BeforeFixedStep) &&
            phaseValue <= static_cast<uint8>(ECS::ProcessorPhase::EndFixedStep);
        return stepMode == (isFixedPhase ? ECS::ProcessorStepMode::Fixed :
                                          ECS::ProcessorStepMode::Variable);
    }
} // namespace

SceneEcsRuntime::SceneEcsRuntime(uint32 cleanupRecordCapacity,
                                 uint32 structuralJournalCapacity)
    : m_registry(structuralJournalCapacity)
    , m_transformHierarchy(m_registry)
    , m_spatialIndex(m_registry)
    , m_commandSubmissionState(
          std::make_shared<Detail::SceneCommandSubmissionState>(m_registry.GetSceneRuntimeId()))
    , m_featureSnapshotStore(m_registry.GetSceneRuntimeId())
    , m_skinningSnapshotStore(m_registry.GetSceneRuntimeId())
    , m_cleanupRecordCapacity(std::max(cleanupRecordCapacity, 1u))
{
}

SceneEcsRuntime::~SceneEcsRuntime()
{
    DiscardQueuedCommandBuffers();
}

uint32 SceneEcsRuntime::ToBarrierIndex(SceneCommandBarrier barrier)
{
    const uint32 index = static_cast<uint32>(barrier);
    return index < RVX_SCENE_COMMAND_BARRIER_COUNT ? index : RVX_SCENE_COMMAND_BARRIER_COUNT;
}

uint32 SceneEcsRuntime::ToProcessorPhaseIndex(ECS::ProcessorPhase phase)
{
    const uint32 index = static_cast<uint32>(phase);
    return index < ProcessorPhaseCount ? index : ProcessorPhaseCount;
}

bool SceneEcsRuntime::IsOwnerThread() const
{
    return m_registry.IsOwnerThread();
}

CleanupDomainMask SceneEcsRuntime::InferRequiredCleanupDomains(
    ECS::EntityHandle entity,
    CleanupDomainMask requestedDomains) const
{
    CleanupDomainMask domains =
        requestedDomains & ToCleanupDomainMask(CleanupDomain::All);

    if (m_registry.TryGet<RigidBody>(entity) != nullptr ||
        m_registry.TryGet<Collider>(entity) != nullptr ||
        m_registry.TryGet<PhysicsBodyState>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::Physics);
    }

    if (m_registry.TryGet<AnimationSkeletonBinding>(entity) != nullptr ||
        m_registry.TryGet<SkinnedMeshBinding>(entity) != nullptr ||
        m_registry.TryGet<Animator>(entity) != nullptr ||
        m_registry.TryGet<AnimationPoseState>(entity) != nullptr ||
        m_registry.TryGet<RootMotionIntent>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::Animation);
    }

    if (m_registry.TryGet<AudioEmitter>(entity) != nullptr ||
        m_registry.TryGet<AudioPlaybackIntent>(entity) != nullptr ||
        m_registry.TryGet<AudioPlaybackState>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::Audio);
    }

    if (m_registry.TryGet<ScriptBehaviour>(entity) != nullptr ||
        m_registry.TryGet<ScriptExecutionIntent>(entity) != nullptr ||
        m_registry.TryGet<ScriptExecutionState>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::Script);
    }

    // Every feature side table is independently acknowledged.  Render still
    // consumes the frozen value independently, so each feature tag also adds
    // Render retirement before slot reuse.
    if (m_registry.TryGet<ParticleRuntimeTag>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::ParticleFeature) |
                   ToCleanupDomainMask(CleanupDomain::Render);
    }
    if (m_registry.TryGet<WaterRuntimeTag>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::WaterFeature) |
                   ToCleanupDomainMask(CleanupDomain::Render);
    }
    if (m_registry.TryGet<TerrainRuntimeTag>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::TerrainFeature) |
                   ToCleanupDomainMask(CleanupDomain::Render);
    }

    if (m_registry.TryGet<Mesh>(entity) != nullptr ||
        m_registry.TryGet<Light>(entity) != nullptr ||
        m_registry.TryGet<Skybox>(entity) != nullptr)
    {
        domains |= ToCleanupDomainMask(CleanupDomain::Render);
    }

    return domains;
}

bool SceneCommandSubmissionPort::IsOpen() const
{
    if (m_state == nullptr)
    {
        return false;
    }

    std::lock_guard lock(m_state->mutex);
    return !m_state->closed;
}

SceneCommandBuffer SceneCommandSubmissionPort::CreateCommandBuffer() const
{
    if (m_state == nullptr)
    {
        return {};
    }

    std::lock_guard lock(m_state->mutex);
    return !m_state->closed ?
               SceneCommandBuffer(m_state->runtimeId, std::this_thread::get_id()) :
               SceneCommandBuffer();
}

SceneCommandBufferReceipt SceneCommandSubmissionPort::SubmitCommandBuffer(
    SceneCommandBuffer&& commandBuffer,
    SceneCommandBarrier barrier) const
{
    const auto receipt = std::make_shared<Detail::SceneCommandBufferReceiptState>();
    const uint32 barrierIndex = SceneEcsRuntime::ToBarrierIndex(barrier);
    if (m_state == nullptr || commandBuffer.IsSubmitted() ||
        barrierIndex >= RVX_SCENE_COMMAND_BARRIER_COUNT)
    {
        commandBuffer.RejectOutstanding(ECS::CommandError::InvalidTarget);
        commandBuffer.m_submitted = true;
        receipt->SetStatus(SceneCommandBufferStatus::Rejected);
        return SceneCommandBufferReceipt(receipt);
    }

    std::shared_ptr<SceneCommandBuffer> queuedCommandBuffer;
    try
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->closed || !commandBuffer.IsBoundTo(m_state->runtimeId))
        {
            commandBuffer.RejectOutstanding(ECS::CommandError::InvalidTarget);
            commandBuffer.m_submitted = true;
            receipt->SetStatus(SceneCommandBufferStatus::Rejected);
            return SceneCommandBufferReceipt(receipt);
        }

        queuedCommandBuffer = std::make_shared<SceneCommandBuffer>(std::move(commandBuffer));
        const uint64 submissionSequence = m_state->diagnostics.nextSubmissionSequence;
        m_state->commandBuffers[barrierIndex].push_back({
            .commandBuffer = queuedCommandBuffer,
            .receipt = receipt,
            .submissionSequence = submissionSequence,
        });
        receipt->SetQueued(barrier, submissionSequence);
        ++m_state->diagnostics.nextSubmissionSequence;
        ++m_state->diagnostics.barriers[barrierIndex].submittedBufferCount;
        ++m_state->diagnostics.barriers[barrierIndex].queuedBufferCount;
    }
    catch (...)
    {
        if (queuedCommandBuffer != nullptr)
        {
            queuedCommandBuffer->RejectOutstanding(ECS::CommandError::OperationRejected);
        }
        else
        {
            commandBuffer.RejectOutstanding(ECS::CommandError::OperationRejected);
            commandBuffer.m_submitted = true;
        }
        receipt->SetStatus(SceneCommandBufferStatus::Rejected);
        return SceneCommandBufferReceipt(receipt);
    }

    return SceneCommandBufferReceipt(receipt);
}

SceneCommandSubmissionPort SceneEcsRuntime::AcquireCommandSubmissionPort() const
{
    return SceneCommandSubmissionPort(m_commandSubmissionState);
}

void SceneEcsRuntime::BeginShutdown() noexcept
{
    DiscardQueuedCommandBuffers();
}

bool SceneEcsRuntime::IsAcceptingCommandSubmissions() const
{
    return AcquireCommandSubmissionPort().IsOpen();
}

SceneCommandBuffer SceneEcsRuntime::CreateCommandBuffer()
{
    return AcquireCommandSubmissionPort().CreateCommandBuffer();
}

SceneCommandBufferReceipt SceneEcsRuntime::SubmitCommandBuffer(
    SceneCommandBuffer&& commandBuffer,
    SceneCommandBarrier barrier)
{
    return AcquireCommandSubmissionPort().SubmitCommandBuffer(std::move(commandBuffer), barrier);
}

SceneCommandDiagnosticsSnapshot SceneEcsRuntime::GetCommandDiagnosticsSnapshot() const
{
    if (m_commandSubmissionState == nullptr)
    {
        return {};
    }

    std::lock_guard lock(m_commandSubmissionState->mutex);
    return m_commandSubmissionState->diagnostics;
}

bool SceneEcsRuntime::RegisterProcessor(ECS::ProcessorDescriptor descriptor)
{
    if (descriptor.phase == ECS::ProcessorPhase::RenderExtraction)
    {
        return false;
    }
    return RegisterProcessorInternal(std::move(descriptor));
}

bool SceneEcsRuntime::RegisterProcessors(
    std::vector<ECS::ProcessorDescriptor> descriptors)
{
    if (!IsOwnerThread() || descriptors.empty())
    {
        return false;
    }

    try
    {
        std::unordered_map<std::string, uint8> names;
        names.reserve(m_registeredProcessors.size() + descriptors.size());
        for (const RegisteredProcessor& existing : m_registeredProcessors)
        {
            names.emplace(existing.descriptor.name, 1u);
        }

        for (const ECS::ProcessorDescriptor& descriptor : descriptors)
        {
            if (descriptor.name.empty() ||
                (!descriptor.run && !descriptor.runWithContext) ||
                descriptor.phase == ECS::ProcessorPhase::RenderExtraction ||
                ToProcessorPhaseIndex(descriptor.phase) >= ProcessorPhaseCount ||
                !IsStepModeValidForPhase(descriptor.phase, descriptor.stepMode) ||
                !names.emplace(descriptor.name, 1u).second)
            {
                return false;
            }
        }

        const size_t originalSize = m_registeredProcessors.size();
        m_registeredProcessors.reserve(originalSize + descriptors.size());
        try
        {
            const uint64 registrationScopeId =
                m_activeProcessorRegistrationScope.value_or(0);
            for (ECS::ProcessorDescriptor& descriptor : descriptors)
            {
                m_registeredProcessors.push_back({
                    .descriptor = std::move(descriptor),
                    .registrationScopeId = registrationScopeId,
                });
            }
        }
        catch (...)
        {
            m_registeredProcessors.resize(originalSize);
            return false;
        }

        m_processorsNeedCompile = true;
        m_processorsCompiled = false;
        m_processorCompileError.clear();
        m_processorConflicts.clear();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::optional<ProcessorRegistrationScope>
SceneEcsRuntime::BeginProcessorRegistrationScope()
{
    if (!IsOwnerThread() || m_activeProcessorRegistrationScope.has_value() ||
        m_nextProcessorRegistrationScopeId == 0)
    {
        return std::nullopt;
    }

    const uint64 scopeId = m_nextProcessorRegistrationScopeId++;
    try
    {
        if (!m_processorRegistrationScopes.insert(scopeId).second)
        {
            return std::nullopt;
        }
    }
    catch (...)
    {
        return std::nullopt;
    }

    m_activeProcessorRegistrationScope = scopeId;
    return ProcessorRegistrationScope(GetSceneRuntimeId(), scopeId);
}

bool SceneEcsRuntime::CloseProcessorRegistrationScope(
    const ProcessorRegistrationScope& scope)
{
    if (!IsOwnerThread() || !scope.IsValid() ||
        scope.m_sceneRuntimeId != GetSceneRuntimeId() ||
        !m_activeProcessorRegistrationScope.has_value() ||
        *m_activeProcessorRegistrationScope != scope.m_scopeId ||
        !m_processorRegistrationScopes.contains(scope.m_scopeId) ||
        m_processorsNeedCompile || !m_processorsCompiled)
    {
        return false;
    }

    m_activeProcessorRegistrationScope.reset();
    return true;
}

bool SceneEcsRuntime::RemoveProcessorRegistrationScope(
    ProcessorRegistrationScope& scope)
{
    if (!IsOwnerThread() || !scope.IsValid() ||
        scope.m_sceneRuntimeId != GetSceneRuntimeId() ||
        !m_processorRegistrationScopes.contains(scope.m_scopeId))
    {
        return false;
    }

    try
    {
        std::vector<RegisteredProcessor> retained;
        retained.reserve(m_registeredProcessors.size());
        for (const RegisteredProcessor& registered : m_registeredProcessors)
        {
            if (registered.registrationScopeId != scope.m_scopeId)
            {
                retained.push_back(registered);
            }
        }

        std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> variableSchedulers;
        std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> fixedSchedulers;
        std::array<uint32, ProcessorPhaseCount> variableSchedulerSizes{};
        std::array<uint32, ProcessorPhaseCount> fixedSchedulerSizes{};
        std::vector<ECS::ProcessorConflict> processorConflicts;
        std::string compileError;
        if (!BuildProcessorSchedulers(retained,
                                      variableSchedulers,
                                      fixedSchedulers,
                                      variableSchedulerSizes,
                                      fixedSchedulerSizes,
                                      processorConflicts,
                                      compileError))
        {
            m_processorCompileError = std::move(compileError);
            return false;
        }

        m_registeredProcessors = std::move(retained);
        m_variableSchedulers = std::move(variableSchedulers);
        m_fixedSchedulers = std::move(fixedSchedulers);
        m_variableSchedulerSizes = variableSchedulerSizes;
        m_fixedSchedulerSizes = fixedSchedulerSizes;
        m_processorConflicts = std::move(processorConflicts);
        m_processorCompileError.clear();
        m_processorsNeedCompile = false;
        m_processorsCompiled = true;
        if (m_activeProcessorRegistrationScope.has_value() &&
            *m_activeProcessorRegistrationScope == scope.m_scopeId)
        {
            m_activeProcessorRegistrationScope.reset();
        }
        m_processorRegistrationScopes.erase(scope.m_scopeId);
        scope.m_sceneRuntimeId = {};
        scope.m_scopeId = 0;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SceneEcsRuntime::RegisterRenderExtractionProcessor(
    std::string name,
    uint32 order,
    std::function<void(const FrozenSceneSnapshot&)> run)
{
    if (!run)
    {
        return false;
    }

    ECS::ProcessorDescriptor descriptor;
    descriptor.name = std::move(name);
    descriptor.phase = ECS::ProcessorPhase::RenderExtraction;
    descriptor.order = order;
    descriptor.stepMode = ECS::ProcessorStepMode::Variable;
    descriptor.run = [this, run = std::move(run)](ECS::Registry&)
    {
        if (m_latestFrozenSnapshot != nullptr)
        {
            run(*m_latestFrozenSnapshot);
        }
    };
    return RegisterProcessorInternal(std::move(descriptor));
}

bool SceneEcsRuntime::RegisterProcessorInternal(ECS::ProcessorDescriptor descriptor)
{
    if (!IsOwnerThread() || descriptor.name.empty() ||
        (!descriptor.run && !descriptor.runWithContext) ||
        ToProcessorPhaseIndex(descriptor.phase) >= ProcessorPhaseCount ||
        !IsStepModeValidForPhase(descriptor.phase, descriptor.stepMode))
    {
        return false;
    }

    const bool duplicateName = std::any_of(
        m_registeredProcessors.begin(),
        m_registeredProcessors.end(),
        [&descriptor](const RegisteredProcessor& existing)
        {
            return existing.descriptor.name == descriptor.name;
        });
    if (duplicateName)
    {
        return false;
    }

    try
    {
        m_registeredProcessors.push_back({
            .descriptor = std::move(descriptor),
            .registrationScopeId = m_activeProcessorRegistrationScope.value_or(0),
        });
        m_processorsNeedCompile = true;
        m_processorsCompiled = false;
        m_processorCompileError.clear();
        m_processorConflicts.clear();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void SceneEcsRuntime::ClearProcessors()
{
    if (!IsOwnerThread())
    {
        return;
    }

    m_registeredProcessors.clear();
    m_processorRegistrationScopes.clear();
    m_activeProcessorRegistrationScope.reset();
    for (ECS::ProcessorScheduler& scheduler : m_variableSchedulers)
    {
        scheduler.Clear();
    }
    for (ECS::ProcessorScheduler& scheduler : m_fixedSchedulers)
    {
        scheduler.Clear();
    }
    m_variableSchedulerSizes.fill(0);
    m_fixedSchedulerSizes.fill(0);
    m_processorConflicts.clear();
    m_processorCompileError.clear();
    m_processorsNeedCompile = false;
    m_processorsCompiled = true;
}

bool SceneEcsRuntime::CompileProcessors()
{
    if (!IsOwnerThread())
    {
        return false;
    }
    if (!m_processorsNeedCompile)
    {
        return m_processorsCompiled;
    }
    return ValidateAndBuildProcessorSchedulers();
}

SceneEcsTickResult SceneEcsRuntime::Tick(const SceneEcsTickRequest& request)
{
    SceneEcsTickResult result;
    ResetCommandBarrierExecution(result);
    if (!IsOwnerThread() || !std::isfinite(request.variableDeltaSeconds) ||
        request.variableDeltaSeconds < 0.0 ||
        (request.fixedStepCount != 0 &&
         (!std::isfinite(request.fixedDeltaSeconds) ||
          request.fixedDeltaSeconds <= 0.0)) ||
        !CompileProcessors())
    {
        return result;
    }

    const auto runPhase = [this, &result](ECS::ProcessorPhase phase,
                                          ECS::ProcessorStepMode stepMode,
                                          float64 deltaSeconds,
                                          uint64 frameSequence,
                                          uint64 fixedStepSequence)
    {
        return RunProcessorPhase(phase,
                                 stepMode,
                                 deltaSeconds,
                                 frameSequence,
                                 fixedStepSequence,
                                 result.processorFailure);
    };

    result.frameSequence = ++m_frameSequence;

    // BeginSimulation playback deliberately runs before any frame processor.
    result.commandBarriers[ToBarrierIndex(SceneCommandBarrier::BeginSimulation)] =
        ExecuteCommandBarrier(SceneCommandBarrier::BeginSimulation);
    result.beginSimulationTransforms = m_transformHierarchy.BeginSimulationFrame();
    if (!runPhase(ECS::ProcessorPhase::BeginSimulation,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0) ||
        !runPhase(ECS::ProcessorPhase::Gameplay,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    for (uint32 step = 0; step < request.fixedStepCount; ++step)
    {
        const uint64 fixedStepSequence = ++m_fixedStepSequence;
        result.lastFixedStepSequence = fixedStepSequence;
        ++result.fixedStepsExecuted;

        const SceneCommandBarrierExecution beforeFixedExecution =
            ExecuteCommandBarrier(SceneCommandBarrier::BeforeFixedStep);
        SceneCommandBarrierExecution& beforeFixedAggregate =
            result.commandBarriers[ToBarrierIndex(SceneCommandBarrier::BeforeFixedStep)];
        beforeFixedAggregate.attemptedBufferCount += beforeFixedExecution.attemptedBufferCount;
        beforeFixedAggregate.appliedBufferCount += beforeFixedExecution.appliedBufferCount;
        beforeFixedAggregate.rejectedBufferCount += beforeFixedExecution.rejectedBufferCount;
        if (!runPhase(ECS::ProcessorPhase::BeforeFixedStep,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence) ||
            !runPhase(ECS::ProcessorPhase::FixedAnimation,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence) ||
            !runPhase(ECS::ProcessorPhase::RootMotion,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence))
        {
            return result;
        }

        // Root motion and kinematic intent are expressed in Scene fragments.
        // Resolve them before handing transforms to physics so fixed consumers
        // never see the previous substep's world pose.
        result.prePhysicsFixedTransforms = m_transformHierarchy.ResolveSimulationTransforms();
        if (!runPhase(ECS::ProcessorPhase::SceneToPhysics,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence) ||
            !runPhase(ECS::ProcessorPhase::PhysicsSimulation,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence) ||
            !runPhase(ECS::ProcessorPhase::PhysicsToScene,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence))
        {
            return result;
        }

        // Transform resolve belongs to this fixed phase, after physics writes and
        // before processors that consume the resolved fixed simulation state.
        result.fixedTransforms = m_transformHierarchy.ResolveSimulationTransforms();
        if (!runPhase(ECS::ProcessorPhase::FixedTransformResolve,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence) ||
            !runPhase(ECS::ProcessorPhase::EndFixedStep,
                      ECS::ProcessorStepMode::Fixed,
                      request.fixedDeltaSeconds,
                      m_frameSequence,
                      fixedStepSequence))
        {
            return result;
        }

        // EndFixedStep is an end-of-substep barrier, so work recorded by fixed
        // processors is visible to the following substep or variable phases.
        const SceneCommandBarrierExecution endFixedExecution =
            ExecuteCommandBarrier(SceneCommandBarrier::EndFixedStep);
        SceneCommandBarrierExecution& endFixedAggregate =
            result.commandBarriers[ToBarrierIndex(SceneCommandBarrier::EndFixedStep)];
        endFixedAggregate.attemptedBufferCount += endFixedExecution.attemptedBufferCount;
        endFixedAggregate.appliedBufferCount += endFixedExecution.appliedBufferCount;
        endFixedAggregate.rejectedBufferCount += endFixedExecution.rejectedBufferCount;
    }

    if (!runPhase(ECS::ProcessorPhase::PostSimulation,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    result.commandBarriers[ToBarrierIndex(SceneCommandBarrier::PrePresentation)] =
        ExecuteCommandBarrier(SceneCommandBarrier::PrePresentation);
    if (!runPhase(ECS::ProcessorPhase::PrePresentation,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0) ||
        !runPhase(ECS::ProcessorPhase::Transform,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    // Variable Transform processors may change local transforms without a fixed
    // substep, so resolve immediately before all presentation-facing consumers.
    result.presentationTransforms = m_transformHierarchy.ResolveSimulationTransforms();
    result.synchronizedRenderTransformCount = m_transformHierarchy.SynchronizeRenderWorldTransforms();
    if (!runPhase(ECS::ProcessorPhase::Bounds,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0) ||
        !runPhase(ECS::ProcessorPhase::Spatial,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    result.spatial = m_spatialIndex.Synchronize();
    if (!runPhase(ECS::ProcessorPhase::Feature,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    // Extraction processors consume only the frozen value snapshot, never live
    // Registry fragments. The renderer integration is intentionally deferred.
    const std::shared_ptr<const FrozenSceneSnapshot> snapshot = FreezeAtPresentationBoundary();
    result.sceneSnapshotRevision = snapshot != nullptr ? snapshot->revision : 0;
    if (snapshot == nullptr)
    {
        return result;
    }
    if (!runPhase(ECS::ProcessorPhase::RenderExtraction,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    // Cleanup consumers need a published record before their EndFrameCleanup
    // processors run. They can acknowledge in this same frame; advancement and
    // slot recycling remain deliberately after the processor barrier.
    result.publishedCleanupRecordCount = PublishPendingDestroyCleanup();
    if (!runPhase(ECS::ProcessorPhase::EndFrameCleanup,
                  ECS::ProcessorStepMode::Variable,
                  request.variableDeltaSeconds,
                  m_frameSequence,
                  0))
    {
        return result;
    }

    result.advancedRetirementCount = AdvanceRetirements();
    result.recycledEntityCount = RecycleRecyclableEntities();
    result.succeeded = true;
    return result;
}

std::shared_ptr<const FrozenSceneSnapshot> SceneEcsRuntime::FreezeAtPresentationBoundary()
{
    if (!IsOwnerThread())
    {
        return {};
    }
    try
    {
        FrozenSceneSnapshot frozen = m_sceneSnapshotBuilder.Build(m_registry);
        if (!m_featureSnapshotStore.FreezeInto(frozen) ||
            !m_skinningSnapshotStore.FreezeInto(frozen))
        {
            return {};
        }
        std::shared_ptr<const FrozenSceneSnapshot> published =
            std::make_shared<const FrozenSceneSnapshot>(std::move(frozen));
        m_latestFrozenSnapshot = published;
        return published;
    }
    catch (...)
    {
        return {};
    }
}

bool SceneEcsRuntime::PublishParticleFeatureSnapshot(
    ECS::EntityHandle entity,
    ParticleRenderSnapshotItem state,
    uint64 payloadRevision)
{
    return IsOwnerThread() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::Feature &&
           m_featureSnapshotStore.PublishParticle(
               m_registry.GetRef(entity), std::move(state), payloadRevision);
}

bool SceneEcsRuntime::PublishWaterFeatureSnapshot(
    ECS::EntityHandle entity,
    WaterRenderSnapshotItem state,
    uint64 payloadRevision)
{
    return IsOwnerThread() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::Feature &&
           m_featureSnapshotStore.PublishWater(
               m_registry.GetRef(entity), std::move(state), payloadRevision);
}

bool SceneEcsRuntime::PublishTerrainFeatureSnapshot(
    ECS::EntityHandle entity,
    TerrainRenderSnapshotItem state,
    uint64 payloadRevision)
{
    return IsOwnerThread() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::Feature &&
           m_featureSnapshotStore.PublishTerrain(
               m_registry.GetRef(entity), std::move(state), payloadRevision);
}

bool SceneEcsRuntime::RemoveFeatureSnapshots(ECS::EntityHandle entity)
{
    return IsOwnerThread() && entity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::EndFrameCleanup &&
           m_featureSnapshotStore.Remove(
               SceneFeatureSnapshotSource{m_registry.GetSceneRuntimeId(), entity});
}

bool SceneEcsRuntime::RemoveParticleFeatureSnapshot(ECS::EntityHandle entity)
{
    return IsOwnerThread() && entity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::EndFrameCleanup &&
           m_featureSnapshotStore.RemoveParticle(
               SceneFeatureSnapshotSource{m_registry.GetSceneRuntimeId(), entity});
}

bool SceneEcsRuntime::RemoveWaterFeatureSnapshot(ECS::EntityHandle entity)
{
    return IsOwnerThread() && entity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::EndFrameCleanup &&
           m_featureSnapshotStore.RemoveWater(
               SceneFeatureSnapshotSource{m_registry.GetSceneRuntimeId(), entity});
}

bool SceneEcsRuntime::RemoveTerrainFeatureSnapshot(ECS::EntityHandle entity)
{
    return IsOwnerThread() && entity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::EndFrameCleanup &&
           m_featureSnapshotStore.RemoveTerrain(
               SceneFeatureSnapshotSource{m_registry.GetSceneRuntimeId(), entity});
}

bool SceneEcsRuntime::PublishSkinningPaletteSnapshot(
    ECS::EntityHandle meshEntity,
    SceneSkinningPaletteSnapshot snapshot)
{
    return IsOwnerThread() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::Feature &&
           m_skinningSnapshotStore.Publish(m_registry.GetRef(meshEntity), std::move(snapshot));
}

bool SceneEcsRuntime::RemoveSkinningPaletteSnapshots(ECS::EntityHandle entity)
{
    return IsOwnerThread() && entity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::EndFrameCleanup &&
           m_skinningSnapshotStore.Remove(entity);
}

bool SceneEcsRuntime::InvalidateSkinningPaletteSnapshot(ECS::EntityHandle meshEntity)
{
    return IsOwnerThread() && meshEntity.IsValid() &&
           m_executingProcessorPhase == ECS::ProcessorPhase::Feature &&
           m_skinningSnapshotStore.Invalidate(meshEntity);
}

SceneCommandBarrierExecution SceneEcsRuntime::ExecuteCommandBarrier(SceneCommandBarrier barrier)
{
    SceneCommandBarrierExecution execution;
    execution.barrier = barrier;
    const uint32 barrierIndex = ToBarrierIndex(barrier);
    if (!IsOwnerThread() || barrierIndex >= RVX_SCENE_COMMAND_BARRIER_COUNT)
    {
        return execution;
    }

    // Detach a stable, sequence-ordered barrier batch before invoking
    // user-recorded operations. Producers may then append while this batch
    // plays, but those buffers stay in the live queue and therefore wait for
    // this barrier's next eligible playback.
    std::vector<Detail::SceneCommandSubmissionState::QueuedCommandBuffer> toPlay;
    {
        std::lock_guard lock(m_commandSubmissionState->mutex);
        if (m_commandSubmissionState->closed)
        {
            return execution;
        }
        std::vector<Detail::SceneCommandSubmissionState::QueuedCommandBuffer>& queued =
            m_commandSubmissionState->commandBuffers[barrierIndex];
        toPlay.swap(queued);
        m_commandSubmissionState->diagnostics.barriers[barrierIndex].queuedBufferCount = 0;
    }

    std::sort(toPlay.begin(),
              toPlay.end(),
              [](const Detail::SceneCommandSubmissionState::QueuedCommandBuffer& left,
                 const Detail::SceneCommandSubmissionState::QueuedCommandBuffer& right)
              {
                  return left.submissionSequence < right.submissionSequence;
              });

    for (Detail::SceneCommandSubmissionState::QueuedCommandBuffer& queued : toPlay)
    {
        ++execution.attemptedBufferCount;
        bool applied = false;
        try
        {
            applied = queued.commandBuffer != nullptr && queued.commandBuffer->Commit(*this);
        }
        catch (...)
        {
            applied = false;
        }

        if (applied)
        {
            queued.receipt->SetStatus(SceneCommandBufferStatus::Applied);
            ++execution.appliedBufferCount;
        }
        else
        {
            queued.receipt->SetStatus(SceneCommandBufferStatus::Rejected);
            ++execution.rejectedBufferCount;
        }
    }

    if (execution.appliedBufferCount != 0 || execution.rejectedBufferCount != 0)
    {
        std::lock_guard lock(m_commandSubmissionState->mutex);
        m_commandSubmissionState->diagnostics.barriers[barrierIndex].appliedBufferCount +=
            execution.appliedBufferCount;
        m_commandSubmissionState->diagnostics.barriers[barrierIndex].rejectedBufferCount +=
            execution.rejectedBufferCount;
    }
    return execution;
}

void SceneEcsRuntime::DiscardQueuedCommandBuffers()
{
    if (m_commandSubmissionState == nullptr)
    {
        return;
    }

    std::lock_guard lock(m_commandSubmissionState->mutex);
    m_commandSubmissionState->closed = true;
    for (uint32 barrierIndex = 0; barrierIndex < RVX_SCENE_COMMAND_BARRIER_COUNT; ++barrierIndex)
    {
        std::vector<Detail::SceneCommandSubmissionState::QueuedCommandBuffer>& queued =
            m_commandSubmissionState->commandBuffers[barrierIndex];
        for (Detail::SceneCommandSubmissionState::QueuedCommandBuffer& entry : queued)
        {
            if (entry.commandBuffer != nullptr)
            {
                entry.commandBuffer->DiscardOutstanding();
            }
            if (entry.receipt != nullptr &&
                entry.receipt->GetStatus() == SceneCommandBufferStatus::Queued)
            {
                entry.receipt->SetStatus(SceneCommandBufferStatus::Discarded);
                ++m_commandSubmissionState->diagnostics.barriers[barrierIndex].discardedBufferCount;
            }
        }
        queued.clear();
        m_commandSubmissionState->diagnostics.barriers[barrierIndex].queuedBufferCount = 0;
    }
}

bool SceneEcsRuntime::RunProcessorPhase(ECS::ProcessorPhase phase,
                                        ECS::ProcessorStepMode stepMode,
                                        float64 deltaSeconds,
                                        uint64 frameSequence,
                                        uint64 fixedStepSequence,
                                        std::optional<ECS::ProcessorExecutionFailure>& outFailure)
{
    const uint32 phaseIndex = ToProcessorPhaseIndex(phase);
    if (!IsOwnerThread() || phaseIndex >= ProcessorPhaseCount || !m_processorsCompiled)
    {
        return false;
    }

    struct ExecutingPhaseReset
    {
        std::optional<ECS::ProcessorPhase>& phase;
        ~ExecutingPhaseReset() { phase.reset(); }
    } reset{m_executingProcessorPhase};
    m_executingProcessorPhase = phase;

    if (stepMode == ECS::ProcessorStepMode::Variable)
    {
        if (m_variableSchedulerSizes[phaseIndex] == 0)
        {
            return true;
        }
        ECS::Registry::StructuralMutationGuard guard =
            m_registry.AcquireStructuralMutationGuard();
        ECS::ProcessorScheduler& scheduler = m_variableSchedulers[phaseIndex];
        const bool succeeded = scheduler.RunVariable(m_registry, deltaSeconds, frameSequence);
        if (!succeeded && scheduler.GetLastExecutionFailure().has_value())
        {
            outFailure = scheduler.GetLastExecutionFailure();
        }
        return succeeded;
    }
    if (m_fixedSchedulerSizes[phaseIndex] == 0)
    {
        return true;
    }
    ECS::Registry::StructuralMutationGuard guard =
        m_registry.AcquireStructuralMutationGuard();
    ECS::ProcessorScheduler& scheduler = m_fixedSchedulers[phaseIndex];
    const bool succeeded = scheduler.RunFixed(m_registry, deltaSeconds, fixedStepSequence);
    if (!succeeded && scheduler.GetLastExecutionFailure().has_value())
    {
        outFailure = scheduler.GetLastExecutionFailure();
    }
    return succeeded;
}

bool SceneEcsRuntime::ValidateAndBuildProcessorSchedulers()
{
    std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> variableSchedulers;
    std::array<ECS::ProcessorScheduler, ProcessorPhaseCount> fixedSchedulers;
    std::array<uint32, ProcessorPhaseCount> variableSchedulerSizes{};
    std::array<uint32, ProcessorPhaseCount> fixedSchedulerSizes{};
    std::vector<ECS::ProcessorConflict> processorConflicts;
    std::string compileError;
    if (!BuildProcessorSchedulers(m_registeredProcessors,
                                  variableSchedulers,
                                  fixedSchedulers,
                                  variableSchedulerSizes,
                                  fixedSchedulerSizes,
                                  processorConflicts,
                                  compileError))
    {
        m_processorCompileError = std::move(compileError);
        m_processorsNeedCompile = false;
        m_processorsCompiled = false;
        return false;
    }

    m_variableSchedulers = std::move(variableSchedulers);
    m_fixedSchedulers = std::move(fixedSchedulers);
    m_variableSchedulerSizes = variableSchedulerSizes;
    m_fixedSchedulerSizes = fixedSchedulerSizes;
    m_processorConflicts = std::move(processorConflicts);
    m_processorCompileError.clear();
    m_processorsNeedCompile = false;
    m_processorsCompiled = true;
    return true;
}

bool SceneEcsRuntime::BuildProcessorSchedulers(
    const std::vector<RegisteredProcessor>& processors,
    std::array<ECS::ProcessorScheduler, ProcessorPhaseCount>& outVariableSchedulers,
    std::array<ECS::ProcessorScheduler, ProcessorPhaseCount>& outFixedSchedulers,
    std::array<uint32, ProcessorPhaseCount>& outVariableSchedulerSizes,
    std::array<uint32, ProcessorPhaseCount>& outFixedSchedulerSizes,
    std::vector<ECS::ProcessorConflict>& outConflicts,
    std::string& outError) const
{
    outError.clear();
    outConflicts.clear();
    outVariableSchedulerSizes.fill(0);
    outFixedSchedulerSizes.fill(0);

    try
    {
        std::unordered_map<std::string, uint32> processorByName;
        processorByName.reserve(processors.size());
        for (uint32 index = 0; index < processors.size(); ++index)
        {
            const ECS::ProcessorDescriptor& descriptor = processors[index].descriptor;
            if (descriptor.name.empty() || (!descriptor.run && !descriptor.runWithContext) ||
                ToProcessorPhaseIndex(descriptor.phase) >= ProcessorPhaseCount ||
                !IsStepModeValidForPhase(descriptor.phase, descriptor.stepMode) ||
                !processorByName.emplace(descriptor.name, index).second)
            {
                outError = "Scene runtime processor registration is invalid or duplicated.";
                return false;
            }
        }

        const auto findDependency = [&processors, &processorByName, &outError](
                                        const ECS::ProcessorDescriptor& source,
                                        const std::string& targetName,
                                        bool sourceRunsBefore)
            -> const ECS::ProcessorDescriptor*
        {
            const auto found = processorByName.find(targetName);
            if (found == processorByName.end())
            {
                outError = "Processor '" + source.name + "' references unknown dependency '" +
                    targetName + "'.";
                return nullptr;
            }

            const ECS::ProcessorDescriptor& target = processors[found->second].descriptor;
            const ECS::ProcessorDescriptor& before = sourceRunsBefore ? source : target;
            const ECS::ProcessorDescriptor& after = sourceRunsBefore ? target : source;
            if (before.stepMode != after.stepMode)
            {
                outError = "Processor dependency between '" + before.name + "' and '" +
                    after.name + "' crosses variable and fixed clocks.";
                return nullptr;
            }
            if (static_cast<uint8>(before.phase) > static_cast<uint8>(after.phase))
            {
                outError = "Processor dependency from '" + before.name + "' to '" + after.name +
                    "' violates fixed phase order.";
                return nullptr;
            }
            return &target;
        };

        for (const RegisteredProcessor& registered : processors)
        {
            const ECS::ProcessorDescriptor& source = registered.descriptor;
            ECS::ProcessorDescriptor scheduled = source;
            scheduled.before.clear();
            scheduled.after.clear();

            for (const std::string& targetName : source.before)
            {
                const ECS::ProcessorDescriptor* target = findDependency(source, targetName, true);
                if (target == nullptr)
                {
                    return false;
                }
                if (target->phase == source.phase)
                {
                    scheduled.before.push_back(targetName);
                }
            }
            for (const std::string& targetName : source.after)
            {
                const ECS::ProcessorDescriptor* target = findDependency(source, targetName, false);
                if (target == nullptr)
                {
                    return false;
                }
                if (target->phase == source.phase)
                {
                    scheduled.after.push_back(targetName);
                }
            }

            const uint32 phaseIndex = ToProcessorPhaseIndex(scheduled.phase);
            ECS::ProcessorScheduler& scheduler =
                scheduled.stepMode == ECS::ProcessorStepMode::Variable ?
                    outVariableSchedulers[phaseIndex] : outFixedSchedulers[phaseIndex];
            if (!scheduler.Register(std::move(scheduled)))
            {
                outError = "Processor '" + source.name + "' could not be registered.";
                return false;
            }
            if (source.stepMode == ECS::ProcessorStepMode::Variable)
            {
                ++outVariableSchedulerSizes[phaseIndex];
            }
            else
            {
                ++outFixedSchedulerSizes[phaseIndex];
            }
        }

        for (uint32 phaseIndex = 0; phaseIndex < ProcessorPhaseCount; ++phaseIndex)
        {
            if (outVariableSchedulerSizes[phaseIndex] != 0 &&
                !outVariableSchedulers[phaseIndex].Compile())
            {
                outError = outVariableSchedulers[phaseIndex].GetLastCompileError();
                return false;
            }
            if (outFixedSchedulerSizes[phaseIndex] != 0 &&
                !outFixedSchedulers[phaseIndex].Compile())
            {
                outError = outFixedSchedulers[phaseIndex].GetLastCompileError();
                return false;
            }

            const std::vector<ECS::ProcessorConflict>& variableConflicts =
                outVariableSchedulers[phaseIndex].GetConflictDiagnostics();
            outConflicts.insert(
                outConflicts.end(), variableConflicts.begin(), variableConflicts.end());
            const std::vector<ECS::ProcessorConflict>& fixedConflicts =
                outFixedSchedulers[phaseIndex].GetConflictDiagnostics();
            outConflicts.insert(
                outConflicts.end(), fixedConflicts.begin(), fixedConflicts.end());
        }
        return true;
    }
    catch (...)
    {
        outError = "Scene runtime processor scheduler allocation failed.";
        return false;
    }
}

void SceneEcsRuntime::ResetCommandBarrierExecution(SceneEcsTickResult& result) const
{
    for (uint32 barrierIndex = 0; barrierIndex < RVX_SCENE_COMMAND_BARRIER_COUNT; ++barrierIndex)
    {
        result.commandBarriers[barrierIndex].barrier =
            static_cast<SceneCommandBarrier>(barrierIndex);
    }
}

ECS::SceneRuntimeId SceneEcsRuntime::GetSceneRuntimeId() const
{
    return m_registry.GetSceneRuntimeId();
}

SceneEntityRef SceneEcsRuntime::GetEntityRef(ECS::EntityHandle entity) const
{
    if (!IsOwnerThread() || !m_registry.IsAlive(entity))
    {
        return {};
    }

    return {.sceneRuntimeId = m_registry.GetSceneRuntimeId(), .entity = entity};
}

SceneSpawnTransaction SceneEcsRuntime::BeginSpawnTransaction()
{
    return IsOwnerThread() ?
               SceneSpawnTransaction(*this, GetSceneRuntimeId()) :
               SceneSpawnTransaction();
}

ECS::EntityHandle SceneEcsRuntime::CreateEntity(const RuntimeEntityDesc& desc)
{
    ECS::EntityTransaction transaction = m_registry.BeginTransaction();
    ECS::EntityReceipt entity = transaction.Create();
    if (!entity.IsQueued())
    {
        return ECS::EntityHandle::Invalid();
    }

    const bool recorded =
        transaction.Add<LocalTransform>(entity, desc.localTransform).IsQueued() &&
        transaction.Add<SimulationWorldTransform>(entity).IsQueued() &&
        transaction.Add<PreviousSimulationWorldTransform>(entity).IsQueued() &&
        transaction.Add<RenderWorldTransform>(entity).IsQueued() &&
        transaction.Add<Bounds>(entity, desc.bounds).IsQueued() &&
        transaction.Add<Active>(entity, desc.active).IsQueued() &&
        transaction.Add<Layer>(entity, desc.layer).IsQueued() &&
        transaction.Add<EntityLifecycleState>(entity).IsQueued() &&
        (desc.active.value || transaction.SetEnabled(entity, false).IsQueued());
    if (!recorded || !transaction.Commit())
    {
        return ECS::EntityHandle::Invalid();
    }
    return entity.GetEntity();
}

SceneSpawnEntityId SceneSpawnTransaction::Create(const RuntimeEntityDesc& desc)
{
    if (!CanRecord() || m_entities.size() >= RVX_INVALID_INDEX)
    {
        static_cast<void>(RejectRecording(ECS::CommandError::RecordingFailed));
        return {};
    }

    try
    {
        const SceneSpawnEntityId entity(static_cast<uint32>(m_entities.size()));
        m_entities.push_back({.desc = desc});
        return entity;
    }
    catch (...)
    {
        static_cast<void>(RejectRecording(ECS::CommandError::RecordingFailed));
        return {};
    }
}

bool SceneSpawnTransaction::SetParent(SceneSpawnEntityId child,
                                      SceneSpawnEntityId parent)
{
    if (!CanRecord() || !Contains(child) || !Contains(parent))
    {
        return RejectRecording(ECS::CommandError::InvalidTarget);
    }
    if (m_entities[child.m_index].parent.kind != ParentKind::None)
    {
        return RejectRecording(ECS::CommandError::OperationRejected);
    }

    m_entities[child.m_index].parent = {
        .kind = ParentKind::Local,
        .localParent = parent,
    };
    return true;
}

bool SceneSpawnTransaction::SetParent(SceneSpawnEntityId child,
                                      SceneEntityRef parent)
{
    if (!CanRecord() || !parent.IsValid() || parent.sceneRuntimeId != m_originRuntimeId)
    {
        return RejectRecording(ECS::CommandError::InvalidTarget);
    }
    return SetParent(child, parent.entity);
}

bool SceneSpawnTransaction::SetParent(SceneSpawnEntityId child,
                                      ECS::EntityHandle parent)
{
    if (!CanRecord() || !Contains(child) || !parent.IsValid())
    {
        return RejectRecording(ECS::CommandError::InvalidTarget);
    }
    if (m_entities[child.m_index].parent.kind != ParentKind::None)
    {
        return RejectRecording(ECS::CommandError::OperationRejected);
    }

    m_entities[child.m_index].parent = {
        .kind = ParentKind::External,
        .externalParent = parent,
    };
    return true;
}

bool SceneSpawnTransaction::ValidateExternalParentChain(ECS::EntityHandle parent) const
{
    if (m_runtime == nullptr || !parent.IsValid())
    {
        return false;
    }

    std::unordered_set<ECS::EntityHandle> visited;
    ECS::EntityHandle current = parent;
    while (current.IsValid())
    {
        if (!m_runtime->m_registry.IsAlive(current) || !visited.insert(current).second)
        {
            return false;
        }

        const ParentRelation* relation =
            m_runtime->m_registry.TryGet<ParentRelation>(current);
        current = relation != nullptr ? relation->parent : ECS::EntityHandle::Invalid();
    }
    return true;
}

bool SceneSpawnTransaction::ValidateParentGraph() const
{
    if (m_runtime == nullptr || !m_runtime->IsOwnerThread() ||
        !m_originRuntimeId.IsValid() ||
        m_originRuntimeId != m_runtime->GetSceneRuntimeId())
    {
        return false;
    }

    std::vector<uint8> states(m_entities.size(), 0u);
    for (uint32 start = 0; start < m_entities.size(); ++start)
    {
        if (states[start] == 2u)
        {
            continue;
        }

        uint32 current = start;
        bool reachedTerminalParent = false;
        while (states[current] == 0u)
        {
            states[current] = 1u;
            const ParentTarget& parent = m_entities[current].parent;
            if (parent.kind == ParentKind::None)
            {
                reachedTerminalParent = true;
                break;
            }
            if (parent.kind == ParentKind::External)
            {
                if (!ValidateExternalParentChain(parent.externalParent))
                {
                    return false;
                }
                reachedTerminalParent = true;
                break;
            }
            if (!Contains(parent.localParent))
            {
                return false;
            }
            current = parent.localParent.m_index;
        }

        if (!reachedTerminalParent && states[current] == 1u)
        {
            return false;
        }

        current = start;
        while (states[current] == 1u)
        {
            const ParentTarget& parent = m_entities[current].parent;
            states[current] = 2u;
            if (parent.kind != ParentKind::Local)
            {
                break;
            }
            current = parent.localParent.m_index;
        }
    }
    return true;
}

void SceneSpawnTransaction::DiscardRecordedValues() noexcept
{
    m_fragmentRecorders.clear();
    m_entities.clear();
    m_runtime = nullptr;
    m_originRuntimeId = {};
}

SceneSpawnCommitResult SceneSpawnTransaction::Commit()
{
    SceneSpawnCommitResult result;
    m_committed = true;

    if (m_runtime == nullptr || m_recordingError != ECS::CommandError::None)
    {
        result.SetRejected(m_recordingError == ECS::CommandError::None ?
                               ECS::CommandError::InvalidTarget : m_recordingError);
        DiscardRecordedValues();
        return result;
    }

    try
    {
        if (!ValidateParentGraph())
        {
            result.SetRejected(ECS::CommandError::OperationRejected);
            DiscardRecordedValues();
            return result;
        }

        // Reserve every mapping allocation before beginning the ECS commit. A
        // failed result allocation therefore cannot publish a Registry state
        // without also returning the complete generation-safe mapping.
        result.m_entities.resize(m_entities.size(), ECS::EntityHandle::Invalid());
        result.m_entityRefs.resize(m_entities.size());

        ECS::EntityTransaction transaction = m_runtime->m_registry.BeginTransaction();
        std::vector<ECS::EntityReceipt> entities;
        entities.reserve(m_entities.size());

        for (const PendingEntity& pending : m_entities)
        {
            ECS::EntityReceipt entity = transaction.Create();
            if (!entity.IsQueued())
            {
                result.SetRejected(entity.GetError());
                DiscardRecordedValues();
                return result;
            }

            const bool recorded =
                transaction.Add<LocalTransform>(entity, pending.desc.localTransform).IsQueued() &&
                transaction.Add<SimulationWorldTransform>(entity).IsQueued() &&
                transaction.Add<PreviousSimulationWorldTransform>(entity).IsQueued() &&
                transaction.Add<RenderWorldTransform>(entity).IsQueued() &&
                transaction.Add<Bounds>(entity, pending.desc.bounds).IsQueued() &&
                transaction.Add<Active>(entity, pending.desc.active).IsQueued() &&
                transaction.Add<Layer>(entity, pending.desc.layer).IsQueued() &&
                transaction.Add<EntityLifecycleState>(entity).IsQueued() &&
                (pending.desc.active.value || transaction.SetEnabled(entity, false).IsQueued());
            if (!recorded)
            {
                result.SetRejected(ECS::CommandError::RecordingFailed);
                DiscardRecordedValues();
                return result;
            }
            entities.push_back(std::move(entity));
        }

        for (const FragmentRecorder& record : m_fragmentRecorders)
        {
            if (!record(transaction, entities))
            {
                result.SetRejected(ECS::CommandError::RecordingFailed);
                DiscardRecordedValues();
                return result;
            }
        }

        for (uint32 childIndex = 0; childIndex < m_entities.size(); ++childIndex)
        {
            const ParentTarget& parent = m_entities[childIndex].parent;
            if (parent.kind == ParentKind::None)
            {
                continue;
            }

            const ECS::CommandReceipt relation = parent.kind == ParentKind::Local
                ? transaction.AddLinked<ParentRelation, &ParentRelation::parent>(
                    entities[childIndex], entities[parent.localParent.m_index])
                : transaction.Add<ParentRelation>(
                    entities[childIndex], {.parent = parent.externalParent});
            if (!relation.IsQueued())
            {
                result.SetRejected(relation.GetError());
                DiscardRecordedValues();
                return result;
            }
        }

        const ECS::TransactionReceipt transactionResult = transaction.Commit();
        if (!transactionResult.IsApplied())
        {
            result.SetRejected(transactionResult.GetError());
            DiscardRecordedValues();
            return result;
        }

        for (uint32 index = 0; index < entities.size(); ++index)
        {
            result.m_entities[index] = entities[index].GetEntity();
            result.m_entityRefs[index] = m_runtime->GetEntityRef(result.m_entities[index]);
        }
        result.SetApplied();
    }
    catch (...)
    {
        result.SetRejected(ECS::CommandError::RecordingFailed);
    }

    DiscardRecordedValues();
    return result;
}

bool SceneEcsRuntime::SetActive(ECS::EntityHandle entity, bool active)
{
    const EntityLifecycleState* lifecycle =
        m_registry.TryGet<EntityLifecycleState>(entity);
    const Active* currentActive = m_registry.TryGet<Active>(entity);
    if (lifecycle == nullptr || currentActive == nullptr ||
        lifecycle->phase != EntityLifecyclePhase::Alive)
    {
        return false;
    }

    const bool previousActive = currentActive->value;
    if (previousActive != active &&
        !m_registry.Write<Active>(entity, [active](Active& target) { target.value = active; }))
    {
        return false;
    }

    if (m_registry.IsEnabled(entity) != active &&
        !m_registry.SetEnabled(entity, active))
    {
        if (previousActive != active)
        {
            m_registry.Write<Active>(
                entity,
                [previousActive](Active& target) { target.value = previousActive; });
        }
        return false;
    }
    return true;
}

bool SceneEcsRuntime::SetLocalTransform(ECS::EntityHandle entity,
                                        const LocalTransform& transform)
{
    const EntityLifecycleState* lifecycle = m_registry.TryGet<EntityLifecycleState>(entity);
    return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
           m_transformHierarchy.SetLocalTransform(entity, transform);
}

SetWorldPoseResult SceneEcsRuntime::SetSimulationWorldPose(
    ECS::EntityHandle entity,
    const Vec3& worldTranslation,
    const Quat& worldRotation)
{
    const EntityLifecycleState* lifecycle = m_registry.TryGet<EntityLifecycleState>(entity);
    if (lifecycle == nullptr || lifecycle->phase != EntityLifecyclePhase::Alive)
    {
        return SetWorldPoseResult::InvalidEntity;
    }
    return m_transformHierarchy.SetWorldPose(entity, worldTranslation, worldRotation);
}

ReparentResult SceneEcsRuntime::Reparent(ECS::EntityHandle child,
                                         ECS::EntityHandle parent,
                                         ReparentMode mode)
{
    const EntityLifecycleState* childLifecycle = m_registry.TryGet<EntityLifecycleState>(child);
    if (childLifecycle == nullptr || childLifecycle->phase != EntityLifecyclePhase::Alive)
    {
        return ReparentResult::InvalidChild;
    }
    if (parent.IsValid())
    {
        const EntityLifecycleState* parentLifecycle =
            m_registry.TryGet<EntityLifecycleState>(parent);
        if (parentLifecycle == nullptr || parentLifecycle->phase != EntityLifecyclePhase::Alive)
        {
            return ReparentResult::InvalidParent;
        }
    }
    return m_transformHierarchy.Reparent(child, parent, mode);
}

ReparentResult SceneEcsRuntime::Detach(ECS::EntityHandle child, ReparentMode mode)
{
    return Reparent(child, ECS::EntityHandle::Invalid(), mode);
}

SkinnedMeshPoseRebindReceipt SceneEcsRuntime::RebindSkinnedMeshPoseOwner(
    const SkinnedMeshPoseRebindRequest& request)
{
    SkinnedMeshPoseRebindReceipt receipt;
    receipt.sceneRuntimeId = m_registry.GetSceneRuntimeId();

    if (!IsOwnerThread())
    {
        receipt.code = SkinnedMeshPoseRebindCode::OwnerThreadRequired;
        return receipt;
    }
    if (!request.expectedSceneRuntimeId.IsValid())
    {
        receipt.code = SkinnedMeshPoseRebindCode::InvalidExpectedSceneRuntime;
        return receipt;
    }
    if (request.expectedSceneRuntimeId != receipt.sceneRuntimeId)
    {
        receipt.code = SkinnedMeshPoseRebindCode::SceneRuntimeMismatch;
        return receipt;
    }
    if (request.meshMembers.empty())
    {
        receipt.code = SkinnedMeshPoseRebindCode::EmptyMeshSet;
        return receipt;
    }
    if (!request.expectedPoseOwner.IsValid() || !request.newPoseOwner.IsValid() ||
        request.expectedPoseOwner.sceneRuntimeId != receipt.sceneRuntimeId ||
        request.newPoseOwner.sceneRuntimeId != receipt.sceneRuntimeId ||
        GetEntityRef(request.expectedPoseOwner.entity) != request.expectedPoseOwner ||
        GetEntityRef(request.newPoseOwner.entity) != request.newPoseOwner ||
        request.sourceModelAssetValue == 0 || request.sourceSkinIndex < 0)
    {
        receipt.code = SkinnedMeshPoseRebindCode::InvalidPoseOwner;
        return receipt;
    }

    const auto hasCompatiblePoseBinding = [this, &request](ECS::EntityHandle entity)
    {
        const EntityLifecycleState* const lifecycle =
            m_registry.TryGet<EntityLifecycleState>(entity);
        const AnimationSkeletonBinding* const binding =
            m_registry.TryGet<AnimationSkeletonBinding>(entity);
        return lifecycle != nullptr && lifecycle->phase == EntityLifecyclePhase::Alive &&
               binding != nullptr && binding->sourceModelAssetValue == request.sourceModelAssetValue &&
               binding->sourceSkinIndex == request.sourceSkinIndex && binding->boneCount != 0;
    };
    if (!hasCompatiblePoseBinding(request.expectedPoseOwner.entity) ||
        !hasCompatiblePoseBinding(request.newPoseOwner.entity))
    {
        receipt.code = SkinnedMeshPoseRebindCode::PoseBindingMismatch;
        return receipt;
    }
    const AnimationSkeletonBinding* const expectedPoseBinding =
        m_registry.TryGet<AnimationSkeletonBinding>(request.expectedPoseOwner.entity);
    const AnimationSkeletonBinding* const newPoseBinding =
        m_registry.TryGet<AnimationSkeletonBinding>(request.newPoseOwner.entity);
    if (expectedPoseBinding == nullptr || newPoseBinding == nullptr ||
        expectedPoseBinding->boneCount != newPoseBinding->boneCount)
    {
        receipt.code = SkinnedMeshPoseRebindCode::PoseBindingMismatch;
        return receipt;
    }

    struct PendingMesh
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        SkinnedMeshBinding replacement;
        bool wasEnabled = true;
    };
    std::vector<PendingMesh> pending;
    try
    {
        pending.reserve(request.meshMembers.size());
        std::vector<ECS::EntityHandle> uniqueMembers;
        uniqueMembers.reserve(request.meshMembers.size());
        for (const SceneEntityRef member : request.meshMembers)
        {
            if (!member.IsValid() || member.sceneRuntimeId != receipt.sceneRuntimeId ||
                GetEntityRef(member.entity) != member)
            {
                receipt.code = SkinnedMeshPoseRebindCode::InvalidMeshMember;
                return receipt;
            }
            uniqueMembers.push_back(member.entity);
        }
        std::sort(uniqueMembers.begin(), uniqueMembers.end());
        if (std::adjacent_find(uniqueMembers.begin(), uniqueMembers.end()) != uniqueMembers.end())
        {
            receipt.code = SkinnedMeshPoseRebindCode::DuplicateMeshMember;
            return receipt;
        }

        for (const SceneEntityRef member : request.meshMembers)
        {
            const EntityLifecycleState* const lifecycle =
                m_registry.TryGet<EntityLifecycleState>(member.entity);
            const Mesh* const mesh = m_registry.TryGet<Mesh>(member.entity);
            const SkinnedMeshBinding* const binding =
                m_registry.TryGet<SkinnedMeshBinding>(member.entity);
            if (lifecycle == nullptr || lifecycle->phase != EntityLifecyclePhase::Alive ||
                mesh == nullptr)
            {
                receipt.code = SkinnedMeshPoseRebindCode::InvalidMeshMember;
                return receipt;
            }
            if (binding == nullptr)
            {
                receipt.code = SkinnedMeshPoseRebindCode::MeshBindingUnavailable;
                return receipt;
            }
            if (binding->poseEntity != request.expectedPoseOwner.entity ||
                binding->sourceModelAssetValue != request.sourceModelAssetValue ||
                binding->sourceSkinIndex != request.sourceSkinIndex)
            {
                receipt.code = SkinnedMeshPoseRebindCode::MeshBindingMismatch;
                return receipt;
            }

            PendingMesh& staged = pending.emplace_back();
            staged.entity = member.entity;
            staged.replacement = *binding;
            staged.replacement.poseEntity = request.newPoseOwner.entity;
            staged.wasEnabled = m_registry.IsFragmentEnabled<SkinnedMeshBinding>(member.entity);
        }
    }
    catch (...)
    {
        receipt.code = SkinnedMeshPoseRebindCode::RecordingRejected;
        receipt.transactionError = ECS::CommandError::RecordingFailed;
        return receipt;
    }

    ECS::EntityTransaction transaction = m_registry.BeginTransaction();
    bool recorded = true;
    for (const PendingMesh& mesh : pending)
    {
        recorded = recorded && transaction.Remove<SkinnedMeshBinding>(mesh.entity).IsQueued();
        recorded = recorded && transaction.Add<SkinnedMeshBinding>(mesh.entity, mesh.replacement).IsQueued();
        if (!mesh.wasEnabled)
        {
            recorded = recorded &&
                       transaction.SetFragmentEnabled<SkinnedMeshBinding>(mesh.entity, false).IsQueued();
        }
    }
    if (!recorded)
    {
        receipt.code = SkinnedMeshPoseRebindCode::RecordingRejected;
        receipt.transactionError = ECS::CommandError::RecordingFailed;
        return receipt;
    }

    const ECS::TransactionReceipt transactionReceipt = transaction.Commit();
    receipt.transactionStatus = transactionReceipt.GetStatus();
    receipt.transactionError = transactionReceipt.GetError();
    receipt.meshCount = transactionReceipt.IsApplied() ? static_cast<uint32>(pending.size()) : 0;
    receipt.code = transactionReceipt.IsApplied() ?
                       SkinnedMeshPoseRebindCode::Applied :
                       SkinnedMeshPoseRebindCode::TransactionRejected;
    return receipt;
}

DestroyRequestResult SceneEcsRuntime::RequestDestroy(
    ECS::EntityHandle entity,
    CleanupDomainMask requiredCleanupDomains)
{
    if (!IsOwnerThread())
    {
        return DestroyRequestResult::MutationRejected;
    }
    const EntityLifecycleState* lifecycle =
        m_registry.TryGet<EntityLifecycleState>(entity);
    if (lifecycle == nullptr)
    {
        return m_registry.IsAlive(entity) ? DestroyRequestResult::MissingLifecycleState :
                                            DestroyRequestResult::InvalidEntity;
    }
    if (lifecycle->phase != EntityLifecyclePhase::Alive)
    {
        return DestroyRequestResult::AlreadyPending;
    }

    try
    {
        if (m_pendingDestroyEntities.size() == m_pendingDestroyEntities.capacity())
        {
            m_pendingDestroyEntities.reserve(m_pendingDestroyEntities.size() + 1u);
        }
    }
    catch (...)
    {
        return DestroyRequestResult::MutationRejected;
    }

    const CleanupDomainMask sanitizedDomains =
        InferRequiredCleanupDomains(entity, requiredCleanupDomains);
    if (!m_registry.Write<EntityLifecycleState>(
            entity,
            [sanitizedDomains](EntityLifecycleState& state)
            {
                state.phase = EntityLifecyclePhase::PendingDestroy;
                state.requiredCleanupDomains = sanitizedDomains;
                state.acknowledgedCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
            }))
    {
        return DestroyRequestResult::MutationRejected;
    }

    if (m_registry.IsEnabled(entity) && !m_registry.Disable(entity))
    {
        m_registry.Write<EntityLifecycleState>(
            entity,
            [](EntityLifecycleState& state)
            {
                state.phase = EntityLifecyclePhase::Alive;
                state.requiredCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
                state.acknowledgedCleanupDomains = ToCleanupDomainMask(CleanupDomain::None);
            });
        return DestroyRequestResult::MutationRejected;
    }

    m_pendingDestroyEntities.push_back(entity);
    return DestroyRequestResult::Accepted;
}

DestroyRequestResult SceneEcsRuntime::RequestDestroyBatch(
    std::span<const ECS::EntityHandle> entities,
    CleanupDomainMask requiredCleanupDomains)
{
    if (!IsOwnerThread() || entities.empty())
    {
        return DestroyRequestResult::MutationRejected;
    }

    struct OriginalState
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        EntityLifecycleState lifecycle;
        bool enabled = false;
    };

    std::vector<OriginalState> originals;
    std::unordered_set<ECS::EntityHandle> uniqueEntities;
    try
    {
        originals.reserve(entities.size());
        uniqueEntities.reserve(entities.size());
        m_pendingDestroyEntities.reserve(
            m_pendingDestroyEntities.size() + entities.size());
    }
    catch (...)
    {
        return DestroyRequestResult::MutationRejected;
    }

    for (const ECS::EntityHandle entity : entities)
    {
        if (!uniqueEntities.insert(entity).second)
        {
            return DestroyRequestResult::MutationRejected;
        }

        const EntityLifecycleState* lifecycle =
            m_registry.TryGet<EntityLifecycleState>(entity);
        if (lifecycle == nullptr)
        {
            return m_registry.IsAlive(entity)
                       ? DestroyRequestResult::MissingLifecycleState
                       : DestroyRequestResult::InvalidEntity;
        }
        if (lifecycle->phase != EntityLifecyclePhase::Alive)
        {
            return DestroyRequestResult::AlreadyPending;
        }
        originals.push_back({
            .entity = entity,
            .lifecycle = *lifecycle,
            .enabled = m_registry.IsEnabled(entity),
        });
    }

    CleanupDomainMask sanitizedDomains =
        requiredCleanupDomains & ToCleanupDomainMask(CleanupDomain::All);
    for (const OriginalState& original : originals)
    {
        sanitizedDomains =
            InferRequiredCleanupDomains(original.entity, sanitizedDomains);
    }
    size_t appliedCount = 0;
    for (; appliedCount < originals.size(); ++appliedCount)
    {
        const OriginalState& original = originals[appliedCount];
        const bool lifecycleWritten = m_registry.Write<EntityLifecycleState>(
            original.entity,
            [sanitizedDomains](EntityLifecycleState& state)
            {
                state.phase = EntityLifecyclePhase::PendingDestroy;
                state.requiredCleanupDomains = sanitizedDomains;
                state.acknowledgedCleanupDomains =
                    ToCleanupDomainMask(CleanupDomain::None);
            });
        const bool disabled =
            lifecycleWritten && (!original.enabled || m_registry.Disable(original.entity));
        if (!disabled)
        {
            if (lifecycleWritten)
            {
                static_cast<void>(m_registry.Write<EntityLifecycleState>(
                    original.entity,
                    [&original](EntityLifecycleState& state)
                    {
                        state = original.lifecycle;
                    }));
            }
            break;
        }
    }

    if (appliedCount != originals.size())
    {
        for (size_t index = 0; index < appliedCount; ++index)
        {
            const OriginalState& original = originals[index];
            if (original.enabled && !m_registry.IsEnabled(original.entity))
            {
                static_cast<void>(m_registry.Enable(original.entity));
            }
            static_cast<void>(m_registry.Write<EntityLifecycleState>(
                original.entity,
                [&original](EntityLifecycleState& state)
                {
                    state = original.lifecycle;
                }));
        }
        return DestroyRequestResult::MutationRejected;
    }

    for (const OriginalState& original : originals)
    {
        m_pendingDestroyEntities.push_back(original.entity);
    }
    return DestroyRequestResult::Accepted;
}

DestroyAllRequestResult SceneEcsRuntime::RequestDestroyAll(
    CleanupDomainMask additionalCleanupDomains)
{
    return RequestDestroyAllExcept({}, additionalCleanupDomains);
}

DestroyAllRequestResult SceneEcsRuntime::RequestDestroyAllExcept(
    std::span<const ECS::EntityHandle> exclusions,
    CleanupDomainMask additionalCleanupDomains)
{
    if (!IsOwnerThread())
    {
        return {
            .result = DestroyRequestResult::MutationRejected,
            .requestedEntityCount = 0,
        };
    }

    struct OriginalState
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        EntityLifecycleState lifecycle;
        CleanupDomainMask requiredCleanupDomains =
            ToCleanupDomainMask(CleanupDomain::None);
        bool enabled = false;
    };

    std::vector<ECS::EntityHandle> retained;
    std::vector<OriginalState> originals;
    try
    {
        retained.assign(exclusions.begin(), exclusions.end());
        std::sort(retained.begin(), retained.end());
        retained.erase(std::unique(retained.begin(), retained.end()), retained.end());
        for (const ECS::EntityHandle entity : retained)
        {
            const EntityLifecycleState* lifecycle =
                m_registry.TryGet<EntityLifecycleState>(entity);
            if (lifecycle == nullptr ||
                lifecycle->phase != EntityLifecyclePhase::Alive)
            {
                return {
                    .result = DestroyRequestResult::InvalidEntity,
                    .requestedEntityCount = 0,
                };
            }
        }

        m_registry.Query<ECS::Read<EntityLifecycleState>>()
            .EachIncludingAllDisabled(
                [this, additionalCleanupDomains, &retained, &originals](
                    ECS::EntityHandle entity,
                    const EntityLifecycleState& lifecycle)
                {
                    if (lifecycle.phase == EntityLifecyclePhase::Alive &&
                        !std::binary_search(retained.begin(), retained.end(), entity))
                    {
                        originals.push_back({
                            .entity = entity,
                            .lifecycle = lifecycle,
                            .requiredCleanupDomains = InferRequiredCleanupDomains(
                                entity,
                                additionalCleanupDomains &
                                    ToCleanupDomainMask(CleanupDomain::All)),
                            .enabled = m_registry.IsEnabled(entity),
                        });
                    }
                });
        std::sort(originals.begin(), originals.end(),
                  [](const OriginalState& left, const OriginalState& right)
                  { return left.entity < right.entity; });
        m_pendingDestroyEntities.reserve(
            m_pendingDestroyEntities.size() + originals.size());
    }
    catch (...)
    {
        return {
            .result = DestroyRequestResult::MutationRejected,
            .requestedEntityCount = 0,
        };
    }

    if (originals.empty())
    {
        return {
            .result = DestroyRequestResult::Accepted,
            .requestedEntityCount = 0,
        };
    }

    size_t appliedCount = 0;
    for (; appliedCount < originals.size(); ++appliedCount)
    {
        const OriginalState& original = originals[appliedCount];
        const bool lifecycleWritten = m_registry.Write<EntityLifecycleState>(
            original.entity,
            [&original](EntityLifecycleState& state)
            {
                state.phase = EntityLifecyclePhase::PendingDestroy;
                state.requiredCleanupDomains = original.requiredCleanupDomains;
                state.acknowledgedCleanupDomains =
                    ToCleanupDomainMask(CleanupDomain::None);
            });
        const bool disabled =
            lifecycleWritten && (!original.enabled || m_registry.Disable(original.entity));
        if (!disabled)
        {
            if (lifecycleWritten)
            {
                static_cast<void>(m_registry.Write<EntityLifecycleState>(
                    original.entity,
                    [&original](EntityLifecycleState& state)
                    { state = original.lifecycle; }));
            }
            break;
        }
    }

    if (appliedCount != originals.size())
    {
        for (size_t index = 0; index < appliedCount; ++index)
        {
            const OriginalState& original = originals[index];
            if (original.enabled && !m_registry.IsEnabled(original.entity))
            {
                static_cast<void>(m_registry.Enable(original.entity));
            }
            static_cast<void>(m_registry.Write<EntityLifecycleState>(
                original.entity,
                [&original](EntityLifecycleState& state)
                { state = original.lifecycle; }));
        }
        return {
            .result = DestroyRequestResult::MutationRejected,
            .requestedEntityCount = 0,
        };
    }

    for (const OriginalState& original : originals)
    {
        m_pendingDestroyEntities.push_back(original.entity);
    }
    return {
        .result = DestroyRequestResult::Accepted,
        .requestedEntityCount = static_cast<uint64>(originals.size()),
    };
}

uint32 SceneEcsRuntime::PublishPendingDestroyCleanup(CleanupReason reason)
{
    PruneDeadLifecycleHandles();
    SortAndUnique(m_pendingDestroyEntities);
    if (m_pendingDestroyEntities.empty())
    {
        return 0;
    }

    try
    {
        m_cleanupRecords.reserve(m_cleanupRecords.size() + m_pendingDestroyEntities.size());
        m_cleanupRequiredEntities.reserve(
            m_cleanupRequiredEntities.size() + m_pendingDestroyEntities.size());
        m_retiringEntities.reserve(m_retiringEntities.size() + m_pendingDestroyEntities.size());
    }
    catch (...)
    {
        return 0;
    }

    uint32 publishedCount = 0;
    std::vector<ECS::EntityHandle> remainingPending;
    try
    {
        remainingPending.reserve(m_pendingDestroyEntities.size());
    }
    catch (...)
    {
        return 0;
    }
    for (ECS::EntityHandle entity : m_pendingDestroyEntities)
    {
        const EntityLifecycleState* lifecycle =
            m_registry.TryGet<EntityLifecycleState>(entity);
        if (lifecycle == nullptr || lifecycle->phase != EntityLifecyclePhase::PendingDestroy)
        {
            continue;
        }

        const CleanupDomainMask requiredDomains = lifecycle->requiredCleanupDomains;
        const bool requiresAcknowledgement =
            requiredDomains != ToCleanupDomainMask(CleanupDomain::None);
        m_cleanupRecords.push_back({
            .sceneRuntimeId = GetSceneRuntimeId(),
            .entity = entity,
            .sequence = m_nextCleanupSequence,
            .reason = reason,
            .requiredCleanupDomains = requiredDomains,
            .acknowledgedCleanupDomains = ToCleanupDomainMask(CleanupDomain::None),
        });
        if (!m_registry.Write<EntityLifecycleState>(
                entity,
                [requiresAcknowledgement](EntityLifecycleState& state)
                {
                    state.phase = requiresAcknowledgement ?
                                      EntityLifecyclePhase::CleanupRequired :
                                      EntityLifecyclePhase::Retiring;
                }))
        {
            m_cleanupRecords.pop_back();
            remainingPending.push_back(entity);
            continue;
        }

        if (requiresAcknowledgement)
        {
            m_cleanupRequiredEntities.push_back(entity);
        }
        else
        {
            m_retiringEntities.push_back(entity);
        }
        ++m_nextCleanupSequence;
        ++publishedCount;
    }
    m_pendingDestroyEntities = std::move(remainingPending);
    TrimCleanupRecordsToCapacity();
    return publishedCount;
}

bool SceneEcsRuntime::AcknowledgeCleanup(ECS::EntityHandle entity,
                                         CleanupDomainMask acknowledgedDomains)
{
    const EntityLifecycleState* lifecycle =
        m_registry.TryGet<EntityLifecycleState>(entity);
    if (lifecycle == nullptr ||
        acknowledgedDomains == ToCleanupDomainMask(CleanupDomain::None) ||
        (acknowledgedDomains & ~lifecycle->requiredCleanupDomains) != 0)
    {
        return false;
    }

    if (lifecycle->phase == EntityLifecyclePhase::Retiring ||
        lifecycle->phase == EntityLifecyclePhase::Recyclable)
    {
        return (lifecycle->acknowledgedCleanupDomains & acknowledgedDomains) ==
               acknowledgedDomains;
    }
    if (lifecycle->phase != EntityLifecyclePhase::CleanupRequired)
    {
        return false;
    }

    const CleanupDomainMask acknowledgedAfter =
        lifecycle->acknowledgedCleanupDomains | acknowledgedDomains;
    const bool becomesRetiring =
        (acknowledgedAfter & lifecycle->requiredCleanupDomains) ==
        lifecycle->requiredCleanupDomains;
    if (becomesRetiring)
    {
        try
        {
            if (m_retiringEntities.size() == m_retiringEntities.capacity())
            {
                m_retiringEntities.reserve(m_retiringEntities.size() + 1u);
            }
        }
        catch (...)
        {
            return false;
        }
    }

    if (!m_registry.Write<EntityLifecycleState>(
            entity,
            [acknowledgedDomains, becomesRetiring](EntityLifecycleState& state)
            {
                state.acknowledgedCleanupDomains |= acknowledgedDomains;
                if (becomesRetiring)
                {
                    state.phase = EntityLifecyclePhase::Retiring;
                }
            }))
    {
        return false;
    }

    if (becomesRetiring)
    {
        m_cleanupRequiredEntities.erase(
            std::remove(m_cleanupRequiredEntities.begin(),
                        m_cleanupRequiredEntities.end(),
                        entity),
            m_cleanupRequiredEntities.end());
        m_retiringEntities.push_back(entity);
    }
    return true;
}

uint32 SceneEcsRuntime::AdvanceRetirements()
{
    PruneDeadLifecycleHandles();
    SortAndUnique(m_retiringEntities);
    if (m_retiringEntities.empty())
    {
        return 0;
    }

    try
    {
        m_recyclableEntities.reserve(m_recyclableEntities.size() + m_retiringEntities.size());
    }
    catch (...)
    {
        return 0;
    }

    uint32 advancedCount = 0;
    std::vector<ECS::EntityHandle> remaining;
    remaining.reserve(m_retiringEntities.size());
    for (ECS::EntityHandle entity : m_retiringEntities)
    {
        const EntityLifecycleState* lifecycle =
            m_registry.TryGet<EntityLifecycleState>(entity);
        if (lifecycle == nullptr || lifecycle->phase != EntityLifecyclePhase::Retiring)
        {
            continue;
        }

        if (m_registry.Write<EntityLifecycleState>(
                entity,
                [](EntityLifecycleState& state)
                {
                    state.phase = EntityLifecyclePhase::Recyclable;
                }))
        {
            m_recyclableEntities.push_back(entity);
            ++advancedCount;
        }
        else
        {
            remaining.push_back(entity);
        }
    }
    m_retiringEntities = std::move(remaining);
    return advancedCount;
}

uint32 SceneEcsRuntime::RecycleRecyclableEntities()
{
    PruneDeadLifecycleHandles();
    SortAndUnique(m_recyclableEntities);
    if (m_recyclableEntities.empty())
    {
        return 0;
    }

    struct DetachedChildState
    {
        ECS::EntityHandle child = ECS::EntityHandle::Invalid();
        ECS::EntityHandle parent = ECS::EntityHandle::Invalid();
        LocalTransform local;
    };

    std::vector<ECS::EntityHandle> childrenToDetach;
    for (ECS::EntityHandle entity : m_recyclableEntities)
    {
        const std::vector<ECS::EntityHandle> children =
            m_transformHierarchy.GetChildren(entity);
        childrenToDetach.insert(childrenToDetach.end(), children.begin(), children.end());
    }
    SortAndUnique(childrenToDetach);
    std::vector<DetachedChildState> detachedChildren;
    detachedChildren.reserve(childrenToDetach.size());
    const auto rollbackDetachedChildren = [this, &detachedChildren]()
    {
        bool restored = true;
        for (auto state = detachedChildren.rbegin(); state != detachedChildren.rend(); ++state)
        {
            try
            {
                if (!m_registry.IsAlive(state->child) || !m_registry.IsAlive(state->parent) ||
                    !m_transformHierarchy.SetLocalTransform(state->child, state->local) ||
                    m_transformHierarchy.Reparent(
                        state->child, state->parent, ReparentMode::KeepLocal) !=
                        ReparentResult::Applied)
                {
                    restored = false;
                }
            }
            catch (...)
            {
                restored = false;
            }
        }
        return restored;
    };

    for (ECS::EntityHandle child : childrenToDetach)
    {
        const ECS::EntityHandle parent = m_transformHierarchy.GetParent(child);
        const LocalTransform* local = m_registry.TryGet<LocalTransform>(child);
        if (!parent.IsValid() || local == nullptr)
        {
            rollbackDetachedChildren();
            return 0;
        }
        detachedChildren.push_back({.child = child, .parent = parent, .local = *local});
        if (m_transformHierarchy.Detach(child, ReparentMode::KeepWorld) !=
            ReparentResult::Applied)
        {
            detachedChildren.pop_back();
            rollbackDetachedChildren();
            return 0;
        }
    }

    ECS::EntityTransaction transaction = m_registry.BeginTransaction();
    for (ECS::EntityHandle entity : m_recyclableEntities)
    {
        if (!m_registry.IsAlive(entity) ||
            !transaction.Destroy(entity).IsQueued())
        {
            rollbackDetachedChildren();
            return 0;
        }
    }
    if (!transaction.Commit())
    {
        rollbackDetachedChildren();
        return 0;
    }

    const uint32 recycledCount = static_cast<uint32>(m_recyclableEntities.size());
    m_recyclableEntities.clear();
    return recycledCount;
}

CleanupRecordRead SceneEcsRuntime::ReadCleanupRecords(CleanupRecordCursor& cursor) const
{
    CleanupRecordRead result;
    if (cursor.nextSequence < m_firstCleanupSequence ||
        cursor.nextSequence > m_nextCleanupSequence)
    {
        result.continuity = CleanupRecordContinuity::Lost;
        ++m_cleanupContinuityLossCount;
        cursor.nextSequence = m_nextCleanupSequence;
        result.nextSequence = m_nextCleanupSequence;
        return result;
    }

    const size_t start = static_cast<size_t>(cursor.nextSequence - m_firstCleanupSequence);
    result.records.assign(m_cleanupRecords.begin() + static_cast<std::ptrdiff_t>(start),
                          m_cleanupRecords.end());
    cursor.nextSequence = m_nextCleanupSequence;
    result.nextSequence = m_nextCleanupSequence;
    return result;
}

SceneEcsDiagnosticsSnapshot SceneEcsRuntime::GetDiagnosticsSnapshot() const
{
    const auto countLive = [this](const std::vector<ECS::EntityHandle>& entities)
    {
        return static_cast<uint32>(std::count_if(
            entities.begin(),
            entities.end(),
            [this](ECS::EntityHandle entity) { return m_registry.IsAlive(entity); }));
    };
    const SceneCommandDiagnosticsSnapshot commandDiagnostics = GetCommandDiagnosticsSnapshot();
    uint32 queuedCommandBufferCount = 0;
    uint64 appliedCommandBufferCount = 0;
    uint64 rejectedCommandBufferCount = 0;
    for (const SceneCommandBarrierDiagnostics& barrier : commandDiagnostics.barriers)
    {
        queuedCommandBufferCount += barrier.queuedBufferCount;
        appliedCommandBufferCount += barrier.appliedBufferCount;
        rejectedCommandBufferCount += barrier.rejectedBufferCount;
    }

    return {
        .sceneRuntimeId = GetSceneRuntimeId(),
        .entityCount = m_registry.GetEntityCount(),
        .pendingDestroyCount = countLive(m_pendingDestroyEntities),
        .cleanupRequiredCount = countLive(m_cleanupRequiredEntities),
        .retiringCount = countLive(m_retiringEntities),
        .recyclableCount = countLive(m_recyclableEntities),
        .nextCleanupSequence = m_nextCleanupSequence,
        .firstCleanupSequence = m_firstCleanupSequence,
        .cleanupContinuityLossCount = m_cleanupContinuityLossCount,
        .nextStructuralSequence = m_registry.GetStructuralJournal().GetNextSequence(),
        .localTransformWriteVersion = m_registry.GetFragmentWriteVersion<LocalTransform>(),
        .simulationWorldTransformWriteVersion =
            m_registry.GetFragmentWriteVersion<SimulationWorldTransform>(),
        .renderWorldTransformWriteVersion =
            m_registry.GetFragmentWriteVersion<RenderWorldTransform>(),
        .frameSequence = m_frameSequence,
        .fixedStepSequence = m_fixedStepSequence,
        .sceneSnapshotRevision = m_latestFrozenSnapshot != nullptr ?
                                     m_latestFrozenSnapshot->revision :
                                     0,
        .spatialEntryCount = m_spatialIndex.GetEntryCount(),
        .queuedCommandBufferCount = queuedCommandBufferCount,
        .appliedCommandBufferCount = appliedCommandBufferCount,
        .rejectedCommandBufferCount = rejectedCommandBufferCount,
    };
}

TransformResolveStats SceneEcsRuntime::BeginSimulationFrame()
{
    return m_transformHierarchy.BeginSimulationFrame();
}

TransformResolveStats SceneEcsRuntime::ResolveSimulationTransforms()
{
    return m_transformHierarchy.ResolveSimulationTransforms();
}

uint32 SceneEcsRuntime::SynchronizeRenderWorldTransforms()
{
    return m_transformHierarchy.SynchronizeRenderWorldTransforms();
}

void SceneEcsRuntime::PruneDeadLifecycleHandles()
{
    const auto removeDead = [this](std::vector<ECS::EntityHandle>& entities)
    {
        entities.erase(
            std::remove_if(entities.begin(),
                           entities.end(),
                           [this](ECS::EntityHandle entity)
                           {
                               return !m_registry.IsAlive(entity);
                           }),
            entities.end());
    };

    removeDead(m_pendingDestroyEntities);
    removeDead(m_cleanupRequiredEntities);
    removeDead(m_retiringEntities);
    removeDead(m_recyclableEntities);
}

void SceneEcsRuntime::TrimCleanupRecordsToCapacity()
{
    if (m_cleanupRecords.size() <= m_cleanupRecordCapacity)
    {
        m_firstCleanupSequence = m_cleanupRecords.empty() ?
                                     m_nextCleanupSequence :
                                     m_cleanupRecords.front().sequence;
        return;
    }

    const size_t removeCount = m_cleanupRecords.size() - m_cleanupRecordCapacity;
    m_cleanupRecords.erase(
        m_cleanupRecords.begin(),
        m_cleanupRecords.begin() + static_cast<std::ptrdiff_t>(removeCount));
    m_firstCleanupSequence = m_cleanupRecords.empty() ?
                                 m_nextCleanupSequence :
                                 m_cleanupRecords.front().sequence;
}
} // namespace RVX::SceneECS
