#include "Particle/ECS/ParticleEcsRuntimeGateway.h"

#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticleSystemLoader.h"
#include "Resource/ResourceManager.h"

#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RVX::Particle
{
namespace
{
    constexpr const char* RVX_PARTICLE_ECS_RESOURCE_NOT_LOADED =
        "Particle ECS requires an already-loaded ParticleSystemResource; asynchronous loading is not started by the gateway.";
    constexpr const char* RVX_PARTICLE_ECS_RESIDENCY_SEAM_MISSING =
        "Particle ECS requires a published exact AssetKey to retain ParticleSystemResource residency; the current resource publication seam did not provide one.";
    constexpr const char* RVX_PARTICLE_ECS_CONFIGURATION_MISMATCH =
        "Particle ECS maxParticleCount must match the immutable loaded ParticleSystemResource.";

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

    [[nodiscard]] bool IsValidRequest(const ParticleEcsRuntimeRequest& request)
    {
        const ParticleEcsConfig& config = request.config;
        return config.systemAssetId.IsValid() && config.configurationRevision != 0 &&
               config.maxParticleCount != 0 && IsFinite(config.emissionRateScale) &&
               config.emissionRateScale >= 0.0f && IsFinite(config.simulationSpeed) &&
               config.simulationSpeed >= 0.0f && IsFinite(request.worldTransform) &&
               std::isfinite(request.deltaSeconds) && request.deltaSeconds >= 0.0;
    }

    [[nodiscard]] ParticleRenderSnapshotMode ToSnapshotRenderMode(ParticleRenderMode mode)
    {
        switch (mode)
        {
            case ParticleRenderMode::Billboard:
                return ParticleRenderSnapshotMode::Billboard;
            case ParticleRenderMode::StretchedBillboard:
                return ParticleRenderSnapshotMode::StretchedBillboard;
            case ParticleRenderMode::HorizontalBillboard:
                return ParticleRenderSnapshotMode::HorizontalBillboard;
            case ParticleRenderMode::VerticalBillboard:
                return ParticleRenderSnapshotMode::VerticalBillboard;
            case ParticleRenderMode::Mesh:
                return ParticleRenderSnapshotMode::Mesh;
            case ParticleRenderMode::Trail:
                return ParticleRenderSnapshotMode::Trail;
        }

        return ParticleRenderSnapshotMode::Billboard;
    }

    [[nodiscard]] ParticleRenderSnapshotBlendMode ToSnapshotBlendMode(ParticleBlendMode mode)
    {
        switch (mode)
        {
            case ParticleBlendMode::Additive:
                return ParticleRenderSnapshotBlendMode::Additive;
            case ParticleBlendMode::AlphaBlend:
                return ParticleRenderSnapshotBlendMode::AlphaBlend;
            case ParticleBlendMode::Multiply:
                return ParticleRenderSnapshotBlendMode::Multiply;
            case ParticleBlendMode::Premultiplied:
                return ParticleRenderSnapshotBlendMode::Premultiplied;
        }

        return ParticleRenderSnapshotBlendMode::AlphaBlend;
    }

    [[nodiscard]] ParticleRenderSnapshotSimulationBackend ToSnapshotSimulationBackend(
        const ParticleSystemInstance& instance)
    {
        if (!instance.IsSimulationSupported())
        {
            return ParticleRenderSnapshotSimulationBackend::None;
        }

        const std::string& backendName = instance.GetSimulationBackendName();
        if (backendName.find("GPU") != std::string::npos)
        {
            return ParticleRenderSnapshotSimulationBackend::GPU;
        }
        if (backendName.find("CPU") != std::string::npos)
        {
            return ParticleRenderSnapshotSimulationBackend::CPU;
        }
        return ParticleRenderSnapshotSimulationBackend::External;
    }
} // namespace

struct ParticleEcsRuntimeGateway::State
{
    struct Entry
    {
        Resource::ResourceHandle<ParticleSystemResource> resource;
        Resource::AssetResidencyLease residencyLease;
        std::unique_ptr<ParticleSystemInstance> instance;
        ParticleEcsConfig config;
        uint64 payloadRevision = 0;
        uint32 generation = 1;
        bool allocated = false;
    };

    explicit State(Resource::ResourceManager& manager)
        : resourceManager(&manager)
        , ownerThread(std::this_thread::get_id())
    {
    }

    [[nodiscard]] bool IsOwnerThread() const
    {
        return std::this_thread::get_id() == ownerThread;
    }

    [[nodiscard]] Entry* Find(ParticleEcsRuntimeHandle handle)
    {
        if (!handle.IsValid() || handle.GetIndex() >= entries.size())
        {
            return nullptr;
        }

        Entry& entry = entries[handle.GetIndex()];
        return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
    }

    [[nodiscard]] const Entry* Find(ParticleEcsRuntimeHandle handle) const
    {
        if (!handle.IsValid() || handle.GetIndex() >= entries.size())
        {
            return nullptr;
        }

        const Entry& entry = entries[handle.GetIndex()];
        return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
    }

    [[nodiscard]] bool BuildEntry(const ParticleEcsRuntimeRequest& request, Entry& outEntry)
    {
        if (resourceManager == nullptr || !IsValidRequest(request))
        {
            lastFailureReason = "Particle ECS gateway rejected an invalid particle runtime request.";
            return false;
        }

        Resource::ResourceHandle<ParticleSystemResource> resource =
            resourceManager->TryAcquireLoaded<ParticleSystemResource>(
                request.config.systemAssetId.value);
        if (!resource || !resource.IsLoaded() || resource.GetId() != request.config.systemAssetId.value)
        {
            lastFailureReason = RVX_PARTICLE_ECS_RESOURCE_NOT_LOADED;
            return false;
        }

        ParticleSystem::Ptr system = resource->GetSystem();
        if (!system || system->maxParticles != request.config.maxParticleCount)
        {
            lastFailureReason = RVX_PARTICLE_ECS_CONFIGURATION_MISMATCH;
            return false;
        }

        const std::optional<Resource::AssetKey> assetKey =
            resourceManager->FindPublishedAssetKey(resource.GetId());
        if (!assetKey.has_value())
        {
            lastFailureReason = RVX_PARTICLE_ECS_RESIDENCY_SEAM_MISSING;
            return false;
        }

        Resource::AssetResidencyLease residencyLease =
            resourceManager->AcquireAssetResidencyLease(*assetKey);
        if (!residencyLease.IsValid())
        {
            lastFailureReason = RVX_PARTICLE_ECS_RESIDENCY_SEAM_MISSING;
            return false;
        }

        auto instance = std::make_unique<ParticleSystemInstance>(std::move(system));
        auto simulator = std::make_unique<CPUParticleSimulator>();
        simulator->Initialize(request.config.maxParticleCount);
        instance->SetSimulator(std::move(simulator), "CPU");
        ApplyConfiguration(*instance, request);

        outEntry.resource = std::move(resource);
        outEntry.residencyLease = std::move(residencyLease);
        outEntry.instance = std::move(instance);
        outEntry.config = request.config;
        outEntry.payloadRevision = request.config.configurationRevision;
        lastFailureReason.clear();
        return true;
    }

    static void ApplyConfiguration(ParticleSystemInstance& instance,
                                   const ParticleEcsRuntimeRequest& request)
    {
        instance.SetTransform(request.worldTransform);
        instance.SetEmissionRateMultiplier(request.config.emissionRateScale);
        instance.SetSimulationSpeedMultiplier(request.config.simulationSpeed);
        instance.SetVisible(request.config.visible);
        instance.SetSimulateWhenHidden(request.config.simulateWhenHidden);
    }

    [[nodiscard]] static bool ApplyCommand(ParticleSystemInstance& instance,
                                           ParticleEcsCommand command,
                                           bool autoPlay)
    {
        switch (command)
        {
            case ParticleEcsCommand::None:
                if (autoPlay)
                {
                    instance.Play();
                }
                return true;
            case ParticleEcsCommand::Play:
                if (instance.IsPaused())
                {
                    instance.Resume();
                }
                else
                {
                    instance.Play();
                }
                return true;
            case ParticleEcsCommand::Stop:
                instance.Stop();
                return true;
            case ParticleEcsCommand::Pause:
                instance.Pause();
                return true;
            case ParticleEcsCommand::Restart:
                instance.Restart();
                return true;
            case ParticleEcsCommand::Clear:
                instance.Clear();
                return true;
        }

        return false;
    }

    static void RestorePlaybackState(ParticleSystemInstance& instance,
                                     PlaybackState playbackState)
    {
        switch (playbackState)
        {
            case PlaybackState::Playing:
                instance.Play();
                break;
            case PlaybackState::Paused:
                instance.Play();
                instance.Pause();
                break;
            case PlaybackState::Stopped:
                break;
        }
    }

    void Retire(Entry& entry)
    {
        entry.instance.reset();
        entry.residencyLease.Reset();
        entry.resource = nullptr;
        entry.config = {};
        entry.payloadRevision = 0;
        entry.allocated = false;
        ++entry.generation;
        if (entry.generation == 0)
        {
            ++entry.generation;
        }
    }

    Resource::ResourceManager* resourceManager = nullptr;
    std::thread::id ownerThread;
    std::string lastFailureReason;
    std::vector<Entry> entries;
    std::vector<uint32> freeEntries;
};

ParticleEcsRuntimeGateway::ParticleEcsRuntimeGateway(Resource::ResourceManager& resourceManager)
    : m_state(std::make_unique<State>(resourceManager))
{
}

ParticleEcsRuntimeGateway::~ParticleEcsRuntimeGateway() = default;

ParticleEcsRuntimeHandle ParticleEcsRuntimeGateway::Create(
    const ParticleEcsRuntimeRequest& request)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return ParticleEcsRuntimeHandle::Invalid();
    }

    State::Entry candidate;
    try
    {
        if (!m_state->BuildEntry(request, candidate) ||
            !State::ApplyCommand(*candidate.instance, request.command, request.config.autoPlay))
        {
            return ParticleEcsRuntimeHandle::Invalid();
        }

        uint32 index = RVX_INVALID_INDEX;
        if (m_state->freeEntries.empty())
        {
            if (m_state->entries.size() == m_state->entries.capacity())
            {
                m_state->entries.reserve(m_state->entries.size() + 1u);
            }
            index = static_cast<uint32>(m_state->entries.size());
            m_state->entries.emplace_back();
        }
        else
        {
            index = m_state->freeEntries.back();
        }

        State::Entry& entry = m_state->entries[index];
        candidate.generation = entry.generation == 0 ? 1 : entry.generation;
        candidate.allocated = true;
        entry = std::move(candidate);
        if (!m_state->freeEntries.empty())
        {
            m_state->freeEntries.pop_back();
        }
        return ParticleEcsRuntimeHandle::Create(index, entry.generation);
    }
    catch (...)
    {
        return ParticleEcsRuntimeHandle::Invalid();
    }
}

bool ParticleEcsRuntimeGateway::Update(ParticleEcsRuntimeHandle handle,
                                       const ParticleEcsRuntimeRequest& request)
{
    if (m_state == nullptr || !m_state->IsOwnerThread() || !IsValidRequest(request))
    {
        return false;
    }

    State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr || entry->instance == nullptr)
    {
        return false;
    }

    try
    {
        if (entry->config.systemAssetId != request.config.systemAssetId)
        {
            const PlaybackState playbackState = entry->instance->GetPlaybackState();
            State::Entry replacement;
            if (!m_state->BuildEntry(request, replacement))
            {
                return false;
            }

            replacement.generation = entry->generation;
            replacement.allocated = true;
            State::RestorePlaybackState(*replacement.instance, playbackState);
            *entry = std::move(replacement);
        }
        else if (entry->instance->GetMaxParticles() != request.config.maxParticleCount)
        {
            return false;
        }

        State::ApplyConfiguration(*entry->instance, request);
        if (!State::ApplyCommand(*entry->instance, request.command, false))
        {
            return false;
        }

        entry->instance->Simulate(static_cast<float>(request.deltaSeconds));
        entry->config = request.config;
        entry->payloadRevision = request.config.configurationRevision;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ParticleEcsRuntimeGateway::CaptureSnapshot(
    ParticleEcsRuntimeHandle handle,
    ParticleEcsRuntimeSnapshot& outSnapshot) const
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return false;
    }

    const State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr || entry->instance == nullptr || !entry->resource)
    {
        return false;
    }

    const ParticleSystemInstance& instance = *entry->instance;
    const ParticleSystem::Ptr system = instance.GetSystem();
    if (!system)
    {
        return false;
    }

    ParticleRenderSnapshotItem item;
    item.instanceId = instance.GetInstanceId();
    item.systemId = entry->config.systemAssetId.value;
    item.systemAssetId = entry->config.systemAssetId;
    item.systemName = system->name;
    item.worldMatrix = instance.GetTransform();
    item.worldBounds = instance.GetWorldBounds();
    item.position = instance.GetPosition();
    item.renderMode = ToSnapshotRenderMode(system->renderMode);
    item.blendMode = ToSnapshotBlendMode(system->blendMode);
    item.simulationBackend = ToSnapshotSimulationBackend(instance);
    item.aliveParticleCount = instance.GetAliveCount();
    item.maxParticleCount = instance.GetMaxParticles();
    item.lodLevel = instance.GetCurrentLODLevel();
    item.normalizedTime = instance.GetNormalizedTime();
    item.visible = instance.IsVisible();
    item.simulationSupported = instance.IsSimulationSupported();
    item.softParticlesEnabled = system->softParticleConfig.enabled;
    item.softParticleFadeDistance = system->softParticleConfig.fadeDistance;
    item.sortingSupported = false;
    item.sortingReason = "Particle sorting is deferred to Render-owned feature passes";

    if (const IParticleSimulator* simulator = instance.GetSimulator())
    {
        item.renderPayloadAvailable = simulator->BuildRenderParticlePayload(item.particles);
    }
    item.payloadStatus = item.renderPayloadAvailable
                             ? ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady
                             : ParticleRenderSnapshotPayloadStatus::MetadataOnly;
    item.renderPayloadReason = item.renderPayloadAvailable
                                   ? "CPU particle payload exported; Render-owned particle upload/draw implementation is not connected"
                                   : "Particle snapshot contains metadata only; Render-owned particle draw data extraction is not connected";
    if (!item.simulationSupported)
    {
        item.unsupportedReason = instance.GetSimulationUnsupportedReason();
    }

    outSnapshot.item = std::move(item);
    outSnapshot.payloadRevision = entry->payloadRevision;
    return true;
}

ParticleEcsReleaseResult ParticleEcsRuntimeGateway::Release(ParticleEcsRuntimeHandle handle)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return ParticleEcsReleaseResult::Failed;
    }

    State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr)
    {
        return ParticleEcsReleaseResult::AlreadyAbsent;
    }

    try
    {
        if (m_state->freeEntries.size() == m_state->freeEntries.capacity())
        {
            m_state->freeEntries.reserve(m_state->freeEntries.size() + 1u);
        }
    }
    catch (...)
    {
        return ParticleEcsReleaseResult::Failed;
    }

    const uint32 index = handle.GetIndex();
    m_state->Retire(*entry);
    m_state->freeEntries.push_back(index);
    return ParticleEcsReleaseResult::Released;
}

bool ParticleEcsRuntimeGateway::IsAlive(ParticleEcsRuntimeHandle handle) const
{
    return m_state != nullptr && m_state->IsOwnerThread() && m_state->Find(handle) != nullptr;
}

const std::string& ParticleEcsRuntimeGateway::GetLastFailureReason() const
{
    static const std::string empty;
    return m_state != nullptr ? m_state->lastFailureReason : empty;
}

uint32 ParticleEcsRuntimeGateway::GetLiveRuntimeCount() const
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return 0;
    }

    uint32 count = 0;
    for (const State::Entry& entry : m_state->entries)
    {
        if (entry.allocated)
        {
            ++count;
        }
    }
    return count;
}
} // namespace RVX::Particle
