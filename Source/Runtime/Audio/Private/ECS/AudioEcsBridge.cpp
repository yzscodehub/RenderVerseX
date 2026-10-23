#include "Audio/ECS/AudioEcsBridge.h"

#include "Audio/AudioSubsystem.h"
#include "ECS/Query.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace RVX::Audio
{
namespace
{
    [[nodiscard]] bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFinite(const Vec3& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Mat4& matrix)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!IsFinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsKnownAttenuationMode(SceneECS::AudioAttenuationMode value)
    {
        return value >= SceneECS::AudioAttenuationMode::None &&
               value <= SceneECS::AudioAttenuationMode::ExponentialDistance;
    }

    [[nodiscard]] bool IsKnownPlaybackCommand(SceneECS::AudioPlaybackCommand value)
    {
        return value >= SceneECS::AudioPlaybackCommand::None &&
               value <= SceneECS::AudioPlaybackCommand::Stop;
    }

    [[nodiscard]] bool IsValidEmitter(const SceneECS::AudioEmitter& emitter)
    {
        return emitter.audioAssetId.IsValid() && emitter.configurationRevision != 0 &&
               IsKnownAttenuationMode(emitter.attenuation) &&
               IsFinite(emitter.volume) && emitter.volume >= 0.0f &&
               IsFinite(emitter.pitch) && emitter.pitch > 0.0f &&
               IsFinite(emitter.minDistance) && emitter.minDistance > 0.0f &&
               IsFinite(emitter.maxDistance) && emitter.maxDistance >= emitter.minDistance &&
               IsFinite(emitter.rolloffFactor) && emitter.rolloffFactor >= 0.0f &&
               IsFinite(emitter.coneInnerAngleDegrees) &&
               emitter.coneInnerAngleDegrees >= 0.0f &&
               emitter.coneInnerAngleDegrees <= 360.0f &&
               IsFinite(emitter.coneOuterAngleDegrees) &&
               emitter.coneOuterAngleDegrees >= emitter.coneInnerAngleDegrees &&
               emitter.coneOuterAngleDegrees <= 360.0f &&
               IsFinite(emitter.coneOuterGain) && emitter.coneOuterGain >= 0.0f &&
               emitter.coneOuterGain <= 1.0f;
    }

    [[nodiscard]] Vec3 ExtractTranslation(const Mat4& matrix)
    {
        return {matrix[3][0], matrix[3][1], matrix[3][2]};
    }

    [[nodiscard]] AttenuationModel ToAudioAttenuationMode(SceneECS::AudioAttenuationMode value)
    {
        switch (value)
        {
            case SceneECS::AudioAttenuationMode::None:
                return AttenuationModel::None;
            case SceneECS::AudioAttenuationMode::Linear:
                return AttenuationModel::Linear;
            case SceneECS::AudioAttenuationMode::Inverse:
                return AttenuationModel::Inverse;
            case SceneECS::AudioAttenuationMode::ExponentialDistance:
                return AttenuationModel::ExponentialDistance;
        }

        return AttenuationModel::Inverse;
    }

    [[nodiscard]] bool RequiresAudioCleanup(const SceneECS::EntityLifecycleState* lifecycle)
    {
        return lifecycle != nullptr &&
               (lifecycle->requiredCleanupDomains &
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)) != 0;
    }
} // namespace

// =============================================================================
// AudioEngine production gateway
// =============================================================================

struct AudioEcsEngineGateway::Entry
{
    AudioHandle engineHandle;
    uint32 generation = 1;
    bool allocated = false;
};

AudioEcsEngineGateway::AudioEcsEngineGateway(AudioEngine& engine,
                                             AudioEcsClipResolver clipResolver)
    : m_engine(&engine)
    , m_clipResolver(std::move(clipResolver))
    , m_ownerThread(std::this_thread::get_id())
{
}

AudioEcsEngineGateway::AudioEcsEngineGateway(AudioSubsystem& subsystem,
                                             AudioEcsClipResolver clipResolver)
    : AudioEcsEngineGateway(subsystem.GetEngine(), std::move(clipResolver))
{
}

AudioEcsEngineGateway::~AudioEcsEngineGateway()
{
    // Cross-thread destruction must not issue unsynchronized AudioEngine calls.
    // Hosts are required to drain on m_ownerThread through AudioEcsBridge before
    // destroying this gateway, so this remains only a same-thread last resort.
    if (m_engine == nullptr || !IsOwnerThread())
    {
        return;
    }

    for (Entry& entry : m_entries)
    {
        if (entry.allocated && entry.engineHandle.IsValid())
        {
            m_engine->Stop(entry.engineHandle);
        }
    }
}

AudioEcsEngineGateway::Entry* AudioEcsEngineGateway::FindEntry(AudioEcsPlaybackHandle handle)
{
    if (!handle.IsValid() || handle.GetIndex() >= m_entries.size())
    {
        return nullptr;
    }

    Entry& entry = m_entries[handle.GetIndex()];
    return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
}

const AudioEcsEngineGateway::Entry* AudioEcsEngineGateway::FindEntry(
    AudioEcsPlaybackHandle handle) const
{
    if (!handle.IsValid() || handle.GetIndex() >= m_entries.size())
    {
        return nullptr;
    }

    const Entry& entry = m_entries[handle.GetIndex()];
    return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
}

bool AudioEcsEngineGateway::IsOwnerThread() const
{
    return std::this_thread::get_id() == m_ownerThread;
}

bool AudioEcsEngineGateway::Retire(AudioEcsPlaybackHandle handle)
{
    if (!IsOwnerThread())
    {
        return false;
    }

    Entry* entry = FindEntry(handle);
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

    entry->engineHandle = AudioHandle{};
    entry->allocated = false;
    ++entry->generation;
    if (entry->generation == 0)
    {
        ++entry->generation;
    }
    m_freeEntries.push_back(handle.GetIndex());
    return true;
}

AudioEcsPlaybackHandle AudioEcsEngineGateway::Play(const AudioEcsPlayRequest& request)
{
    if (!IsOwnerThread() || m_engine == nullptr || !m_engine->IsInitialized() || !m_clipResolver ||
        !request.audioAssetId.IsValid() || request.audioAssetId != request.emitter.audioAssetId ||
        !IsValidEmitter(request.emitter) || !IsFinite(request.worldPosition) ||
        (request.emitter.busId != 0 && !m_engine->HasBus(request.emitter.busId)))
    {
        return AudioEcsPlaybackHandle::Invalid();
    }

    AudioClip::Ptr clip = m_clipResolver(request.audioAssetId);
    if (clip == nullptr || !clip->IsLoaded())
    {
        return AudioEcsPlaybackHandle::Invalid();
    }

    AudioPlaySettings playSettings;
    playSettings.volume = request.emitter.volume;
    playSettings.pitch = request.emitter.pitch;
    playSettings.loop = request.emitter.loop;
    playSettings.busId = request.emitter.busId;

    AudioHandle engineHandle;
    if (request.emitter.spatialize)
    {
        Audio3DSettings spatialSettings;
        spatialSettings.position = request.worldPosition;
        spatialSettings.minDistance = request.emitter.minDistance;
        spatialSettings.maxDistance = request.emitter.maxDistance;
        spatialSettings.rolloffFactor = request.emitter.rolloffFactor;
        spatialSettings.attenuationModel = ToAudioAttenuationMode(request.emitter.attenuation);
        spatialSettings.coneInnerAngle = request.emitter.coneInnerAngleDegrees;
        spatialSettings.coneOuterAngle = request.emitter.coneOuterAngleDegrees;
        spatialSettings.coneOuterGain = request.emitter.coneOuterGain;
        engineHandle = m_engine->Play3D(clip, spatialSettings, playSettings);
    }
    else
    {
        engineHandle = m_engine->Play(clip, playSettings);
    }

    if (!engineHandle.IsValid())
    {
        return AudioEcsPlaybackHandle::Invalid();
    }

    uint32 entryIndex = RVX_INVALID_INDEX;
    try
    {
        if (m_freeEntries.empty())
        {
            if (m_entries.size() == m_entries.capacity())
            {
                m_entries.reserve(m_entries.size() + 1u);
            }
            entryIndex = static_cast<uint32>(m_entries.size());
            m_entries.push_back({});
        }
        else
        {
            entryIndex = m_freeEntries.back();
            m_freeEntries.pop_back();
        }
    }
    catch (...)
    {
        m_engine->Stop(engineHandle);
        return AudioEcsPlaybackHandle::Invalid();
    }

    Entry& entry = m_entries[entryIndex];
    entry.engineHandle = engineHandle;
    entry.allocated = true;
    return AudioEcsPlaybackHandle::Create(entryIndex, entry.generation);
}

bool AudioEcsEngineGateway::Update(AudioEcsPlaybackHandle handle,
                                   const AudioEcsPlaybackUpdate& update)
{
    if (!IsOwnerThread())
    {
        return false;
    }

    Entry* entry = FindEntry(handle);
    if (m_engine == nullptr || entry == nullptr || !m_engine->IsInitialized() ||
        !entry->engineHandle.IsValid() || !m_engine->IsPlaying(entry->engineHandle) ||
        !IsValidEmitter(update.emitter) || !IsFinite(update.worldPosition))
    {
        return false;
    }

    m_engine->SetVolume(entry->engineHandle, update.emitter.volume);
    m_engine->SetPitch(entry->engineHandle, update.emitter.pitch);
    m_engine->SetLooping(entry->engineHandle, update.emitter.loop);
    if (update.emitter.spatialize)
    {
        m_engine->SetPosition(entry->engineHandle, update.worldPosition);
    }
    return m_engine->IsPlaying(entry->engineHandle);
}

AudioEcsStopResult AudioEcsEngineGateway::Stop(AudioEcsPlaybackHandle handle)
{
    if (!IsOwnerThread())
    {
        return AudioEcsStopResult::Failed;
    }

    Entry* entry = FindEntry(handle);
    if (entry == nullptr)
    {
        return AudioEcsStopResult::AlreadyAbsent;
    }
    if (m_engine == nullptr || !entry->engineHandle.IsValid())
    {
        return AudioEcsStopResult::Failed;
    }
    if (!m_engine->IsPlaying(entry->engineHandle))
    {
        return Retire(handle) ? AudioEcsStopResult::AlreadyAbsent : AudioEcsStopResult::Failed;
    }

    m_engine->Stop(entry->engineHandle);
    if (m_engine->IsPlaying(entry->engineHandle))
    {
        return AudioEcsStopResult::Failed;
    }
    return Retire(handle) ? AudioEcsStopResult::Stopped : AudioEcsStopResult::Failed;
}

bool AudioEcsEngineGateway::IsPlaybackAlive(AudioEcsPlaybackHandle handle) const
{
    if (!IsOwnerThread())
    {
        return false;
    }

    const Entry* entry = FindEntry(handle);
    return m_engine != nullptr && entry != nullptr && entry->engineHandle.IsValid() &&
           m_engine->IsInitialized() && m_engine->IsPlaying(entry->engineHandle);
}

// =============================================================================
// Pure ECS bridge
// =============================================================================

struct AudioEcsBridge::State
{
    struct Binding
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        AudioEcsPlaybackHandle playback = AudioEcsPlaybackHandle::Invalid();
        AssetId audioAssetId{};
        /** @brief Exact producer revision successfully used by the current playback. */
        uint64 configurationRevision = 0;
        uint32 busId = 0;
        bool spatialize = true;
        bool playOnStartConsumed = false;
        bool awaitingCleanupAcknowledgement = false;
        SceneECS::AudioBindingStatus status = SceneECS::AudioBindingStatus::Unbound;
    };

    State(SceneECS::SceneEcsRuntime& runtimeIn, IAudioEcsPlaybackGateway& playbackGatewayIn)
        : runtime(&runtimeIn)
        , playbackGateway(&playbackGatewayIn)
        , sceneRuntimeId(runtimeIn.GetSceneRuntimeId())
        , structuralCursor(runtimeIn.GetRegistry().GetStructuralJournal().CreateCursor())
    {
    }

    ~State()
    {
        if (playbackGateway == nullptr)
        {
            return;
        }

        std::vector<AudioEcsPlaybackHandle> activeHandles;
        activeHandles.reserve(bindings.size());
        for (const Binding& binding : bindings)
        {
            if (binding.playback.IsValid())
            {
                activeHandles.push_back(binding.playback);
            }
        }
        std::sort(activeHandles.begin(), activeHandles.end());
        for (const AudioEcsPlaybackHandle handle : activeHandles)
        {
            static_cast<void>(playbackGateway->Stop(handle));
        }
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

        const ECS::EntityHandle removedEntity = bindings[index].entity;
        entityToBinding.erase(removedEntity);
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
                    SceneECS::AudioBindingStatus status,
                    std::optional<uint64> appliedConfigurationRevision = std::nullopt,
                    std::optional<uint64> consumedCommandSequence = std::nullopt)
    {
        if (!registry.Has<SceneECS::AudioPlaybackState>(entity))
        {
            return;
        }

        registry.Write<SceneECS::AudioPlaybackState>(
            entity,
            [status, appliedConfigurationRevision, consumedCommandSequence](
                SceneECS::AudioPlaybackState& target)
            {
                target.status = status;
                if (appliedConfigurationRevision.has_value())
                {
                    target.lastAppliedConfigurationRevision = *appliedConfigurationRevision;
                }
                if (consumedCommandSequence.has_value() &&
                    *consumedCommandSequence > target.lastConsumedCommandSequence)
                {
                    target.lastConsumedCommandSequence = *consumedCommandSequence;
                }
                ++target.synchronizationRevision;
            });
    }

    [[nodiscard]] bool StopPlayback(Binding& binding)
    {
        if (!binding.playback.IsValid())
        {
            return true;
        }

        const AudioEcsStopResult stopResult = playbackGateway->Stop(binding.playback);
        if (stopResult == AudioEcsStopResult::Failed)
        {
            return false;
        }
        if (stopResult == AudioEcsStopResult::Stopped)
        {
            ++stopCount;
        }
        binding.playback = AudioEcsPlaybackHandle::Invalid();
        return true;
    }

    [[nodiscard]] uint32 GetOutstandingPlaybackCount() const
    {
        return static_cast<uint32>(std::count_if(
            bindings.begin(), bindings.end(),
            [](const Binding& binding) { return binding.playback.IsValid(); }));
    }

    [[nodiscard]] bool PrepareForShutdown()
    {
        ++shutdownPreparationCount;
        bool drained = true;
        for (Binding& binding : bindings)
        {
            if (!StopPlayback(binding))
            {
                ++shutdownStopFailureCount;
                binding.status = SceneECS::AudioBindingStatus::SynchronizationFailed;
                drained = false;
                continue;
            }
            binding.status = SceneECS::AudioBindingStatus::Stopped;
        }
        return drained && GetOutstandingPlaybackCount() == 0;
    }

    [[nodiscard]] bool ReleaseBinding(ECS::Registry& registry,
                                      ECS::EntityHandle entity,
                                      bool retainCleanupEvidence,
                                      SceneECS::AudioBindingStatus releasedStatus)
    {
        const uint32 index = FindBindingIndex(entity);
        if (index == RVX_INVALID_INDEX)
        {
            return true;
        }

        Binding& binding = bindings[index];
        if (!StopPlayback(binding))
        {
            return false;
        }

        if (retainCleanupEvidence)
        {
            binding.awaitingCleanupAcknowledgement = true;
            binding.status = SceneECS::AudioBindingStatus::PendingDestroy;
            WriteState(registry, entity, binding.status);
            return true;
        }

        WriteState(registry, entity, releasedStatus);
        EraseBinding(index);
        return true;
    }

    enum class StartPlaybackResult : uint8
    {
        Started = 0,
        Rejected,
        ReleaseFailed,
    };

    [[nodiscard]] StartPlaybackResult StartPlayback(
        ECS::Registry& registry,
        ECS::EntityHandle entity,
        Binding& binding,
        const SceneECS::AudioEmitter& emitter,
        const Vec3& worldPosition,
        std::optional<uint64> consumedCommandSequence = std::nullopt)
    {
        const AudioEcsPlaybackHandle playback = playbackGateway->Play({
            .audioAssetId = emitter.audioAssetId,
            .emitter = emitter,
            .worldPosition = worldPosition,
        });
        if (!playback.IsValid() || !playbackGateway->IsPlaybackAlive(playback))
        {
            // A valid handle establishes ownership even when its liveness proof
            // fails. Retain it until Stop proves release, otherwise a gateway
            // slot or backend object could be orphaned behind a false success.
            if (playback.IsValid())
            {
                binding.playback = playback;
                if (!StopPlayback(binding))
                {
                    ++playbackReleaseFailureCount;
                    binding.status = SceneECS::AudioBindingStatus::SynchronizationFailed;
                    WriteState(registry, entity, binding.status);
                    return StartPlaybackResult::ReleaseFailed;
                }
            }
            ++rejectedConfigurationCount;
            binding.status = SceneECS::AudioBindingStatus::PlaybackRejected;
            WriteState(registry, entity, binding.status);
            return StartPlaybackResult::Rejected;
        }

        binding.playback = playback;
        binding.audioAssetId = emitter.audioAssetId;
        binding.configurationRevision = emitter.configurationRevision;
        binding.busId = emitter.busId;
        binding.spatialize = emitter.spatialize;
        binding.status = SceneECS::AudioBindingStatus::Active;
        ++playCount;
        WriteState(registry, entity, binding.status, emitter.configurationRevision,
                   consumedCommandSequence);
        return StartPlaybackResult::Started;
    }

    struct Eligibility
    {
        const SceneECS::AudioEmitter* emitter = nullptr;
        const SceneECS::AudioPlaybackIntent* intent = nullptr;
        const SceneECS::AudioPlaybackState* playbackState = nullptr;
        const SceneECS::RenderWorldTransform* transform = nullptr;
        const SceneECS::EntityLifecycleState* lifecycle = nullptr;
        SceneECS::AudioBindingStatus failure = SceneECS::AudioBindingStatus::Unbound;
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
            return result;
        }

        result.emitter = registry.TryGet<SceneECS::AudioEmitter>(entity);
        result.intent = registry.TryGet<SceneECS::AudioPlaybackIntent>(entity);
        result.playbackState = registry.TryGet<SceneECS::AudioPlaybackState>(entity);
        result.transform = registry.TryGet<SceneECS::RenderWorldTransform>(entity);
        const SceneECS::Active* active = registry.TryGet<SceneECS::Active>(entity);
        if (result.emitter == nullptr || result.intent == nullptr || result.playbackState == nullptr ||
            result.transform == nullptr || active == nullptr ||
            !registry.IsEnabled(entity) ||
            !registry.IsFragmentEnabled<SceneECS::AudioEmitter>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::AudioPlaybackIntent>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::AudioPlaybackState>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::RenderWorldTransform>(entity) ||
            !registry.IsFragmentEnabled<SceneECS::Active>(entity))
        {
            result.failure = SceneECS::AudioBindingStatus::Inactive;
            return result;
        }
        if (!active->value)
        {
            result.failure = SceneECS::AudioBindingStatus::Inactive;
            return result;
        }
        if (!IsValidEmitter(*result.emitter) || !IsFinite(result.transform->matrix) ||
            !IsFinite(ExtractTranslation(result.transform->matrix)))
        {
            result.failure = SceneECS::AudioBindingStatus::InvalidConfiguration;
            return result;
        }

        result.eligible = true;
        return result;
    }

    void ProcessEligible(ECS::ProcessorExecutionContext& context,
                         ECS::EntityHandle entity,
                         const Eligibility& eligibility)
    {
        ECS::Registry& registry = context.registry;
        const SceneECS::AudioEmitter& emitter = *eligibility.emitter;
        const SceneECS::AudioPlaybackIntent& intent = *eligibility.intent;
        const SceneECS::AudioPlaybackState& playbackState = *eligibility.playbackState;
        const Vec3 worldPosition = ExtractTranslation(eligibility.transform->matrix);

        uint32 bindingIndex = FindBindingIndex(entity);
        const auto ensureBinding = [this, entity, &bindingIndex]()
        {
            return bindingIndex != RVX_INVALID_INDEX || AddBinding(entity, bindingIndex);
        };

        bool attemptedNewCommand = false;
        // None is deliberately a no-op even when a producer retains an older
        // sequence value. The sequence belongs to explicit Play/Stop effects,
        // not to clearing an intent after it has been observed.
        if (intent.command != SceneECS::AudioPlaybackCommand::None)
        {
            if (!IsKnownPlaybackCommand(intent.command) || intent.sequence == 0)
            {
                ++rejectedCommandCount;
                WriteState(registry, entity, SceneECS::AudioBindingStatus::InvalidConfiguration);
                return;
            }

            if (intent.sequence > playbackState.lastConsumedCommandSequence)
            {
                attemptedNewCommand = true;
                if (!ensureBinding())
                {
                    ++rejectedConfigurationCount;
                    WriteState(registry, entity, SceneECS::AudioBindingStatus::PlaybackRejected);
                    return;
                }

                Binding& binding = bindings[bindingIndex];
                if (intent.command == SceneECS::AudioPlaybackCommand::Play)
                {
                    if (!StopPlayback(binding))
                    {
                        context.ReportFailure("Audio playback stop failed before replay command.");
                        return;
                    }
                    binding.playOnStartConsumed = true;
                    const StartPlaybackResult startResult = StartPlayback(
                        registry, entity, binding, emitter, worldPosition, intent.sequence);
                    if (startResult == StartPlaybackResult::ReleaseFailed)
                    {
                        context.ReportFailure(
                            "Audio playback release failed after a rejected ECS play command.");
                        return;
                    }
                    if (startResult != StartPlaybackResult::Started)
                    {
                        return;
                    }
                }
                else
                {
                    if (!StopPlayback(binding))
                    {
                        context.ReportFailure("Audio playback stop failed for ECS stop command.");
                        return;
                    }
                    binding.playOnStartConsumed = true;
                    binding.status = SceneECS::AudioBindingStatus::Stopped;
                    WriteState(registry, entity, binding.status, std::nullopt,
                               intent.sequence);
                    if (!emitter.playOnStart)
                    {
                        EraseBinding(bindingIndex);
                        bindingIndex = RVX_INVALID_INDEX;
                    }
                }
            }
        }

        if (!attemptedNewCommand && emitter.playOnStart)
        {
            if (!ensureBinding())
            {
                ++rejectedConfigurationCount;
                WriteState(registry, entity, SceneECS::AudioBindingStatus::PlaybackRejected);
                return;
            }
            Binding& binding = bindings[bindingIndex];
            if (!binding.playback.IsValid() && !binding.playOnStartConsumed)
            {
                binding.playOnStartConsumed = true;
                const StartPlaybackResult startResult =
                    StartPlayback(registry, entity, binding, emitter, worldPosition);
                if (startResult == StartPlaybackResult::ReleaseFailed)
                {
                    context.ReportFailure(
                        "Audio playback release failed after a rejected play-on-start request.");
                    return;
                }
            }
        }

        if (bindingIndex == RVX_INVALID_INDEX)
        {
            WriteState(registry, entity, SceneECS::AudioBindingStatus::Unbound);
            return;
        }

        Binding& binding = bindings[bindingIndex];
        if (binding.playback.IsValid() && !playbackGateway->IsPlaybackAlive(binding.playback))
        {
            // Let the gateway retire its generation slot after proving that the
            // underlying playback is absent. Dropping the bridge handle first
            // would leak an AudioEngine gateway entry after natural completion.
            if (!StopPlayback(binding))
            {
                context.ReportFailure("Audio playback retirement failed after absence detection.");
                return;
            }
            binding.status = SceneECS::AudioBindingStatus::Stopped;
            WriteState(registry, entity, binding.status);
            return;
        }

        if (!binding.playback.IsValid())
        {
            if (binding.status != SceneECS::AudioBindingStatus::PlaybackRejected)
            {
                binding.status = SceneECS::AudioBindingStatus::Stopped;
                WriteState(registry, entity, binding.status);
            }
            return;
        }

        if (binding.audioAssetId != emitter.audioAssetId ||
            binding.configurationRevision != emitter.configurationRevision ||
            binding.busId != emitter.busId || binding.spatialize != emitter.spatialize)
        {
            if (!StopPlayback(binding))
            {
                context.ReportFailure("Audio playback stop failed during ECS configuration restart.");
                return;
            }
            const StartPlaybackResult startResult =
                StartPlayback(registry, entity, binding, emitter, worldPosition);
            if (startResult == StartPlaybackResult::ReleaseFailed)
            {
                context.ReportFailure(
                    "Audio playback release failed after a rejected ECS configuration restart.");
                return;
            }
            if (startResult != StartPlaybackResult::Started)
            {
                return;
            }
        }

        if (!playbackGateway->Update(binding.playback, {
                .emitter = emitter,
                .worldPosition = worldPosition,
            }))
        {
            ++synchronizationFailureCount;
            binding.status = SceneECS::AudioBindingStatus::SynchronizationFailed;
            if (!StopPlayback(binding))
            {
                context.ReportFailure("Audio playback release failed after ECS synchronization failure.");
                return;
            }
            WriteState(registry, entity, binding.status);
            return;
        }

        binding.status = SceneECS::AudioBindingStatus::Active;
        WriteState(registry, entity, binding.status);
    }

    void Reconcile(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        const ECS::StructuralJournalRead read = registry.ReadStructuralChanges(structuralCursor);
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
            const Eligibility eligibility = GetEligibility(registry, entity);
            if (eligibility.eligible)
            {
                continue;
            }

            const bool retainForCleanup = RequiresAudioCleanup(eligibility.lifecycle) &&
                                          eligibility.lifecycle->phase !=
                                              SceneECS::EntityLifecyclePhase::Alive;
            if (!ReleaseBinding(registry, entity, retainForCleanup, eligibility.failure))
            {
                context.ReportFailure("Audio playback release failed during ECS reconciliation.");
                return;
            }
        }

        std::vector<ECS::EntityHandle> entities;
        registry.Query<ECS::Read<SceneECS::AudioEmitter>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity, const SceneECS::AudioEmitter&)
            {
                entities.push_back(entity);
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const Eligibility eligibility = GetEligibility(registry, entity);
            if (!eligibility.eligible)
            {
                if (eligibility.failure == SceneECS::AudioBindingStatus::InvalidConfiguration)
                {
                    ++rejectedConfigurationCount;
                }
                WriteState(registry, entity, eligibility.failure);
                continue;
            }

            ProcessEligible(context, entity, eligibility);
            if (context.HasReportedFailure())
            {
                return;
            }
        }
    }

    void AcknowledgeRetainedCleanup(ECS::ProcessorExecutionContext& context)
    {
        ECS::Registry& registry = context.registry;
        std::vector<ECS::EntityHandle> entities;
        registry.Query<ECS::Read<SceneECS::EntityLifecycleState>>().EachIncludingAllDisabled(
            [&entities](ECS::EntityHandle entity,
                        const SceneECS::EntityLifecycleState& lifecycle)
            {
                if (lifecycle.phase == SceneECS::EntityLifecyclePhase::CleanupRequired &&
                    (lifecycle.requiredCleanupDomains &
                     SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)) != 0)
                {
                    entities.push_back(entity);
                }
            });
        std::sort(entities.begin(), entities.end());
        for (const ECS::EntityHandle entity : entities)
        {
            const SceneECS::EntityLifecycleState* lifecycle =
                registry.TryGet<SceneECS::EntityLifecycleState>(entity);
            if (!RequiresAudioCleanup(lifecycle) ||
                lifecycle->phase != SceneECS::EntityLifecyclePhase::CleanupRequired)
            {
                continue;
            }

            const uint32 index = FindBindingIndex(entity);
            if (index != RVX_INVALID_INDEX &&
                !ReleaseBinding(registry, entity, true,
                                SceneECS::AudioBindingStatus::PendingDestroy))
            {
                context.ReportFailure("Audio playback release failed while retrying cleanup acknowledgement.");
                return;
            }

            // Acknowledge only after Stop reported success/absence, or when no
            // side-table binding exists (which itself proves absence).
            if (!runtime->AcknowledgeCleanup(
                    entity, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)))
            {
                context.ReportFailure("Audio cleanup acknowledgement was rejected.");
                return;
            }
            const uint32 retainedIndex = FindBindingIndex(entity);
            if (retainedIndex != RVX_INVALID_INDEX)
            {
                EraseBinding(retainedIndex);
            }
        }
    }

    void Cleanup(ECS::ProcessorExecutionContext& context)
    {
        // Lifecycle state is durable evidence, so retry it before advancing the
        // independent cleanup cursor. A transient Stop failure therefore cannot
        // be hidden by a consumed record.
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
            if (context.HasReportedFailure())
            {
                return;
            }
            AcknowledgeRetainedCleanup(context);
            return;
        }

        for (const SceneECS::CleanupRecord& record : read.records)
        {
            if (record.sceneRuntimeId != sceneRuntimeId ||
                (record.requiredCleanupDomains &
                 SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Audio)) == 0)
            {
                continue;
            }
            if (!ReleaseBinding(context.registry, record.entity, true,
                                SceneECS::AudioBindingStatus::PendingDestroy))
            {
                context.ReportFailure("Audio playback release failed during cleanup.");
                return;
            }
        }
        AcknowledgeRetainedCleanup(context);
    }

    SceneECS::SceneEcsRuntime* runtime = nullptr;
    IAudioEcsPlaybackGateway* playbackGateway = nullptr;
    ECS::SceneRuntimeId sceneRuntimeId;
    ECS::StructuralJournalCursor structuralCursor;
    SceneECS::CleanupRecordCursor cleanupCursor;
    std::vector<Binding> bindings;
    std::unordered_map<ECS::EntityHandle, uint32> entityToBinding;
    AudioEcsBridgeRegistrationResult registration =
        AudioEcsBridgeRegistrationResult::InvalidRuntime;
    uint64 structuralContinuityLossCount = 0;
    uint64 cleanupContinuityLossCount = 0;
    uint64 authoritativeReconcileCount = 0;
    uint64 playCount = 0;
    uint64 stopCount = 0;
    uint64 rejectedConfigurationCount = 0;
    uint64 rejectedCommandCount = 0;
    uint64 synchronizationFailureCount = 0;
    uint64 playbackReleaseFailureCount = 0;
    uint64 shutdownPreparationCount = 0;
    uint64 shutdownStopFailureCount = 0;
    bool initialReconcileComplete = false;
    bool processorsEnabled = false;
};

AudioEcsBridge::AudioEcsBridge(SceneECS::SceneEcsRuntime& runtime,
                               IAudioEcsPlaybackGateway& playbackGateway)
    : m_state(std::make_shared<State>(runtime, playbackGateway))
{
}

AudioEcsBridge::~AudioEcsBridge() = default;

AudioEcsBridgeRegistrationResult AudioEcsBridge::RegisterProcessors()
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->playbackGateway == nullptr ||
        !m_state->sceneRuntimeId.IsValid())
    {
        return AudioEcsBridgeRegistrationResult::InvalidRuntime;
    }
    if (m_state->registration == AudioEcsBridgeRegistrationResult::Registered)
    {
        return AudioEcsBridgeRegistrationResult::AlreadyRegistered;
    }

    const std::string prefix = "AudioEcsBridge." +
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
        descriptor.access.resourceWrites = {std::type_index(typeid(IAudioEcsPlaybackGateway))};
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
        descriptors.push_back(makeDescriptor(
            "Feature", ECS::ProcessorPhase::Feature,
            {std::type_index(typeid(SceneECS::AudioEmitter)),
             std::type_index(typeid(SceneECS::AudioPlaybackIntent)),
             std::type_index(typeid(SceneECS::AudioPlaybackState)),
             std::type_index(typeid(SceneECS::RenderWorldTransform)),
             std::type_index(typeid(SceneECS::Active)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            {std::type_index(typeid(SceneECS::AudioPlaybackState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Reconcile(context);
            }));
        descriptors.push_back(makeDescriptor(
            "EndFrameCleanup", ECS::ProcessorPhase::EndFrameCleanup,
            // A lost cleanup cursor invokes the same authoritative reconcile as
            // Feature, so its declared reads include every fragment that path
            // may inspect rather than hiding that recovery dependency.
            {std::type_index(typeid(SceneECS::AudioEmitter)),
             std::type_index(typeid(SceneECS::AudioPlaybackIntent)),
             std::type_index(typeid(SceneECS::AudioPlaybackState)),
             std::type_index(typeid(SceneECS::RenderWorldTransform)),
             std::type_index(typeid(SceneECS::Active)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            {std::type_index(typeid(SceneECS::AudioPlaybackState)),
             std::type_index(typeid(SceneECS::EntityLifecycleState))},
            [](State& state, ECS::ProcessorExecutionContext& context)
            {
                state.Cleanup(context);
            }));
    }
    catch (...)
    {
        m_state->registration = AudioEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
        return m_state->registration;
    }

    const bool registered = m_state->runtime->RegisterProcessors(std::move(descriptors));
    m_state->processorsEnabled = registered;
    m_state->registration = registered ? AudioEcsBridgeRegistrationResult::Registered :
                                         AudioEcsBridgeRegistrationResult::ProcessorRegistrationFailed;
    return m_state->registration;
}

bool AudioEcsBridge::IsRegistered() const
{
    return m_state != nullptr &&
           m_state->registration == AudioEcsBridgeRegistrationResult::Registered;
}

ECS::SceneRuntimeId AudioEcsBridge::GetSceneRuntimeId() const
{
    return m_state != nullptr ? m_state->sceneRuntimeId : ECS::SceneRuntimeId{};
}

bool AudioEcsBridge::PrepareForShutdown()
{
    return m_state != nullptr && m_state->playbackGateway != nullptr &&
           m_state->PrepareForShutdown();
}

AudioEcsPlaybackHandle AudioEcsBridge::FindPlayback(ECS::SceneRuntimeId sceneRuntimeId,
                                                     ECS::EntityHandle entity) const
{
    if (m_state == nullptr || m_state->runtime == nullptr || m_state->playbackGateway == nullptr ||
        sceneRuntimeId != m_state->sceneRuntimeId ||
        !m_state->runtime->GetRegistry().IsAlive(entity))
    {
        return AudioEcsPlaybackHandle::Invalid();
    }

    const uint32 index = m_state->FindBindingIndex(entity);
    if (index == RVX_INVALID_INDEX)
    {
        return AudioEcsPlaybackHandle::Invalid();
    }

    const AudioEcsPlaybackHandle playback = m_state->bindings[index].playback;
    return playback.IsValid() && m_state->playbackGateway->IsPlaybackAlive(playback) ?
               playback :
               AudioEcsPlaybackHandle::Invalid();
}

AudioEcsBridgeDiagnosticsSnapshot AudioEcsBridge::GetDiagnosticsSnapshot() const
{
    AudioEcsBridgeDiagnosticsSnapshot snapshot;
    if (m_state == nullptr)
    {
        return snapshot;
    }

    snapshot.sceneRuntimeId = m_state->sceneRuntimeId;
    snapshot.registration = m_state->registration;
    snapshot.structuralContinuityLossCount = m_state->structuralContinuityLossCount;
    snapshot.cleanupContinuityLossCount = m_state->cleanupContinuityLossCount;
    snapshot.authoritativeReconcileCount = m_state->authoritativeReconcileCount;
    snapshot.playCount = m_state->playCount;
    snapshot.stopCount = m_state->stopCount;
    snapshot.rejectedConfigurationCount = m_state->rejectedConfigurationCount;
    snapshot.rejectedCommandCount = m_state->rejectedCommandCount;
    snapshot.synchronizationFailureCount = m_state->synchronizationFailureCount;
    snapshot.playbackReleaseFailureCount = m_state->playbackReleaseFailureCount;
    snapshot.shutdownPreparationCount = m_state->shutdownPreparationCount;
    snapshot.shutdownStopFailureCount = m_state->shutdownStopFailureCount;
    snapshot.bindings.reserve(m_state->bindings.size());
    for (const State::Binding& binding : m_state->bindings)
    {
        const SceneECS::AudioPlaybackState* playbackState =
            m_state->runtime != nullptr ?
                m_state->runtime->GetRegistry().TryGet<SceneECS::AudioPlaybackState>(binding.entity) :
                nullptr;
        snapshot.bindings.push_back({
            .entity = binding.entity,
            .playback = binding.playback,
            .status = binding.status,
            .lastConsumedCommandSequence = playbackState != nullptr ?
                                               playbackState->lastConsumedCommandSequence :
                                               0,
            .awaitingCleanupAcknowledgement = binding.awaitingCleanupAcknowledgement,
        });
        if (binding.playback.IsValid() && m_state->playbackGateway != nullptr &&
            m_state->playbackGateway->IsPlaybackAlive(binding.playback))
        {
            ++snapshot.activePlaybackCount;
        }
        if (binding.playback.IsValid())
        {
            ++snapshot.outstandingPlaybackCount;
        }
        if (binding.awaitingCleanupAcknowledgement)
        {
            ++snapshot.pendingCleanupCount;
        }
    }
    std::sort(snapshot.bindings.begin(), snapshot.bindings.end(),
              [](const AudioEcsBindingDiagnostic& lhs, const AudioEcsBindingDiagnostic& rhs)
              {
                  return lhs.entity < rhs.entity;
              });
    return snapshot;
}
} // namespace RVX::Audio
