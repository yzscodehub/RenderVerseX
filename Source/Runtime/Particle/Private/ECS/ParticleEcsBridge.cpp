#include "Particle/ECS/ParticleEcsBridge.h"

#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace RVX::Particle
{
namespace
{
    [[nodiscard]] bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFinite(const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!IsFinite(value[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsKnownCommand(ParticleEcsCommand command)
    {
        return command >= ParticleEcsCommand::None &&
               command <= ParticleEcsCommand::Clear;
    }

    [[nodiscard]] bool IsValidConfig(const ParticleEcsConfig& config)
    {
        return config.systemAssetId.IsValid() && config.configurationRevision != 0 &&
               config.maxParticleCount != 0 && IsFinite(config.emissionRateScale) &&
               config.emissionRateScale >= 0.0f && IsFinite(config.simulationSpeed) &&
               config.simulationSpeed >= 0.0f;
    }

    [[nodiscard]] bool RequiresFeatureCleanup(const SceneECS::EntityLifecycleState* lifecycle)
    {
        return lifecycle != nullptr &&
               (lifecycle->requiredCleanupDomains &
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::ParticleFeature)) != 0;
    }

    [[nodiscard]] Vec3 ExtractTranslation(const Mat4& transform)
    {
        return {transform[3][0], transform[3][1], transform[3][2]};
    }
} // namespace

struct ParticleEcsBridge::State
{
    struct Binding
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        ParticleEcsRuntimeHandle runtime = ParticleEcsRuntimeHandle::Invalid();
        uint64 configurationRevision = 0;
        bool autoPlayConsumed = false;
        bool awaitingCleanupAcknowledgement = false;
        ParticleEcsBindingStatus status = ParticleEcsBindingStatus::Unbound;
    };

    State(SceneECS::SceneEcsRuntime& runtimeIn, IParticleEcsGateway& gatewayIn)
        : runtime(&runtimeIn)
        , gateway(&gatewayIn)
        , sceneRuntimeId(runtimeIn.GetSceneRuntimeId())
        , structuralCursor(runtimeIn.GetRegistry().GetStructuralJournal().CreateCursor())
    {
    }

    ~State()
    {
        if (gateway == nullptr)
        {
            return;
        }
        for (Binding& binding : bindings)
        {
            if (binding.runtime.IsValid())
            {
                static_cast<void>(gateway->Release(binding.runtime));
            }
        }
    }

    [[nodiscard]] uint32 FindBinding(ECS::EntityHandle entity) const
    {
        const auto found = entityToBinding.find(entity);
        return found != entityToBinding.end() ? found->second : RVX_INVALID_INDEX;
    }

    [[nodiscard]] bool AddBinding(ECS::EntityHandle entity, uint32& outIndex)
    {
        const uint32 existing = FindBinding(entity);
        if (existing != RVX_INVALID_INDEX)
        {
            outIndex = existing;
            return true;
        }
        try
        {
            bindings.reserve(bindings.size() + 1u);
            entityToBinding.reserve(entityToBinding.size() + 1u);
            outIndex = static_cast<uint32>(bindings.size());
            bindings.push_back({.entity = entity});
            entityToBinding.emplace(entity, outIndex);
            return true;
        }
        catch (...)
        {
            if (!bindings.empty() && bindings.back().entity == entity)
            {
                bindings.pop_back();
            }
            entityToBinding.erase(entity);
            outIndex = RVX_INVALID_INDEX;
            return false;
        }
    }

    void EraseBinding(uint32 index)
    {
        if (index >= bindings.size())
        {
            return;
        }
        entityToBinding.erase(bindings[index].entity);
        const uint32 lastIndex = static_cast<uint32>(bindings.size() - 1u);
        if (index != lastIndex)
        {
            bindings[index] = std::move(bindings[lastIndex]);
            entityToBinding[bindings[index].entity] = index;
        }
        bindings.pop_back();
    }

    void WriteState(ECS::Registry& registry,
                    ECS::EntityHandle entity,
                    ParticleEcsBindingStatus status,
                    std::optional<uint64> configurationRevision = std::nullopt,
                    std::optional<uint64> commandSequence = std::nullopt,
                    std::optional<uint64> payloadRevision = std::nullopt)
    {
        static_cast<void>(registry.Write<ParticleEcsState>(
            entity,
            [status, configurationRevision, commandSequence, payloadRevision](ParticleEcsState& state)
            {
                state.status = status;
                if (configurationRevision.has_value())
                {
                    state.lastAppliedConfigurationRevision = *configurationRevision;
                }
                if (commandSequence.has_value() &&
                    *commandSequence > state.lastConsumedCommandSequence)
                {
                    state.lastConsumedCommandSequence = *commandSequence;
                }
                if (payloadRevision.has_value())
                {
                    state.lastPublishedPayloadRevision = *payloadRevision;
                }
                ++state.synchronizationRevision;
            }));
    }

    [[nodiscard]] bool Release(Binding& binding)
    {
        if (!binding.runtime.IsValid())
        {
            return true;
        }
        const ParticleEcsReleaseResult result = gateway->Release(binding.runtime);
        if (result == ParticleEcsReleaseResult::Failed)
        {
            return false;
        }
        binding.runtime = ParticleEcsRuntimeHandle::Invalid();
        ++releasedRuntimeCount;
        return true;
    }

    struct Eligibility
    {
        const ParticleEcsConfig* config = nullptr;
        const ParticleEcsIntent* intent = nullptr;
        const ParticleEcsState* state = nullptr;
        const SceneECS::RenderWorldTransform* transform = nullptr;
        const SceneECS::EntityLifecycleState* lifecycle = nullptr;
        ParticleEcsBindingStatus failure = ParticleEcsBindingStatus::Unbound;
        bool eligible = false;
    };

    [[nodiscard]] Eligibility GetEligibility(ECS::Registry& registry,
                                             ECS::EntityHandle entity) const
    {
        Eligibility result;
        result.lifecycle = registry.TryGet<SceneECS::EntityLifecycleState>(entity);
        if (!registry.IsAlive(entity) || result.lifecycle == nullptr ||
            result.lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive)
        {
            result.failure = ParticleEcsBindingStatus::PendingDestroy;
            return result;
        }
        result.config = registry.TryGet<ParticleEcsConfig>(entity);
        result.intent = registry.TryGet<ParticleEcsIntent>(entity);
        result.state = registry.TryGet<ParticleEcsState>(entity);
        result.transform = registry.TryGet<SceneECS::RenderWorldTransform>(entity);
        const SceneECS::Active* active = registry.TryGet<SceneECS::Active>(entity);
        if (result.config == nullptr || result.intent == nullptr || result.state == nullptr ||
            result.transform == nullptr || active == nullptr ||
            !registry.Has<SceneECS::ParticleRuntimeTag>(entity) || !registry.IsEnabled(entity) ||
            !registry.IsFragmentEnabled<ParticleEcsConfig>(entity) ||
            !registry.IsFragmentEnabled<ParticleEcsIntent>(entity) ||
            !registry.IsFragmentEnabled<ParticleEcsState>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::ParticleRuntimeTag>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::RenderWorldTransform>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::Active>(entity) || !active->value)
        {
            result.failure = ParticleEcsBindingStatus::Stopped;
            return result;
        }
        if (!IsValidConfig(*result.config) || !IsFinite(result.transform->matrix))
        {
            result.failure = ParticleEcsBindingStatus::InvalidConfiguration;
            return result;
        }
        result.eligible = true;
        return result;
    }

    [[nodiscard]] bool PublishInactive(ECS::ProcessorExecutionContext& context,
                                        ECS::EntityHandle entity,
                                        const ParticleEcsConfig* config,
                                        const SceneECS::RenderWorldTransform* transform,
                                        std::string reason)
    {
        ParticleRenderSnapshotItem snapshot;
        snapshot.systemAssetId = config != nullptr ? config->systemAssetId : AssetId{};
        snapshot.worldMatrix = transform != nullptr ? transform->matrix : Mat4(1.0f);
        snapshot.position = ExtractTranslation(snapshot.worldMatrix);
        snapshot.visible = false;
        snapshot.simulationSupported = false;
        snapshot.renderPayloadAvailable = false;
        snapshot.unsupportedReason = std::move(reason);
        if (!runtime->PublishParticleFeatureSnapshot(
                entity, std::move(snapshot), config != nullptr ? config->configurationRevision : 0))
        {
            context.ReportFailure("Particle feature snapshot publication was rejected.");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool PublishSnapshot(ECS::ProcessorExecutionContext& context,
                                        ECS::EntityHandle entity,
                                        Binding& binding,
                                        const ParticleEcsConfig& config,
                                        const SceneECS::RenderWorldTransform& transform)
    {
        ParticleEcsRuntimeSnapshot output;
        if (!gateway->CaptureSnapshot(binding.runtime, output))
        {
            ++synchronizationFailureCount;
            binding.status = ParticleEcsBindingStatus::SynchronizationFailed;
            WriteState(context.registry, entity, binding.status);
            return PublishInactive(context, entity, &config, &transform,
                                   "Particle gateway did not provide a complete value snapshot.");
        }
        output.item.systemAssetId = config.systemAssetId;
        output.item.worldMatrix = transform.matrix;
        output.item.position = ExtractTranslation(transform.matrix);
        const uint64 payloadRevision = output.payloadRevision != 0 ?
                                           output.payloadRevision : config.configurationRevision;
        if (!runtime->PublishParticleFeatureSnapshot(entity, std::move(output.item), payloadRevision))
        {
            context.ReportFailure("Particle feature snapshot publication was rejected.");
            return false;
        }
        WriteState(context.registry, entity, binding.status, config.configurationRevision,
                   std::nullopt, payloadRevision);
        return true;
    }

    void ProcessEligible(ECS::ProcessorExecutionContext& context,
                         ECS::EntityHandle entity,
                         const Eligibility& eligibility)
    {
        const ParticleEcsConfig& config = *eligibility.config;
        const ParticleEcsIntent& intent = *eligibility.intent;
        const ParticleEcsState& state = *eligibility.state;
        const SceneECS::RenderWorldTransform& transform = *eligibility.transform;

        ParticleEcsCommand command = ParticleEcsCommand::None;
        std::optional<uint64> consumedSequence;
        if (intent.command != ParticleEcsCommand::None)
        {
            if (!IsKnownCommand(intent.command) || intent.sequence == 0)
            {
                ++rejectedCommandCount;
                WriteState(context.registry, entity, ParticleEcsBindingStatus::InvalidConfiguration);
                static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                                  "Particle command is invalid."));
                return;
            }
            if (intent.sequence > state.lastConsumedCommandSequence)
            {
                command = intent.command;
                consumedSequence = intent.sequence;
            }
        }

        uint32 bindingIndex = FindBinding(entity);
        const auto ensureBinding = [this, entity, &bindingIndex]()
        {
            return bindingIndex != RVX_INVALID_INDEX || AddBinding(entity, bindingIndex);
        };

        const bool commandStartsRuntime = command == ParticleEcsCommand::Play ||
                                          command == ParticleEcsCommand::Restart;
        if (bindingIndex == RVX_INVALID_INDEX && (commandStartsRuntime || config.autoPlay))
        {
            if (!ensureBinding())
            {
                ++rejectedConfigurationCount;
                WriteState(context.registry, entity, ParticleEcsBindingStatus::RuntimeRejected);
                static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                                  "Particle bridge side-table allocation failed."));
                return;
            }
            Binding& binding = bindings[bindingIndex];
            binding.runtime = gateway->Create({
                .config = config,
                .worldTransform = transform.matrix,
                .command = command,
                .deltaSeconds = context.deltaSeconds,
            });
            binding.autoPlayConsumed = config.autoPlay;
            if (!binding.runtime.IsValid() || !gateway->IsAlive(binding.runtime))
            {
                if (binding.runtime.IsValid() && !Release(binding))
                {
                    context.ReportFailure("Particle gateway release failed after a rejected create.");
                    return;
                }
                ++rejectedConfigurationCount;
                binding.status = ParticleEcsBindingStatus::RuntimeRejected;
                WriteState(context.registry, entity, binding.status);
                EraseBinding(bindingIndex);
                static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                                  "Particle gateway rejected the ECS configuration."));
                return;
            }
            ++createdRuntimeCount;
        }

        if (bindingIndex == RVX_INVALID_INDEX)
        {
            const ParticleEcsBindingStatus stoppedStatus = command == ParticleEcsCommand::Pause ?
                                                        ParticleEcsBindingStatus::Paused :
                                                        ParticleEcsBindingStatus::Stopped;
            WriteState(context.registry, entity, stoppedStatus, std::nullopt, consumedSequence);
            static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                              "Particle runtime is inactive."));
            return;
        }

        Binding& binding = bindings[bindingIndex];
        if (!binding.runtime.IsValid() || !gateway->IsAlive(binding.runtime))
        {
            if (!Release(binding))
            {
                context.ReportFailure("Particle gateway retirement failed after absence detection.");
                return;
            }
            binding.status = ParticleEcsBindingStatus::Stopped;
            WriteState(context.registry, entity, binding.status);
            EraseBinding(bindingIndex);
            static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                              "Particle runtime is no longer alive."));
            return;
        }

        if (!gateway->Update(binding.runtime, {
                .config = config,
                .worldTransform = transform.matrix,
                .command = command,
                .deltaSeconds = context.deltaSeconds,
            }))
        {
            ++synchronizationFailureCount;
            binding.status = ParticleEcsBindingStatus::SynchronizationFailed;
            WriteState(context.registry, entity, binding.status);
            static_cast<void>(PublishInactive(context, entity, &config, &transform,
                                              "Particle gateway synchronization failed."));
            return;
        }

        binding.configurationRevision = config.configurationRevision;
        binding.status = command == ParticleEcsCommand::Stop || command == ParticleEcsCommand::Clear ?
                             ParticleEcsBindingStatus::Stopped :
                         command == ParticleEcsCommand::Pause ?
                             ParticleEcsBindingStatus::Paused :
                             ParticleEcsBindingStatus::Active;
        WriteState(context.registry, entity, binding.status, config.configurationRevision,
                   consumedSequence);
        static_cast<void>(PublishSnapshot(context, entity, binding, config, transform));
    }

    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        const ECS::StructuralJournalRead read =
            context.registry.ReadStructuralChanges(structuralCursor);
        const bool continuityLost = read.continuity == ECS::StructuralJournalContinuity::Lost;
        if (continuityLost)
        {
            ++structuralContinuityLossCount;
        }
        if (!initialReconcileComplete || continuityLost)
        {
            ++authoritativeReconcileCount;
        }
        initialReconcileComplete = true;

        std::vector<ECS::EntityHandle> existing;
        existing.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            existing.push_back(binding.entity);
        }
        std::sort(existing.begin(), existing.end());
        for (const ECS::EntityHandle entity : existing)
        {
            const Eligibility eligibility = GetEligibility(context.registry, entity);
            if (eligibility.eligible)
            {
                continue;
            }
            const uint32 index = FindBinding(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            Binding& binding = bindings[index];
            if (!Release(binding))
            {
                context.ReportFailure("Particle gateway release failed during reconciliation.");
                return;
            }
            const bool retainCleanupEvidence = RequiresFeatureCleanup(eligibility.lifecycle) &&
                                               eligibility.lifecycle->phase !=
                                                   SceneECS::EntityLifecyclePhase::Alive;
            if (retainCleanupEvidence)
            {
                binding.awaitingCleanupAcknowledgement = true;
                binding.status = ParticleEcsBindingStatus::PendingDestroy;
                WriteState(context.registry, entity, binding.status);
            }
            else
            {
                WriteState(context.registry, entity, eligibility.failure);
                EraseBinding(index);
            }
        }

        std::vector<ECS::EntityHandle> entities;
        context.registry.Query<ECS::Read<ParticleEcsConfig>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const ParticleEcsConfig&)
            {
                entities.push_back(entity);
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const Eligibility eligibility = GetEligibility(context.registry, entity);
            if (!eligibility.eligible)
            {
                if (eligibility.failure == ParticleEcsBindingStatus::InvalidConfiguration)
                {
                    ++rejectedConfigurationCount;
                }
                WriteState(context.registry, entity, eligibility.failure);
                continue;
            }
            ProcessEligible(context, entity, eligibility);
            if (context.HasReportedFailure())
            {
                return;
            }
        }
    }

    void AcknowledgeCleanup(ECS::ProcessorExecutionContext& context)
    {
        std::vector<ECS::EntityHandle> entities;
        context.registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired &&
                    (lifecycle.requiredCleanupDomains &
                     SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::ParticleFeature)) != 0)
                {
                    entities.push_back(entity);
                }
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBinding(entity);
            if (index != RVX_INVALID_INDEX)
            {
                Binding& binding = bindings[index];
                if (!Release(binding))
                {
                    context.ReportFailure("Particle gateway release failed during cleanup retry.");
                    return;
                }
            }
            // A frozen value must disappear before ParticleFeature
            // acknowledges its side-table release. Render remains an
            // independent retirement consumer inferred from ParticleRuntimeTag.
            if (!runtime->RemoveParticleFeatureSnapshot(entity) ||
                !runtime->AcknowledgeCleanup(
                    entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::ParticleFeature)))
            {
                context.ReportFailure("Particle feature cleanup acknowledgement was rejected.");
                return;
            }
            const uint32 retained = FindBinding(entity);
            if (retained != RVX_INVALID_INDEX)
            {
                EraseBinding(retained);
            }
        }
    }

    void Cleanup(ECS::ProcessorExecutionContext& context)
    {
        AcknowledgeCleanup(context);
        if (context.HasReportedFailure())
        {
            return;
        }
        const SceneECS::CleanupRecordRead read = runtime->ReadCleanupRecords(cleanupCursor);
        if (read.continuity == SceneECS::CleanupRecordContinuity::Lost)
        {
            ++cleanupContinuityLossCount;
            Reconcile(context);
            if (!context.HasReportedFailure())
            {
                AcknowledgeCleanup(context);
            }
            return;
        }
        // Records prove order for diagnostics, while lifecycle remains the
        // authoritative retry source. Do not acknowledge merely because a
        // record was consumed; AcknowledgeCleanup rechecks release and removal.
        static_cast<void>(read);
        AcknowledgeCleanup(context);
    }

    [[nodiscard]] bool PrepareForShutdown()
    {
        ++shutdownPreparationCount;
        bool released = true;
        for (Binding& binding : bindings)
        {
            if (!Release(binding))
            {
                ++shutdownReleaseFailureCount;
                binding.status = ParticleEcsBindingStatus::SynchronizationFailed;
                released = false;
            }
        }
        if (released)
        {
            bindings.clear();
            entityToBinding.clear();
        }
        const SceneECS::SceneFeatureSnapshotStoreCounts snapshots =
            runtime->GetFeatureSnapshotCounts();
        return released && bindings.empty() && snapshots.particleCount == 0;
    }

    SceneECS::SceneEcsRuntime* runtime = nullptr;
    IParticleEcsGateway* gateway = nullptr;
    ECS::SceneRuntimeId sceneRuntimeId;
    ECS::StructuralJournalCursor structuralCursor;
    SceneECS::CleanupRecordCursor cleanupCursor;
    std::vector<Binding> bindings;
    std::unordered_map<ECS::EntityHandle, uint32> entityToBinding;
    ParticleEcsBridgeRegistrationResult registration =
        ParticleEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0;
    uint64 cleanupContinuityLossCount = 0;
    uint64 authoritativeReconcileCount = 0;
    uint64 createdRuntimeCount = 0;
    uint64 releasedRuntimeCount = 0;
    uint64 rejectedConfigurationCount = 0;
    uint64 rejectedCommandCount = 0;
    uint64 synchronizationFailureCount = 0;
    uint64 shutdownPreparationCount = 0;
    uint64 shutdownReleaseFailureCount = 0;
    bool initialReconcileComplete = false;
    bool processorsEnabled = false;
};

ParticleEcsBridge::ParticleEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                                     IParticleEcsGateway& gateway)
    : m_state(std::make_shared<State>(runtime, gateway))
{
}

ParticleEcsBridge::~ParticleEcsBridge() = default;

ParticleEcsBridgeRegistrationResult ParticleEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->gateway == nullptr ||
        !m_state->sceneRuntimeId.IsValid())
    {
        return ParticleEcsBridgeRegistrationResult::InvalidRuntime;
    }
    if (m_state->registration == ParticleEcsBridgeRegistrationResult::Registered)
    {
        return ParticleEcsBridgeRegistrationResult::AlreadyRegistered;
    }

    const std::string prefix = "ParticleEcsBridge." +
                               std::to_string(m_state->sceneRuntimeId.GetValue()) + ".";
    const std::weak_ptr<State> weakState = m_state;
    const auto makeDescriptor = [weakState, prefix](
                                    std::string suffix,
                                    ECS::ProcessorPhase phase,
                                    std::vector<std::type_index> reads,
                                    std::vector<std::type_index> writes,
                                    std::function<void(State&, ECS::ProcessorExecutionContext&)> run)
    {
        ECS::ProcessorDescriptor descriptor;
        descriptor.name = prefix + std::move(suffix);
        descriptor.phase = phase;
        descriptor.stepMode = ECS::ProcessorStepMode::Variable;
        descriptor.access.reads = std::move(reads);
        descriptor.access.writes = std::move(writes);
        descriptor.access.resourceWrites = {std::type_index(typeid(IParticleEcsGateway))};
        descriptor.runWithContext = [weakState, run = std::move(run)](
                                        ECS::ProcessorExecutionContext& context)
        {
            if (const std::shared_ptr<State> state = weakState.lock();
                state != nullptr && state->processorsEnabled)
            {
                run(*state, context);
            }
        };
        return descriptor;
    };

    try
    {
        const std::vector<std::type_index> reads = {
            std::type_index(typeid(ParticleEcsConfig)),
            std::type_index(typeid(ParticleEcsIntent)),
            std::type_index(typeid(ParticleEcsState)),
            std::type_index(typeid(SceneECS::ParticleRuntimeTag)),
            std::type_index(typeid(SceneECS::RenderWorldTransform)),
            std::type_index(typeid(SceneECS::Active)),
            std::type_index(typeid(SceneECS::EntityLifecycleState)),
        };
        std::vector<ECS::ProcessorDescriptor> descriptors;
        descriptors.reserve(2u);
        descriptors.push_back(makeDescriptor(
            "Feature", ECS::ProcessorPhase::Feature, reads,
            {std::type_index(typeid(ParticleEcsState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(makeDescriptor(
            "EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup, reads,
            {std::type_index(typeid(ParticleEcsState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Cleanup(context);
            }));
        const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors));
        m_state->processorsEnabled = registered;
        m_state->registration = registered ? ParticleEcsBridgeRegistrationResult::Registered :
                                             ParticleEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    }
    catch (...)
    {
        m_state->registration = ParticleEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    }
    return m_state->registration;
}

bool ParticleEcsBridge::IsRegistered() const
{
    return m_state != nullptr &&
           m_state->registration == ParticleEcsBridgeRegistrationResult::Registered;
}

ECS::SceneRuntimeId ParticleEcsBridge::GetSceneRuntimeId() const
{
    return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{};
}

bool ParticleEcsBridge::PrepareForShutdown()
{
    return m_state != nullptr && m_state->runtime != nullptr && m_state->gateway != nullptr &&
           m_state->PrepareForShutdown();
}

ParticleEcsRuntimeHandle ParticleEcsBridge::FindRuntime(
    ECS::SceneRuntimeId sceneRuntimeId,
    ECS::EntityHandle entity) const
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->gateway == nullptr ||
        sceneRuntimeId != m_state->sceneRuntimeId ||
        !m_state->runtime->GetRegistry().IsAlive(entity))
    {
        return ParticleEcsRuntimeHandle::Invalid();
    }
    const uint32 index = m_state->FindBinding(entity);
    if (index == RVX_INVALID_INDEX)
    {
        return ParticleEcsRuntimeHandle::Invalid();
    }
    const ParticleEcsRuntimeHandle runtime = m_state->bindings[index].runtime;
    return runtime.IsValid() && m_state->gateway->IsAlive(runtime) ? runtime :
                                                                     ParticleEcsRuntimeHandle::Invalid();
}

ParticleEcsBridgeDiagnosticsSnapshot ParticleEcsBridge::GetDiagnosticsSnapshot() const
{
    ParticleEcsBridgeDiagnosticsSnapshot snapshot;
    if (m_state == nullptr)
    {
        return snapshot;
    }
    snapshot.sceneRuntimeId = m_state->sceneRuntimeId;
    snapshot.registration = m_state->registration;
    snapshot.bindingCount = static_cast<uint32>(m_state->bindings.size());
    snapshot.publishedSnapshotCount = m_state->runtime != nullptr ?
                                          m_state->runtime->GetFeatureSnapshotCounts().particleCount : 0;
    snapshot.structuralContinuityLossCount = m_state->structuralContinuityLossCount;
    snapshot.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount;
    snapshot.authoritativeReconcileCount = m_state->authoritativeReconcileCount;
    snapshot.createdRuntimeCount = m_state->createdRuntimeCount;
    snapshot.releasedRuntimeCount = m_state->releasedRuntimeCount;
    snapshot.rejectedConfigurationCount = m_state->rejectedConfigurationCount;
    snapshot.rejectedCommandCount = m_state->rejectedCommandCount;
    snapshot.synchronizationFailureCount = m_state->synchronizationFailureCount;
    snapshot.shutdownPreparationCount = m_state->shutdownPreparationCount;
    snapshot.shutdownReleaseFailureCount = m_state->shutdownReleaseFailureCount;
    snapshot.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings)
    {
        snapshot.bindings.push_back({
            .entity = binding.entity,
            .runtime = binding.runtime,
            .status = binding.status,
            .configurationRevision = binding.configurationRevision,
            .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement,
        });
        if (binding.runtime.IsValid())
        {
            ++snapshot.outstandingRuntimeCount;
        }
        if (binding.awaitingCleanupAcknowledgement)
        {
            ++snapshot.pendingCleanupCount;
        }
    }
    std::sort(snapshot.bindings.begin(), snapshot.bindings.end(),
              [](const ParticleEcsBindingDiagnostic& lhs, const ParticleEcsBindingDiagnostic& rhs)
              {
                  return lhs.entity < rhs.entity;
              });
    return snapshot;
}
} // namespace RVX::Particle
