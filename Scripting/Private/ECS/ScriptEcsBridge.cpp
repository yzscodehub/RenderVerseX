#include "Scripting/ECS/ScriptEcsBridge.h"

#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace RVX::Scripting
{
namespace
{
    [[nodiscard]] bool IsKnownCommand(SceneECS::ScriptExecutionCommand command)
    {
        return command >= SceneECS::ScriptExecutionCommand::None &&
               command <= SceneECS::ScriptExecutionCommand::Reload;
    }

    [[nodiscard]] bool RequiresScriptCleanup(const SceneECS::EntityLifecycleState* lifecycle)
    {
        return lifecycle != nullptr &&
               (lifecycle->requiredCleanupDomains &
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Script)) != 0;
    }

    template<typename... Args>
    [[nodiscard]] bool CallOptional(sol::table& instance, const char* name, Args&&... args)
    {
        sol::object object = instance[name];
        if (!object.valid() || object.get_type() == sol::type::lua_nil)
        {
            return true;
        }
        if (!object.is<sol::protected_function>())
        {
            return false;
        }

        sol::protected_function function = object.as<sol::protected_function>();
        sol::protected_function_result result =
            function(instance, std::forward<Args>(args)...);
        return result.valid();
    }
} // namespace

// =============================================================================
// ScriptingSubsystem production gateway
// =============================================================================

struct ScriptEcsEngineGateway::Program
{
    AssetId assetId{};
    uint64 sourceRevision = 0;
    ScriptHandle scriptHandle = InvalidScriptHandle;
    std::filesystem::path relativePath;
    sol::table classTable;
};

struct ScriptEcsEngineGateway::Entry
{
    uint32 generation = 1;
    bool allocated = false;
    bool started = false;
    ECS::SceneRuntimeId sceneRuntimeId;
    ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
    Program program;
    sol::table instance;
};

ScriptEcsEngineGateway::ScriptEcsEngineGateway(::RVX::ScriptingSubsystem& subsystem,
                                               ScriptEcsSourceResolver sourceResolver)
    : m_subsystem(&subsystem)
    , m_sourceResolver(std::move(sourceResolver))
    , m_ownerThread(std::this_thread::get_id())
{
}

ScriptEcsEngineGateway::~ScriptEcsEngineGateway()
{
    // VM calls from a foreign destructor thread would be a data race. Hosts
    // must use PrepareForShutdown on m_ownerThread before tearing down Lua.
    if (IsOwnerThread())
    {
        static_cast<void>(PrepareForShutdown());
    }
}

bool ScriptEcsEngineGateway::IsOwnerThread() const
{
    return std::this_thread::get_id() == m_ownerThread;
}

ScriptEcsEngineGateway::Entry* ScriptEcsEngineGateway::FindEntry(ScriptEcsInstanceHandle instance)
{
    if (!instance.IsValid() || instance.GetIndex() >= m_entries.size())
    {
        return nullptr;
    }

    Entry& entry = m_entries[instance.GetIndex()];
    return entry.allocated && entry.generation == instance.GetGeneration() ? &entry : nullptr;
}

const ScriptEcsEngineGateway::Entry* ScriptEcsEngineGateway::FindEntry(
    ScriptEcsInstanceHandle instance) const
{
    if (!instance.IsValid() || instance.GetIndex() >= m_entries.size())
    {
        return nullptr;
    }

    const Entry& entry = m_entries[instance.GetIndex()];
    return entry.allocated && entry.generation == instance.GetGeneration() ? &entry : nullptr;
}

std::optional<ScriptEcsEngineGateway::Program> ScriptEcsEngineGateway::ResolveProgram(
    AssetId assetId,
    bool forceReload)
{
    if (!IsOwnerThread() || m_subsystem == nullptr || !m_subsystem->GetLuaState().IsInitialized() ||
        !m_sourceResolver || !assetId.IsValid())
    {
        return std::nullopt;
    }

    const std::optional<ScriptEcsSource> source = m_sourceResolver(assetId);
    if (!source.has_value() || source->relativePath.empty() || source->sourceRevision == 0)
    {
        return std::nullopt;
    }

    const ScriptHandle handle = m_subsystem->LoadScript(source->relativePath);
    if (handle == InvalidScriptHandle)
    {
        return std::nullopt;
    }
    if (forceReload && !m_subsystem->ReloadScript(handle))
    {
        return std::nullopt;
    }

    const CachedScript* cached = m_subsystem->GetScript(handle);
    if (cached == nullptr || !cached->isValid)
    {
        return std::nullopt;
    }

    // Compile, then execute the candidate chunk only to obtain its explicitly
    // returned class table. There is intentionally no global-name fallback.
    sol::load_result loaded = m_subsystem->GetLuaState().LoadString(
        cached->source, cached->filePath.string());
    if (!loaded.valid())
    {
        return std::nullopt;
    }
    sol::protected_function chunk = loaded;
    sol::protected_function_result execution = chunk();
    if (!execution.valid() || execution.return_count() != 1 ||
        !execution.get<sol::object>().is<sol::table>())
    {
        return std::nullopt;
    }

    return Program{
        .assetId = assetId,
        .sourceRevision = source->sourceRevision,
        .scriptHandle = handle,
        .relativePath = source->relativePath,
        .classTable = execution.get<sol::table>(),
    };
}

bool ScriptEcsEngineGateway::Retire(ScriptEcsInstanceHandle instance)
{
    if (!IsOwnerThread())
    {
        return false;
    }

    Entry* entry = FindEntry(instance);
    if (entry == nullptr)
    {
        return false;
    }

    try
    {
        if (m_freeEntries.size() == m_freeEntries.capacity())
        {
            m_freeEntries.reserve(m_freeEntries.size() + 1u);
        }
    }
    catch (...)
    {
        return false;
    }

    entry->instance = sol::table();
    entry->program = Program{};
    entry->entity = ECS::EntityHandle::Invalid();
    entry->sceneRuntimeId = ECS::SceneRuntimeId{};
    entry->started = false;
    entry->allocated = false;
    ++entry->generation;
    if (entry->generation == 0)
    {
        ++entry->generation;
    }
    m_freeEntries.push_back(instance.GetIndex());
    return true;
}

ScriptEcsInstanceHandle ScriptEcsEngineGateway::Create(const ScriptEcsProgramRequest& request)
{
    if (!IsOwnerThread() || request.configurationRevision == 0)
    {
        return ScriptEcsInstanceHandle::Invalid();
    }

    const std::optional<Program> program = ResolveProgram(request.scriptAssetId, true);
    if (!program.has_value())
    {
        return ScriptEcsInstanceHandle::Invalid();
    }

    sol::table instance = m_subsystem->GetState().create_table();
    sol::table metatable = m_subsystem->GetState().create_table();
    metatable["__index"] = program->classTable;
    instance[sol::metatable_key] = metatable;

    uint32 index = RVX_INVALID_INDEX;
    try
    {
        if (m_freeEntries.empty())
        {
            if (m_entries.size() == m_entries.capacity())
            {
                m_entries.reserve(m_entries.size() + 1u);
            }
            index = static_cast<uint32>(m_entries.size());
            m_entries.push_back({});
        }
        else
        {
            index = m_freeEntries.back();
            m_freeEntries.pop_back();
        }
    }
    catch (...)
    {
        return ScriptEcsInstanceHandle::Invalid();
    }

    Entry& entry = m_entries[index];
    entry.allocated = true;
    entry.started = false;
    entry.program = *program;
    entry.instance = instance;
    return ScriptEcsInstanceHandle::Create(index, entry.generation);
}

bool ScriptEcsEngineGateway::Start(ScriptEcsInstanceHandle instance,
                                   ECS::SceneRuntimeId sceneRuntimeId,
                                   ECS::EntityHandle entity)
{
    if (!IsOwnerThread() || !sceneRuntimeId.IsValid() || !entity.IsValid())
    {
        return false;
    }

    Entry* entry = FindEntry(instance);
    if (entry == nullptr || !entry->instance.valid())
    {
        return false;
    }
    if (entry->started)
    {
        return entry->sceneRuntimeId == sceneRuntimeId && entry->entity == entity;
    }

    entry->instance["scene_runtime_id"] = sceneRuntimeId.GetValue();
    entry->instance["entity_handle"] = entity.GetPackedValue();
    if (!CallOptional(entry->instance, "OnStart"))
    {
        return false;
    }
    entry->sceneRuntimeId = sceneRuntimeId;
    entry->entity = entity;
    entry->started = true;
    return true;
}

bool ScriptEcsEngineGateway::Update(ScriptEcsInstanceHandle instance, float deltaTime)
{
    if (!IsOwnerThread() || !std::isfinite(deltaTime))
    {
        return false;
    }

    Entry* entry = FindEntry(instance);
    return entry != nullptr && entry->started && entry->instance.valid() &&
           CallOptional(entry->instance, "OnUpdate", deltaTime);
}

ScriptEcsStopResult ScriptEcsEngineGateway::Stop(ScriptEcsInstanceHandle instance)
{
    if (!IsOwnerThread())
    {
        return ScriptEcsStopResult::Failed;
    }

    Entry* entry = FindEntry(instance);
    if (entry == nullptr)
    {
        return ScriptEcsStopResult::AlreadyAbsent;
    }
    if (entry->started && !CallOptional(entry->instance, "OnDestroy"))
    {
        return ScriptEcsStopResult::Failed;
    }
    return Retire(instance) ? ScriptEcsStopResult::Stopped : ScriptEcsStopResult::Failed;
}

bool ScriptEcsEngineGateway::Reload(ScriptEcsInstanceHandle instance)
{
    if (!IsOwnerThread())
    {
        return false;
    }

    Entry* entry = FindEntry(instance);
    if (entry == nullptr)
    {
        return false;
    }

    const std::optional<Program> candidate = ResolveProgram(entry->program.assetId, true);
    if (!candidate.has_value())
    {
        return false; // Existing entry/table remains completely retained.
    }

    sol::table candidateInstance = m_subsystem->GetState().create_table();
    sol::table metatable = m_subsystem->GetState().create_table();
    metatable["__index"] = candidate->classTable;
    candidateInstance[sol::metatable_key] = metatable;

    if (entry->started)
    {
        candidateInstance["scene_runtime_id"] = entry->sceneRuntimeId.GetValue();
        candidateInstance["entity_handle"] = entry->entity.GetPackedValue();
        if (!CallOptional(candidateInstance, "OnStart"))
        {
            return false;
        }
        if (!CallOptional(entry->instance, "OnDestroy"))
        {
            static_cast<void>(CallOptional(candidateInstance, "OnDestroy"));
            return false;
        }
    }

    entry->program = *candidate;
    entry->instance = candidateInstance;
    return true;
}

bool ScriptEcsEngineGateway::IsInstanceAlive(ScriptEcsInstanceHandle instance) const
{
    if (!IsOwnerThread())
    {
        return false;
    }

    const Entry* entry = FindEntry(instance);
    return entry != nullptr && entry->instance.valid();
}

bool ScriptEcsEngineGateway::PrepareForShutdown()
{
    if (!IsOwnerThread())
    {
        return false;
    }

    std::vector<ScriptEcsInstanceHandle> instances;
    instances.reserve(m_entries.size());
    for (uint32 index = 0; index < m_entries.size(); ++index)
    {
        const Entry& entry = m_entries[index];
        if (entry.allocated)
        {
            instances.push_back(ScriptEcsInstanceHandle::Create(index, entry.generation));
        }
    }
    std::sort(instances.begin(), instances.end());
    bool drained = true;
    for (const ScriptEcsInstanceHandle instance : instances)
    {
        if (Stop(instance) == ScriptEcsStopResult::Failed)
        {
            drained = false;
        }
    }
    return drained;
}

// =============================================================================
// Pure ECS bridge
// =============================================================================

struct ScriptEcsBridge::State
{
    struct Binding
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        ScriptEcsInstanceHandle instance = ScriptEcsInstanceHandle::Invalid();
        AssetId scriptAssetId{};
        uint64 configurationRevision = 0;
        bool started = false;
        bool awaitingCleanupAcknowledgement = false;
        SceneECS::ScriptBindingStatus status = SceneECS::ScriptBindingStatus::Unbound;
    };

    State(SceneECS::SceneEcsRuntime& runtimeIn, IScriptEcsExecutionGateway& gatewayIn)
        : runtime(&runtimeIn)
        , gateway(&gatewayIn)
        , sceneRuntimeId(runtimeIn.GetSceneRuntimeId())
        , ownerThread(std::this_thread::get_id())
        , structuralCursor(runtimeIn.GetRegistry().GetStructuralJournal().CreateCursor())
    {
    }

    [[nodiscard]] bool IsOwnerThread() const
    {
        return std::this_thread::get_id() == ownerThread && runtime != nullptr &&
               runtime->GetRegistry().IsOwnerThread();
    }

    [[nodiscard]] uint32 FindBindingIndex(ECS::EntityHandle entity) const
    {
        const auto found = entityToBinding.find(entity);
        return found != entityToBinding.end() ? found->second : RVX_INVALID_INDEX;
    }

    [[nodiscard]] bool AddBinding(ECS::EntityHandle entity, uint32& outIndex)
    {
        const uint32 existing = FindBindingIndex(entity);
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
        const ECS::EntityHandle entity = bindings[index].entity;
        entityToBinding.erase(entity);
        const uint32 last = static_cast<uint32>(bindings.size() - 1u);
        if (index != last)
        {
            bindings[index] = std::move(bindings[last]);
            entityToBinding[bindings[index].entity] = index;
        }
        bindings.pop_back();
    }

    void WriteState(ECS::Registry& registry,
                    ECS::EntityHandle entity,
                    SceneECS::ScriptBindingStatus status,
                    std::optional<uint64> appliedRevision = std::nullopt,
                    std::optional<uint64> consumedSequence = std::nullopt)
    {
        if (!registry.Has<SceneECS::ScriptExecutionState>(entity))
        {
            return;
        }
        registry.Write<SceneECS::ScriptExecutionState>(
            entity,
            [status, appliedRevision, consumedSequence](SceneECS::ScriptExecutionState& state)
            {
                state.status = status;
                if (appliedRevision.has_value())
                {
                    state.lastAppliedConfigurationRevision = *appliedRevision;
                }
                if (consumedSequence.has_value() &&
                    *consumedSequence > state.lastConsumedCommandSequence)
                {
                    state.lastConsumedCommandSequence = *consumedSequence;
                }
                ++state.synchronizationRevision;
            });
    }

    [[nodiscard]] bool StopInstance(Binding& binding)
    {
        if (!binding.instance.IsValid())
        {
            return true;
        }
        const ScriptEcsStopResult result = gateway->Stop(binding.instance);
        if (result == ScriptEcsStopResult::Failed)
        {
            ++releaseFailureCount;
            return false;
        }
        if (result == ScriptEcsStopResult::Stopped)
        {
            ++stopCount;
        }
        binding.instance = ScriptEcsInstanceHandle::Invalid();
        binding.started = false;
        return true;
    }

    [[nodiscard]] bool ReleaseBinding(ECS::Registry& registry,
                                      ECS::EntityHandle entity,
                                      bool retainCleanupEvidence,
                                      SceneECS::ScriptBindingStatus releasedStatus)
    {
        const uint32 index = FindBindingIndex(entity);
        if (index == RVX_INVALID_INDEX)
        {
            return true;
        }
        Binding& binding = bindings[index];
        if (!StopInstance(binding))
        {
            return false;
        }
        if (retainCleanupEvidence)
        {
            binding.awaitingCleanupAcknowledgement = true;
            binding.status = SceneECS::ScriptBindingStatus::PendingDestroy;
            WriteState(registry, entity, binding.status);
            return true;
        }
        WriteState(registry, entity, releasedStatus);
        EraseBinding(index);
        return true;
    }

    [[nodiscard]] bool CreateInstance(ECS::Registry& registry,
                                      ECS::EntityHandle entity,
                                      Binding& binding,
                                      const SceneECS::ScriptBehaviour& behaviour)
    {
        const ScriptEcsInstanceHandle instance = gateway->Create({
            .scriptAssetId = behaviour.scriptAssetId,
            .configurationRevision = behaviour.configurationRevision,
        });
        if (!instance.IsValid() || !gateway->IsInstanceAlive(instance))
        {
            if (instance.IsValid() && gateway->Stop(instance) == ScriptEcsStopResult::Failed)
            {
                ++releaseFailureCount;
            }
            ++rejectedConfigurationCount;
            binding.status = SceneECS::ScriptBindingStatus::ProgramRejected;
            WriteState(registry, entity, binding.status);
            return false;
        }
        binding.instance = instance;
        binding.scriptAssetId = behaviour.scriptAssetId;
        binding.configurationRevision = behaviour.configurationRevision;
        binding.started = false;
        binding.status = SceneECS::ScriptBindingStatus::Loaded;
        ++createCount;
        WriteState(registry, entity, binding.status, behaviour.configurationRevision);
        return true;
    }

    [[nodiscard]] bool StartInstance(ECS::Registry& registry,
                                     ECS::EntityHandle entity,
                                     Binding& binding,
                                     const SceneECS::ScriptBehaviour& behaviour,
                                     std::optional<uint64> consumedSequence = std::nullopt)
    {
        if (!binding.instance.IsValid() || !gateway->IsInstanceAlive(binding.instance))
        {
            binding.status = SceneECS::ScriptBindingStatus::InvocationFailed;
            ++invocationFailureCount;
            WriteState(registry, entity, binding.status);
            return false;
        }
        if (!binding.started && !gateway->Start(binding.instance, sceneRuntimeId, entity))
        {
            binding.status = SceneECS::ScriptBindingStatus::InvocationFailed;
            ++invocationFailureCount;
            WriteState(registry, entity, binding.status);
            return false;
        }
        if (!binding.started)
        {
            ++startCount;
        }
        binding.started = true;
        binding.status = SceneECS::ScriptBindingStatus::Running;
        WriteState(registry, entity, binding.status, behaviour.configurationRevision, consumedSequence);
        return true;
    }

    [[nodiscard]] bool ReplaceForConfiguration(ECS::ProcessorExecutionContext& context,
                                               ECS::EntityHandle entity,
                                               Binding& binding,
                                               const SceneECS::ScriptBehaviour& behaviour)
    {
        const ScriptEcsInstanceHandle candidate = gateway->Create({
            .scriptAssetId = behaviour.scriptAssetId,
            .configurationRevision = behaviour.configurationRevision,
        });
        if (!candidate.IsValid() || !gateway->IsInstanceAlive(candidate))
        {
            if (candidate.IsValid())
            {
                static_cast<void>(gateway->Stop(candidate));
            }
            ++rejectedConfigurationCount;
            binding.status = SceneECS::ScriptBindingStatus::ProgramRejected;
            WriteState(context.registry, entity, binding.status);
            return false; // Old binding remains live and owned.
        }

        const bool shouldStart = binding.started || behaviour.startOnLoad;
        if (shouldStart && !gateway->Start(candidate, sceneRuntimeId, entity))
        {
            static_cast<void>(gateway->Stop(candidate));
            ++invocationFailureCount;
            binding.status = SceneECS::ScriptBindingStatus::InvocationFailed;
            WriteState(context.registry, entity, binding.status);
            return false;
        }
        if (!StopInstance(binding))
        {
            static_cast<void>(gateway->Stop(candidate));
            context.ReportFailure("Script instance release failed during configuration replacement.");
            return false;
        }

        binding.instance = candidate;
        binding.scriptAssetId = behaviour.scriptAssetId;
        binding.configurationRevision = behaviour.configurationRevision;
        binding.started = shouldStart;
        binding.status = shouldStart ? SceneECS::ScriptBindingStatus::Running :
                                      SceneECS::ScriptBindingStatus::Loaded;
        ++createCount;
        if (shouldStart)
        {
            ++startCount;
        }
        WriteState(context.registry, entity, binding.status, behaviour.configurationRevision);
        return true;
    }

    void ProcessEligible(ECS::ProcessorExecutionContext& context,
                         ECS::EntityHandle entity,
                         const SceneECS::ScriptBehaviour& behaviour,
                         const SceneECS::ScriptExecutionIntent& intent,
                         const SceneECS::ScriptExecutionState& executionState)
    {
        if (!behaviour.scriptAssetId.IsValid() || behaviour.configurationRevision == 0)
        {
            ++rejectedConfigurationCount;
            WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::InvalidConfiguration);
            return;
        }

        uint32 index = FindBindingIndex(entity);
        const auto ensureBinding = [this, entity, &index]()
        {
            return index != RVX_INVALID_INDEX || AddBinding(entity, index);
        };

        if (index != RVX_INVALID_INDEX)
        {
            Binding& binding = bindings[index];
            if (!binding.instance.IsValid() || !gateway->IsInstanceAlive(binding.instance))
            {
                if (!StopInstance(binding))
                {
                    context.ReportFailure("Script instance retirement failed after liveness loss.");
                    return;
                }
                EraseBinding(index);
                index = RVX_INVALID_INDEX;
            }
            else if (binding.scriptAssetId != behaviour.scriptAssetId ||
                     binding.configurationRevision != behaviour.configurationRevision)
            {
                if (!ReplaceForConfiguration(context, entity, binding, behaviour) ||
                    context.HasReportedFailure())
                {
                    return;
                }
            }
        }

        if (intent.command != SceneECS::ScriptExecutionCommand::None &&
            intent.sequence > executionState.lastConsumedCommandSequence)
        {
            if (!IsKnownCommand(intent.command) || intent.sequence == 0)
            {
                ++rejectedCommandCount;
                WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::InvalidConfiguration);
                return;
            }

            if (intent.command == SceneECS::ScriptExecutionCommand::Start)
            {
                if (!ensureBinding())
                {
                    ++rejectedConfigurationCount;
                    WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::ProgramRejected);
                    return;
                }
                Binding& binding = bindings[index];
                if (!binding.instance.IsValid() && !CreateInstance(context.registry, entity, binding, behaviour))
                {
                    return;
                }
                static_cast<void>(StartInstance(context.registry, entity, binding, behaviour, intent.sequence));
                return;
            }

            if (intent.command == SceneECS::ScriptExecutionCommand::Stop)
            {
                if (index != RVX_INVALID_INDEX &&
                    !ReleaseBinding(context.registry, entity, false,
                                    SceneECS::ScriptBindingStatus::Stopped))
                {
                    context.ReportFailure("Script instance stop failed for ECS stop command.");
                    return;
                }
                WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::Stopped,
                           std::nullopt, intent.sequence);
                return;
            }

            // A Reload command creates a missing program, but deliberately does
            // not synthesize Start when no instance had ever run.
            if (!ensureBinding())
            {
                ++rejectedConfigurationCount;
                WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::ProgramRejected);
                return;
            }
            Binding& binding = bindings[index];
            if (!binding.instance.IsValid())
            {
                if (!CreateInstance(context.registry, entity, binding, behaviour))
                {
                    return;
                }
                WriteState(context.registry, entity, binding.status, behaviour.configurationRevision,
                           intent.sequence);
                return;
            }
            if (!gateway->Reload(binding.instance))
            {
                ++invocationFailureCount;
                binding.status = SceneECS::ScriptBindingStatus::InvocationFailed;
                WriteState(context.registry, entity, binding.status);
                return;
            }
            ++reloadCount;
            binding.status = binding.started ? SceneECS::ScriptBindingStatus::Running :
                                               SceneECS::ScriptBindingStatus::Loaded;
            WriteState(context.registry, entity, binding.status, behaviour.configurationRevision,
                       intent.sequence);
            return;
        }

        if (index == RVX_INVALID_INDEX && behaviour.startOnLoad)
        {
            if (!ensureBinding())
            {
                ++rejectedConfigurationCount;
                WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::ProgramRejected);
                return;
            }
            Binding& binding = bindings[index];
            if (!CreateInstance(context.registry, entity, binding, behaviour))
            {
                return;
            }
            static_cast<void>(StartInstance(context.registry, entity, binding, behaviour));
            return;
        }

        if (index == RVX_INVALID_INDEX)
        {
            WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::Unbound);
            return;
        }

        Binding& binding = bindings[index];
        if (!binding.started)
        {
            binding.status = SceneECS::ScriptBindingStatus::Loaded;
            WriteState(context.registry, entity, binding.status, behaviour.configurationRevision);
            return;
        }
        if (!gateway->Update(binding.instance, static_cast<float>(context.deltaSeconds)))
        {
            ++invocationFailureCount;
            binding.status = SceneECS::ScriptBindingStatus::InvocationFailed;
            WriteState(context.registry, entity, binding.status);
            return;
        }
        binding.status = SceneECS::ScriptBindingStatus::Running;
        WriteState(context.registry, entity, binding.status, behaviour.configurationRevision);
    }

    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        if (!IsOwnerThread())
        {
            context.ReportFailure("Script ECS bridge called from a non-owner thread.");
            return;
        }
        const ECS::StructuralJournalRead read =
            context.registry.ReadStructuralChanges(structuralCursor);
        if (read.continuity == ECS::StructuralJournalContinuity::Lost)
        {
            ++structuralContinuityLossCount;
        }
        if (!initialReconcileComplete || read.continuity == ECS::StructuralJournalContinuity::Lost)
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
            const SceneECS::EntityLifecycleState* lifecycle =
                context.registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            const bool retainCleanup = RequiresScriptCleanup(lifecycle) &&
                                       lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive;
            const bool hasScript = context.registry.IsAlive(entity) &&
                                   context.registry.TryGet<SceneECS::ScriptBehaviour>(entity) != nullptr;
            if (hasScript && lifecycle != nullptr &&
                lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive)
            {
                continue;
            }
            if (!ReleaseBinding(context.registry, entity, retainCleanup,
                                SceneECS::ScriptBindingStatus::Stopped))
            {
                context.ReportFailure("Script instance release failed during ECS reconciliation.");
                return;
            }
        }

        std::vector<ECS::EntityHandle> entities;
        context.registry.Query<ECS::Read<SceneECS::ScriptBehaviour>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::ScriptBehaviour&)
            {
                entities.push_back(entity);
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                context.registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            const SceneECS::Active* active = context.registry.TryGet<SceneECS::Active>(entity);
            const SceneECS::ScriptBehaviour* behaviour =
                context.registry.TryGet<SceneECS::ScriptBehaviour>(entity);
            const SceneECS::ScriptExecutionIntent* intent =
                context.registry.TryGet<SceneECS::ScriptExecutionIntent>(entity);
            const SceneECS::ScriptExecutionState* state =
                context.registry.TryGet<SceneECS::ScriptExecutionState>(entity);
            if (lifecycle == nullptr || lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
                behaviour == nullptr || intent == nullptr || state == nullptr)
            {
                continue;
            }
            if (!context.registry.IsEnabled(entity) ||
                !context.registry.IsFragmentEnabled<SceneECS::ScriptBehaviour>(entity) ||
                !context.registry.IsFragmentEnabled<SceneECS::ScriptExecutionIntent>(entity) ||
                !context.registry.IsFragmentEnabled<SceneECS::ScriptExecutionState>(entity) ||
                active == nullptr || !active->value ||
                !context.registry.IsFragmentEnabled<SceneECS::Active>(entity))
            {
                const uint32 index = FindBindingIndex(entity);
                if (index != RVX_INVALID_INDEX)
                {
                    bindings[index].status = SceneECS::ScriptBindingStatus::Paused;
                }
                WriteState(context.registry, entity, SceneECS::ScriptBindingStatus::Paused);
                continue;
            }
            ProcessEligible(context, entity, *behaviour, *intent, *state);
            if (context.HasReportedFailure())
            {
                return;
            }
        }
    }

    void AcknowledgeRetainedCleanup(ECS::ProcessorExecutionContext& context)
    {
        std::vector<ECS::EntityHandle> entities;
        context.registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired &&
                    (lifecycle.requiredCleanupDomains &
                     SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Script)) != 0)
                {
                    entities.push_back(entity);
                }
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                context.registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            if (!RequiresScriptCleanup(lifecycle) ||
                lifecycle->phase != SceneECS::EntityLifecyclePhase::CleanupRequired)
            {
                continue;
            }
            if (!ReleaseBinding(context.registry, entity, true,
                                SceneECS::ScriptBindingStatus::PendingDestroy))
            {
                context.ReportFailure("Script instance release failed while retrying cleanup acknowledgement.");
                return;
            }
            if (!runtime->AcknowledgeCleanup(
                    entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Script)))
            {
                context.ReportFailure("Script cleanup acknowledgement was rejected.");
                return;
            }
            const uint32 index = FindBindingIndex(entity);
            if (index != RVX_INVALID_INDEX)
            {
                EraseBinding(index);
            }
        }
    }

    void Cleanup(ECS::ProcessorExecutionContext& context)
    {
        if (!IsOwnerThread())
        {
            context.ReportFailure("Script ECS cleanup called from a non-owner thread.");
            return;
        }
        AcknowledgeRetainedCleanup(context);
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
                AcknowledgeRetainedCleanup(context);
            }
            return;
        }

        for (const SceneECS::CleanupRecord& record : read.records)
        {
            if (record.sceneRuntimeId != sceneRuntimeId ||
                (record.requiredCleanupDomains &
                 SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Script)) == 0)
            {
                continue;
            }
            if (!ReleaseBinding(context.registry, record.entity, true,
                                SceneECS::ScriptBindingStatus::PendingDestroy))
            {
                context.ReportFailure("Script instance release failed during cleanup.");
                return;
            }
        }
        AcknowledgeRetainedCleanup(context);
    }

    [[nodiscard]] bool PrepareForShutdown()
    {
        if (!IsOwnerThread() || gateway == nullptr)
        {
            return false;
        }
        ++shutdownPreparationCount;
        std::vector<ECS::EntityHandle> entities;
        entities.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            entities.push_back(binding.entity);
        }
        std::sort(entities.begin(), entities.end());
        bool drained = true;
        for (const ECS::EntityHandle entity : entities)
        {
            const uint32 index = FindBindingIndex(entity);
            if (index == RVX_INVALID_INDEX)
            {
                continue;
            }
            if (!StopInstance(bindings[index]))
            {
                ++shutdownFailureCount;
                drained = false;
                continue;
            }
            EraseBinding(index);
        }
        if (!gateway->PrepareForShutdown())
        {
            ++shutdownFailureCount;
            drained = false;
        }
        if (drained)
        {
            processorsEnabled = false;
        }
        return drained;
    }

    SceneECS::SceneEcsRuntime* runtime = nullptr;
    IScriptEcsExecutionGateway* gateway = nullptr;
    ECS::SceneRuntimeId sceneRuntimeId;
    std::thread::id ownerThread;
    ECS::StructuralJournalCursor structuralCursor;
    SceneECS::CleanupRecordCursor cleanupCursor;
    std::vector<Binding> bindings;
    std::unordered_map<ECS::EntityHandle, uint32> entityToBinding;
    ScriptEcsBridgeRegistrationResult registration =
        ScriptEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0;
    uint64 cleanupContinuityLossCount = 0;
    uint64 authoritativeReconcileCount = 0;
    uint64 createCount = 0;
    uint64 startCount = 0;
    uint64 stopCount = 0;
    uint64 reloadCount = 0;
    uint64 rejectedConfigurationCount = 0;
    uint64 rejectedCommandCount = 0;
    uint64 invocationFailureCount = 0;
    uint64 releaseFailureCount = 0;
    uint64 shutdownPreparationCount = 0;
    uint64 shutdownFailureCount = 0;
    bool initialReconcileComplete = false;
    bool processorsEnabled = false;
};

ScriptEcsBridge::ScriptEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                                 IScriptEcsExecutionGateway& gateway)
    : m_state(std::make_shared<State>(runtime, gateway))
{
}

ScriptEcsBridge::~ScriptEcsBridge() = default;

ScriptEcsBridgeRegistrationResult ScriptEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->gateway == nullptr ||
        !m_state->sceneRuntimeId.IsValid())
    {
        return ScriptEcsBridgeRegistrationResult::InvalidRuntime;
    }
    if (!m_state->IsOwnerThread())
    {
        return ScriptEcsBridgeRegistrationResult::ThreadAffinityViolation;
    }
    if (m_state->registration == ScriptEcsBridgeRegistrationResult::Registered)
    {
        return ScriptEcsBridgeRegistrationResult::AlreadyRegistered;
    }

    const std::string prefix = "ScriptEcsBridge." +
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
        descriptor.access.resourceWrites = {std::type_index(typeid(IScriptEcsExecutionGateway))};
        descriptor.runWithContext = [weakState, run = std::move(run)](
                                        ECS::ProcessorExecutionContext& context)
        {
            if (const std::shared_ptr<State> state = weakState.lock())
            {
                if (state->processorsEnabled)
                {
                    run(*state, context);
                }
            }
        };
        return descriptor;
    };

    std::vector<ECS::ProcessorDescriptor> descriptors;
    try
    {
        descriptors.reserve(2u);
        const std::vector<std::type_index> reads = {
            std::type_index(typeid(SceneECS::ScriptBehaviour)),
            std::type_index(typeid(SceneECS::ScriptExecutionIntent)),
            std::type_index(typeid(SceneECS::ScriptExecutionState)),
            std::type_index(typeid(SceneECS::Active)),
            std::type_index(typeid(SceneECS::EntityLifecycleState)),
        };
        descriptors.push_back(makeDescriptor(
            "Gameplay", ECS::ProcessorPhase::Gameplay, reads,
            {std::type_index(typeid(SceneECS::ScriptExecutionState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(makeDescriptor(
            "EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup, reads,
            {std::type_index(typeid(SceneECS::ScriptExecutionState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Cleanup(context);
            }));
    }
    catch (...)
    {
        m_state->registration = ScriptEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
        return m_state->registration;
    }

    const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors));
    m_state->processorsEnabled = registered;
    m_state->registration = registered ? ScriptEcsBridgeRegistrationResult::Registered :
                                         ScriptEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    return m_state->registration;
}

bool ScriptEcsBridge::IsRegistered() const
{
    return m_state != nullptr &&
           m_state->registration == ScriptEcsBridgeRegistrationResult::Registered;
}

ECS::SceneRuntimeId ScriptEcsBridge::GetSceneRuntimeId() const
{
    return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{};
}

bool ScriptEcsBridge::PrepareForShutdown()
{
    return m_state != nullptr && m_state->PrepareForShutdown();
}

ScriptEcsInstanceHandle ScriptEcsBridge::FindInstance(ECS::SceneRuntimeId sceneRuntimeId,
                                                       ECS::EntityHandle entity) const
{
    if (m_state == nullptr || !m_state->IsOwnerThread() || m_state->runtime == nullptr ||
        m_state->gateway == nullptr ||
        sceneRuntimeId != m_state->sceneRuntimeId || !m_state->runtime->GetRegistry().IsAlive(entity))
    {
        return ScriptEcsInstanceHandle::Invalid();
    }
    const uint32 index = m_state->FindBindingIndex(entity);
    if (index == RVX_INVALID_INDEX)
    {
        return ScriptEcsInstanceHandle::Invalid();
    }
    const ScriptEcsInstanceHandle instance = m_state->bindings[index].instance;
    return instance.IsValid() && m_state->gateway->IsInstanceAlive(instance) ?
               instance :
               ScriptEcsInstanceHandle::Invalid();
}

ScriptEcsBridgeDiagnosticsSnapshot ScriptEcsBridge::GetDiagnosticsSnapshot() const
{
    ScriptEcsBridgeDiagnosticsSnapshot snapshot;
    if (m_state == nullptr)
    {
        return snapshot;
    }
    if (!m_state->IsOwnerThread())
    {
        return snapshot;
    }
    snapshot.sceneRuntimeId = m_state->sceneRuntimeId;
    snapshot.registration = m_state->registration;
    snapshot.structuralContinuityLossCount = m_state->structuralContinuityLossCount;
    snapshot.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount;
    snapshot.authoritativeReconcileCount = m_state->authoritativeReconcileCount;
    snapshot.createCount = m_state->createCount;
    snapshot.startCount = m_state->startCount;
    snapshot.stopCount = m_state->stopCount;
    snapshot.reloadCount = m_state->reloadCount;
    snapshot.rejectedConfigurationCount = m_state->rejectedConfigurationCount;
    snapshot.rejectedCommandCount = m_state->rejectedCommandCount;
    snapshot.invocationFailureCount = m_state->invocationFailureCount;
    snapshot.releaseFailureCount = m_state->releaseFailureCount;
    snapshot.shutdownPreparationCount = m_state->shutdownPreparationCount;
    snapshot.shutdownFailureCount = m_state->shutdownFailureCount;
    snapshot.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings)
    {
        const SceneECS::ScriptExecutionState* executionState =
            m_state->runtime != nullptr ?
                m_state->runtime->GetRegistry().TryGet<SceneECS::ScriptExecutionState>(binding.entity) :
                nullptr;
        snapshot.bindings.push_back({
            .entity = binding.entity,
            .instance = binding.instance,
            .status = binding.status,
            .lastConsumedCommandSequence = executionState != nullptr ?
                                               executionState->lastConsumedCommandSequence :
                                               0,
            .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement,
        });
        if (binding.instance.IsValid() && m_state->gateway != nullptr &&
            m_state->gateway->IsInstanceAlive(binding.instance))
        {
            ++snapshot.activeInstanceCount;
        }
        if (binding.instance.IsValid())
        {
            ++snapshot.outstandingInstanceCount;
        }
        if (binding.awaitingCleanupAcknowledgement)
        {
            ++snapshot.pendingCleanupCount;
        }
    }
    std::sort(snapshot.bindings.begin(), snapshot.bindings.end(),
              [](const ScriptEcsBindingDiagnostic& lhs, const ScriptEcsBindingDiagnostic& rhs)
              {
                  return lhs.entity < rhs.entity;
              });
    return snapshot;
}
} // namespace RVX::Scripting
