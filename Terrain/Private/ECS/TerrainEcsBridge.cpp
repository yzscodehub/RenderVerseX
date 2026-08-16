#include "Terrain/ECS/TerrainEcsBridge.h"

#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace RVX::Terrain
{
namespace
{
    [[nodiscard]] bool IsFinite(float value) { return std::isfinite(value); }
    [[nodiscard]] bool IsFinite(const Vec3& value) { return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z); }
    [[nodiscard]] bool IsFinite(const Mat4& value)
    { for (uint32 column = 0; column < 4; ++column) for (uint32 row = 0; row < 4; ++row) if (!IsFinite(value[column][row])) return false; return true; }
    [[nodiscard]] bool IsKnown(TerrainEcsCommand value) { return value >= TerrainEcsCommand::None && value <= TerrainEcsCommand::Rebuild; }
    [[nodiscard]] bool IsValid(const TerrainEcsConfig& value)
    { return value.heightmapAssetId.IsValid() && value.materialAssetId.IsValid() && value.configurationRevision != 0 && IsFinite(value.size) && value.size.x > 0.0f && value.size.y > 0.0f && value.size.z > 0.0f && IsFinite(value.lodBias) && value.patchSize != 0 && value.maxLODLevels != 0; }
    [[nodiscard]] bool NeedsCleanup(const SceneECS::EntityLifecycleState* lifecycle)
    { return lifecycle != nullptr && (lifecycle->requiredCleanupDomains & SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::TerrainFeature)) != 0; }
    [[nodiscard]] Vec3 Position(const Mat4& transform) { return {transform[3][0], transform[3][1], transform[3][2]}; }
} // namespace

struct TerrainEcsBridge::State
{
    struct Binding { ECS::EntityHandle entity = ECS::EntityHandle::Invalid(); TerrainEcsRuntimeHandle runtime = TerrainEcsRuntimeHandle::Invalid(); uint64 configurationRevision = 0; TerrainEcsBindingStatus status = TerrainEcsBindingStatus::Unbound; bool awaitingCleanupAcknowledgement = false; };
    State(SceneECS::SceneEcsRuntime& runtimeIn, ITerrainEcsGateway& gatewayIn)
        : runtime(&runtimeIn), gateway(&gatewayIn), sceneRuntimeId(runtimeIn.GetSceneRuntimeId()), structuralCursor(runtimeIn.GetRegistry().GetStructuralJournal().CreateCursor()) {}
    ~State() { if (gateway != nullptr) for (Binding& binding : bindings) if (binding.runtime.IsValid()) static_cast<void>(gateway->Release(binding.runtime)); }
    [[nodiscard]] uint32 Find(ECS::EntityHandle entity) const { const auto found = index.find(entity); return found == index.end() ? RVX_INVALID_INDEX : found->second; }
    [[nodiscard]] bool Add(ECS::EntityHandle entity, uint32& out)
    {
        if ((out = Find(entity)) != RVX_INVALID_INDEX) return true;
        try { bindings.reserve(bindings.size() + 1u); index.reserve(index.size() + 1u); out = static_cast<uint32>(bindings.size()); bindings.push_back({.entity = entity}); index.emplace(entity, out); return true; }
        catch (...) { out = RVX_INVALID_INDEX; return false; }
    }
    void Erase(uint32 value)
    { if (value >= bindings.size()) return; index.erase(bindings[value].entity); const uint32 last = static_cast<uint32>(bindings.size() - 1u); if (value != last) { bindings[value] = std::move(bindings[last]); index[bindings[value].entity] = value; } bindings.pop_back(); }
    [[nodiscard]] bool Release(Binding& binding)
    { if (!binding.runtime.IsValid()) return true; if (gateway->Release(binding.runtime) == TerrainEcsReleaseResult::Failed) return false; binding.runtime = TerrainEcsRuntimeHandle::Invalid(); ++releasedRuntimeCount; return true; }
    void Write(ECS::Registry& registry, ECS::EntityHandle entity, TerrainEcsBindingStatus status, std::optional<uint64> config = {}, std::optional<uint64> command = {}, std::optional<uint64> payload = {})
    { static_cast<void>(registry.Write<TerrainEcsState>(entity, [=](TerrainEcsState& state) { state.status = status; if (config) state.lastAppliedConfigurationRevision = *config; if (command && *command > state.lastConsumedCommandSequence) state.lastConsumedCommandSequence = *command; if (payload) state.lastPublishedPayloadRevision = *payload; ++state.synchronizationRevision; })); }
    struct Eligible
    { const TerrainEcsConfig* config = nullptr; const TerrainEcsIntent* intent = nullptr; const TerrainEcsState* state = nullptr; const SceneECS::RenderWorldTransform* transform = nullptr; const SceneECS::EntityLifecycleState* lifecycle = nullptr; TerrainEcsBindingStatus failure = TerrainEcsBindingStatus::Inactive; bool value = false; };
    [[nodiscard]] Eligible Get(ECS::Registry& registry, ECS::EntityHandle entity) const
    {
        Eligible result; result.lifecycle = registry.TryGet<SceneECS::EntityLifecycleState>(entity);
        if (!registry.IsAlive(entity) || result.lifecycle == nullptr || result.lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive) { result.failure = TerrainEcsBindingStatus::PendingDestroy; return result; }
        result.config = registry.TryGet<TerrainEcsConfig>(entity); result.intent = registry.TryGet<TerrainEcsIntent>(entity); result.state = registry.TryGet<TerrainEcsState>(entity); result.transform = registry.TryGet<SceneECS::RenderWorldTransform>(entity); const SceneECS::Active* active = registry.TryGet<SceneECS::Active>(entity);
        if (result.config == nullptr || result.intent == nullptr || result.state == nullptr || result.transform == nullptr || active == nullptr || !active->value || !registry.IsEnabled(entity) || !registry.Has<SceneECS::TerrainRuntimeTag>(entity) || !registry.IsFragmentEnabled<TerrainEcsConfig>(entity) || !registry.IsFragmentEnabled<TerrainEcsIntent>(entity) || !registry.IsFragmentEnabled<TerrainEcsState>(entity) || !registry.IsFragmentEnabled<SceneECS::TerrainRuntimeTag>(entity) || !registry.IsFragmentEnabled<SceneECS::RenderWorldTransform>(entity)) return result;
        if (!IsValid(*result.config) || !IsFinite(result.transform->matrix)) { result.failure = TerrainEcsBindingStatus::InvalidConfiguration; return result; }
        result.value = true; return result;
    }
    [[nodiscard]] bool PublishInactive(ECS::ProcessorExecutionContext& context, ECS::EntityHandle entity, const TerrainEcsConfig* config, const SceneECS::RenderWorldTransform* transform, std::string reason)
    {
        TerrainRenderSnapshotItem item; item.heightmapAssetId = config != nullptr ? config->heightmapAssetId : AssetId{}; item.materialAssetId = config != nullptr ? config->materialAssetId : AssetId{}; item.worldPosition = transform != nullptr ? Position(transform->matrix) : Vec3(0.0f); item.size = config != nullptr ? config->size : Vec3(0.0f); item.lodBias = config != nullptr ? config->lodBias : 0.0f; item.patchSize = config != nullptr ? config->patchSize : 0; item.maxLODLevels = config != nullptr ? config->maxLODLevels : 0; item.renderGpuPathAvailable = false; item.cpuDataAvailable = false; item.renderPathReason = std::move(reason);
        if (!runtime->PublishTerrainFeatureSnapshot(entity, std::move(item), config != nullptr ? config->configurationRevision : 0)) { context.ReportFailure("Terrain feature snapshot publication was rejected."); return false; } return true;
    }
    [[nodiscard]] bool Publish(ECS::ProcessorExecutionContext& context, ECS::EntityHandle entity, Binding& binding, const TerrainEcsConfig& config, const SceneECS::RenderWorldTransform& transform)
    {
        TerrainEcsRuntimeSnapshot output;
        if (!gateway->CaptureSnapshot(binding.runtime, output)) { ++synchronizationFailureCount; binding.status = TerrainEcsBindingStatus::SynchronizationFailed; Write(context.registry, entity, binding.status); return PublishInactive(context, entity, &config, &transform, "Terrain gateway did not provide a complete value snapshot."); }
        output.item.heightmapAssetId = config.heightmapAssetId; output.item.materialAssetId = config.materialAssetId; output.item.worldPosition = Position(transform.matrix); const uint64 revision = output.payloadRevision != 0 ? output.payloadRevision : config.configurationRevision;
        if (!runtime->PublishTerrainFeatureSnapshot(entity, std::move(output.item), revision)) { context.ReportFailure("Terrain feature snapshot publication was rejected."); return false; }
        Write(context.registry, entity, binding.status, config.configurationRevision, {}, revision); return true;
    }
    void Process(ECS::ProcessorExecutionContext& context, ECS::EntityHandle entity, const Eligible& eligible)
    {
        const TerrainEcsConfig& config = *eligible.config; const TerrainEcsIntent& intent = *eligible.intent; const TerrainEcsState& current = *eligible.state; const auto& transform = *eligible.transform;
        TerrainEcsCommand command = TerrainEcsCommand::None; std::optional<uint64> sequence;
        if (intent.command != TerrainEcsCommand::None) { if (!IsKnown(intent.command) || intent.sequence == 0) { ++rejectedCommandCount; Write(context.registry, entity, TerrainEcsBindingStatus::InvalidConfiguration); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain command is invalid.")); return; } if (intent.sequence > current.lastConsumedCommandSequence) { command = intent.command; sequence = intent.sequence; } }
        uint32 bindingIndex = Find(entity);
        if (bindingIndex == RVX_INVALID_INDEX && (config.autoActivate || command == TerrainEcsCommand::Activate || command == TerrainEcsCommand::Rebuild))
        {
            if (!Add(entity, bindingIndex)) { ++rejectedConfigurationCount; Write(context.registry, entity, TerrainEcsBindingStatus::RuntimeRejected); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain bridge side-table allocation failed.")); return; }
            Binding& binding = bindings[bindingIndex]; binding.runtime = gateway->Create({.config = config, .worldTransform = transform.matrix, .command = command, .deltaSeconds = context.deltaSeconds});
            if (!binding.runtime.IsValid() || !gateway->IsAlive(binding.runtime)) { if (binding.runtime.IsValid() && !Release(binding)) { context.ReportFailure("Terrain gateway release failed after a rejected create."); return; } ++rejectedConfigurationCount; Erase(bindingIndex); Write(context.registry, entity, TerrainEcsBindingStatus::RuntimeRejected); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain gateway rejected the ECS configuration.")); return; }
            ++createdRuntimeCount;
        }
        if (bindingIndex == RVX_INVALID_INDEX) { Write(context.registry, entity, TerrainEcsBindingStatus::Inactive, {}, sequence); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain runtime is inactive.")); return; }
        Binding& binding = bindings[bindingIndex];
        if (!binding.runtime.IsValid() || !gateway->IsAlive(binding.runtime)) { if (!Release(binding)) { context.ReportFailure("Terrain gateway retirement failed after absence detection."); return; } Erase(bindingIndex); Write(context.registry, entity, TerrainEcsBindingStatus::Inactive); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain runtime is no longer alive.")); return; }
        if (!gateway->Update(binding.runtime, {.config = config, .worldTransform = transform.matrix, .command = command, .deltaSeconds = context.deltaSeconds})) { ++synchronizationFailureCount; binding.status = TerrainEcsBindingStatus::SynchronizationFailed; Write(context.registry, entity, binding.status); static_cast<void>(PublishInactive(context, entity, &config, &transform, "Terrain gateway synchronization failed.")); return; }
        binding.configurationRevision = config.configurationRevision; binding.status = command == TerrainEcsCommand::Deactivate ? TerrainEcsBindingStatus::Inactive : TerrainEcsBindingStatus::Active; Write(context.registry, entity, binding.status, config.configurationRevision, sequence); static_cast<void>(Publish(context, entity, binding, config, transform));
    }
    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        const ECS::StructuralJournalRead changes = context.registry.ReadStructuralChanges(structuralCursor); const bool lost = changes.continuity == ECS::StructuralJournalContinuity::Lost; if (lost) ++structuralContinuityLossCount; if (!initialReconcileComplete || lost) ++authoritativeReconcileCount; initialReconcileComplete = true;
        std::vector<ECS::EntityHandle> existing; existing.reserve(bindings.size()); for (const Binding& binding : bindings) existing.push_back(binding.entity); std::sort(existing.begin(), existing.end());
        for (ECS::EntityHandle entity : existing)
        {
            const Eligible eligible = Get(context.registry, entity); if (eligible.value) continue; const uint32 bindingIndex = Find(entity); if (bindingIndex == RVX_INVALID_INDEX) continue; Binding& binding = bindings[bindingIndex];
            if (!Release(binding)) { context.ReportFailure("Terrain gateway release failed during reconciliation."); return; }
            if (NeedsCleanup(eligible.lifecycle) && eligible.lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive) { binding.awaitingCleanupAcknowledgement = true; binding.status = TerrainEcsBindingStatus::PendingDestroy; Write(context.registry, entity, binding.status); }
            else { Write(context.registry, entity, eligible.failure); Erase(bindingIndex); }
        }
        std::vector<ECS::EntityHandle> entities; context.registry.Query<ECS::Read<TerrainEcsConfig>>().EachIncludingAllDisabled([&entities](ECS::EntityHandle entity, const TerrainEcsConfig&) { entities.push_back(entity); }); std::sort(entities.begin(), entities.end());
        for (ECS::EntityHandle entity : entities)
        {
            const Eligible eligible = Get(context.registry, entity);
            if (!eligible.value) { if (eligible.failure == TerrainEcsBindingStatus::InvalidConfiguration) ++rejectedConfigurationCount; Write(context.registry, entity, eligible.failure); if (eligible.config != nullptr && eligible.transform != nullptr) static_cast<void>(PublishInactive(context, entity, eligible.config, eligible.transform, "Terrain feature is inactive or invalid.")); continue; }
            Process(context, entity, eligible); if (context.HasReportedFailure()) return;
        }
    }
    void Acknowledge(ECS::ProcessorExecutionContext& context)
    {
        std::vector<ECS::EntityHandle> entities; context.registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled([&entities](ECS::EntityHandle entity, const SceneECS::EntityLifecycleState& lifecycle) { if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired && (lifecycle.requiredCleanupDomains & SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::TerrainFeature)) != 0) entities.push_back(entity); }); std::sort(entities.begin(), entities.end());
        for (ECS::EntityHandle entity : entities)
        {
            const uint32 bindingIndex = Find(entity); if (bindingIndex != RVX_INVALID_INDEX && !Release(bindings[bindingIndex])) { context.ReportFailure("Terrain gateway release failed during cleanup retry."); return; }
            if (!runtime->RemoveTerrainFeatureSnapshot(entity) || !runtime->AcknowledgeCleanup(entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::TerrainFeature))) { context.ReportFailure("Terrain feature cleanup acknowledgement was rejected."); return; }
            const uint32 retained = Find(entity); if (retained != RVX_INVALID_INDEX) Erase(retained);
        }
    }
    void Cleanup(ECS::ProcessorExecutionContext& context)
    { Acknowledge(context); if (context.HasReportedFailure()) return; const SceneECS::CleanupRecordRead records = runtime->ReadCleanupRecords(cleanupCursor); if (records.continuity == SceneECS::CleanupRecordContinuity::Lost) { ++cleanupContinuityLossCount; Reconcile(context); if (!context.HasReportedFailure()) Acknowledge(context); return; } Acknowledge(context); }
    [[nodiscard]] bool PrepareForShutdown()
    { ++shutdownPreparationCount; bool released = true; for (Binding& binding : bindings) if (!Release(binding)) { ++shutdownReleaseFailureCount; released = false; } if (released) { bindings.clear(); index.clear(); } return released && bindings.empty() && runtime->GetFeatureSnapshotCounts().terrainCount == 0; }
    SceneECS::SceneEcsRuntime* runtime = nullptr; ITerrainEcsGateway* gateway = nullptr; ECS::SceneRuntimeId sceneRuntimeId; ECS::StructuralJournalCursor structuralCursor; SceneECS::CleanupRecordCursor cleanupCursor; std::vector<Binding> bindings; std::unordered_map<ECS::EntityHandle, uint32> index; TerrainEcsBridgeRegistrationResult registration = TerrainEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0, cleanupContinuityLossCount = 0, authoritativeReconcileCount = 0, createdRuntimeCount = 0, releasedRuntimeCount = 0, rejectedConfigurationCount = 0, rejectedCommandCount = 0, synchronizationFailureCount = 0, shutdownPreparationCount = 0, shutdownReleaseFailureCount = 0; bool initialReconcileComplete = false, processorsEnabled = false;
};

TerrainEcsBridge::TerrainEcsBridge(SceneECS::SceneEcsRuntime& runtime, ITerrainEcsGateway& gateway) : m_state(std::make_shared<State>(runtime, gateway)) {}
TerrainEcsBridge::~TerrainEcsBridge() = default;
TerrainEcsBridgeRegistrationResult TerrainEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->gateway == nullptr || !m_state->sceneRuntimeId.IsValid()) return TerrainEcsBridgeRegistrationResult::InvalidRuntime;
    if (m_state->registration == TerrainEcsBridgeRegistrationResult::Registered) return TerrainEcsBridgeRegistrationResult::AlreadyRegistered;
    const std::string prefix = "TerrainEcsBridge." + std::to_string(m_state->sceneRuntimeId.GetValue()) + "."; const std::weak_ptr<State> weak = m_state;
    const auto descriptor = [weak, prefix](std::string suffix, ECS::ProcessorPhase phase, std::vector<std::type_index> reads, std::vector<std::type_index> writes, std::function<void(State&, ECS::ProcessorExecutionContext&)> run)
    { ECS::ProcessorDescriptor value; value.name = prefix + std::move(suffix); value.phase = phase; value.stepMode = ECS::ProcessorStepMode::Variable; value.access.reads = std::move(reads); value.access.writes = std::move(writes); value.access.resourceWrites = {std::type_index(typeid(ITerrainEcsGateway))}; value.runWithContext = [weak, run = std::move(run)](ECS::ProcessorExecutionContext& context) { if (const std::shared_ptr<State> state = weak.lock(); state != nullptr && state->processorsEnabled) run(*state, context); }; return value; };
    try
    {
        const std::vector<std::type_index> reads = {std::type_index(typeid(TerrainEcsConfig)), std::type_index(typeid(TerrainEcsIntent)), std::type_index(typeid(TerrainEcsState)), std::type_index(typeid(SceneECS::TerrainRuntimeTag)), std::type_index(typeid(SceneECS::RenderWorldTransform)), std::type_index(typeid(SceneECS::Active)), std::type_index(typeid(SceneECS::EntityLifecycleState))}; std::vector<ECS::ProcessorDescriptor> descriptors; descriptors.reserve(2u);
        descriptors.push_back(descriptor("Feature", ECS::ProcessorPhase::Feature, reads, {std::type_index(typeid(TerrainEcsState))}, [](State& state, ECS::ProcessorExecutionContext& context) { state.Reconcile(context); })); descriptors.push_back(descriptor("EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup, reads, {std::type_index(typeid(TerrainEcsState)), std::type_index(typeid(SceneECS::EntityLifecycleState))}, [](State& state, ECS::ProcessorExecutionContext& context) { state.Cleanup(context); }));
        const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors)); m_state->processorsEnabled = registered; m_state->registration = registered ? TerrainEcsBridgeRegistrationResult::Registered : TerrainEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    }
    catch (...) { m_state->registration = TerrainEcsBridgeRegistrationResult::ProcessorRegistrationFailed; } return m_state->registration;
}
bool TerrainEcsBridge::IsRegistered() const { return m_state != nullptr && m_state->registration == TerrainEcsBridgeRegistrationResult::Registered; }
ECS::SceneRuntimeId TerrainEcsBridge::GetSceneRuntimeId() const { return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{}; }
bool TerrainEcsBridge::PrepareForShutdown() { return m_state != nullptr && m_state->runtime != nullptr && m_state->gateway != nullptr && m_state->PrepareForShutdown(); }
TerrainEcsRuntimeHandle TerrainEcsBridge::FindRuntime(ECS::SceneRuntimeId sceneRuntimeId, ECS::EntityHandle entity) const { if (m_state == nullptr || m_state->runtime == nullptr || m_state->gateway == nullptr || sceneRuntimeId != m_state->sceneRuntimeId || !m_state->runtime->GetRegistry().IsAlive(entity)) return TerrainEcsRuntimeHandle::Invalid(); const uint32 value = m_state->Find(entity); if (value == RVX_INVALID_INDEX) return TerrainEcsRuntimeHandle::Invalid(); const TerrainEcsRuntimeHandle handle = m_state->bindings[value].runtime; return handle.IsValid() && m_state->gateway->IsAlive(handle) ? handle : TerrainEcsRuntimeHandle::Invalid(); }
TerrainEcsBridgeDiagnosticsSnapshot TerrainEcsBridge::GetDiagnosticsSnapshot() const
{
    TerrainEcsBridgeDiagnosticsSnapshot result; if (m_state == nullptr) return result; result.sceneRuntimeId = m_state->sceneRuntimeId; result.registration = m_state->registration; result.bindingCount = static_cast<uint32>(m_state->bindings.size()); result.publishedSnapshotCount = m_state->runtime != nullptr ? m_state->runtime->GetFeatureSnapshotCounts().terrainCount : 0; result.structuralContinuityLossCount = m_state->structuralContinuityLossCount; result.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount; result.authoritativeReconcileCount = m_state->authoritativeReconcileCount; result.createdRuntimeCount = m_state->createdRuntimeCount; result.releasedRuntimeCount = m_state->releasedRuntimeCount; result.rejectedConfigurationCount = m_state->rejectedConfigurationCount; result.rejectedCommandCount = m_state->rejectedCommandCount; result.synchronizationFailureCount = m_state->synchronizationFailureCount; result.shutdownPreparationCount = m_state->shutdownPreparationCount; result.shutdownReleaseFailureCount = m_state->shutdownReleaseFailureCount; result.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings) { result.bindings.push_back({.entity = binding.entity, .runtime = binding.runtime, .status = binding.status, .configurationRevision = binding.configurationRevision, .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement}); if (binding.runtime.IsValid()) ++result.outstandingRuntimeCount; if (binding.awaitingCleanupAcknowledgement) ++result.pendingCleanupCount; }
    std::sort(result.bindings.begin(), result.bindings.end(), [](const TerrainEcsBindingDiagnostic& lhs, const TerrainEcsBindingDiagnostic& rhs) { return lhs.entity < rhs.entity; }); return result;
}
} // namespace RVX::Terrain
