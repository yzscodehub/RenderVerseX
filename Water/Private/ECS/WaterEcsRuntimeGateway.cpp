#include "Water/ECS/WaterEcsRuntimeGateway.h"

#include "Water/Caustics.h"
#include "Water/Underwater.h"
#include "Water/WaterSimulation.h"
#include "Water/WaterSurface.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace RVX::Water
{
namespace
{
    [[nodiscard]] bool IsFinite(float value)
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFinite(const Vec2& value)
    {
        return IsFinite(value.x) && IsFinite(value.y);
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

    [[nodiscard]] bool IsValidRequest(const WaterEcsRuntimeRequest& request)
    {
        const WaterEcsConfig& config = request.config;
        return config.surfaceAssetId.IsValid() && config.materialAssetId.IsValid() &&
               config.configurationRevision != 0 && IsFinite(config.size) &&
               config.size.x > 0.0f && config.size.y > 0.0f && IsFinite(config.depth) &&
               config.depth >= 0.0f && config.resolution != 0 &&
               IsFinite(request.worldTransform) && std::isfinite(request.deltaSeconds) &&
               request.deltaSeconds >= 0.0;
    }

    [[nodiscard]] bool IsKnownCommand(WaterEcsCommand command)
    {
        return command >= WaterEcsCommand::None && command <= WaterEcsCommand::Refresh;
    }

    [[nodiscard]] WaterSurfaceType ToWaterSurfaceType(WaterRenderSnapshotSurfaceType type)
    {
        switch (type)
        {
            case WaterRenderSnapshotSurfaceType::Ocean:
                return WaterSurfaceType::Ocean;
            case WaterRenderSnapshotSurfaceType::Lake:
                return WaterSurfaceType::Lake;
            case WaterRenderSnapshotSurfaceType::River:
                return WaterSurfaceType::River;
            case WaterRenderSnapshotSurfaceType::Pool:
                return WaterSurfaceType::Pool;
        }

        return WaterSurfaceType::Ocean;
    }

    [[nodiscard]] WaterSimulationType ToWaterSimulationType(
        WaterRenderSnapshotSimulationType type)
    {
        switch (type)
        {
            case WaterRenderSnapshotSimulationType::Simple:
                return WaterSimulationType::Simple;
            case WaterRenderSnapshotSimulationType::Gerstner:
                return WaterSimulationType::Gerstner;
            case WaterRenderSnapshotSimulationType::FFT:
                return WaterSimulationType::FFT;
        }

        return WaterSimulationType::Gerstner;
    }

    [[nodiscard]] Vec3 ExtractTranslation(const Mat4& transform)
    {
        return {transform[3][0], transform[3][1], transform[3][2]};
    }
} // namespace

struct WaterEcsRuntimeGateway::State
{
    struct Entry
    {
        std::unique_ptr<WaterSurface> surface;
        std::unique_ptr<WaterSimulation> simulation;
        std::unique_ptr<Caustics> caustics;
        std::unique_ptr<Underwater> underwater;
        WaterEcsConfig config;
        Mat4 worldTransform{1.0f};
        uint64 payloadRevision = 0;
        uint32 generation = 1;
        bool active = true;
        bool allocated = false;
    };

    State()
        : ownerThread(std::this_thread::get_id())
    {
    }

    [[nodiscard]] bool IsOwnerThread() const
    {
        return std::this_thread::get_id() == ownerThread;
    }

    [[nodiscard]] Entry* Find(WaterEcsRuntimeHandle handle)
    {
        if (!handle.IsValid() || handle.GetIndex() >= entries.size())
        {
            return nullptr;
        }

        Entry& entry = entries[handle.GetIndex()];
        return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
    }

    [[nodiscard]] const Entry* Find(WaterEcsRuntimeHandle handle) const
    {
        if (!handle.IsValid() || handle.GetIndex() >= entries.size())
        {
            return nullptr;
        }

        const Entry& entry = entries[handle.GetIndex()];
        return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
    }

    [[nodiscard]] static bool BuildEntry(const WaterEcsRuntimeRequest& request,
                                         Entry& outEntry)
    {
        if (!IsValidRequest(request))
        {
            return false;
        }

        auto surface = std::make_unique<WaterSurface>();
        WaterSurfaceDesc surfaceDesc;
        surfaceDesc.size = request.config.size;
        surfaceDesc.resolution = request.config.resolution;
        surfaceDesc.type = ToWaterSurfaceType(request.config.surfaceType);
        if (!surface->Create(surfaceDesc))
        {
            return false;
        }

        auto simulation = std::make_unique<WaterSimulation>();
        WaterSimulationDesc simulationDesc;
        simulationDesc.type = ToWaterSimulationType(request.config.simulationType);
        simulationDesc.resolution = request.config.resolution;
        simulationDesc.domainSize = std::max(request.config.size.x, request.config.size.y);
        simulationDesc.oceanParams.depth = request.config.depth;
        if (!simulation->Initialize(simulationDesc))
        {
            return false;
        }

        std::unique_ptr<Caustics> caustics;
        if (request.config.causticsEnabled)
        {
            caustics = std::make_unique<Caustics>();
            CausticsDesc causticsDesc;
            causticsDesc.maxDepth = request.config.depth;
            if (!caustics->Initialize(causticsDesc))
            {
                return false;
            }
        }

        std::unique_ptr<Underwater> underwater;
        if (request.config.underwaterEffectsEnabled)
        {
            underwater = std::make_unique<Underwater>();
            if (!underwater->Initialize({}))
            {
                return false;
            }
        }

        outEntry.surface = std::move(surface);
        outEntry.simulation = std::move(simulation);
        outEntry.caustics = std::move(caustics);
        outEntry.underwater = std::move(underwater);
        outEntry.config = request.config;
        outEntry.worldTransform = request.worldTransform;
        outEntry.payloadRevision = request.config.configurationRevision;
        outEntry.active = request.config.autoActivate;
        return true;
    }

    [[nodiscard]] static bool ApplyCommand(Entry& entry, WaterEcsCommand command)
    {
        switch (command)
        {
            case WaterEcsCommand::None:
                return true;
            case WaterEcsCommand::Activate:
            case WaterEcsCommand::Refresh:
                entry.active = true;
                if (entry.simulation)
                {
                    entry.simulation->SetPaused(false);
                }
                return true;
            case WaterEcsCommand::Deactivate:
                entry.active = false;
                if (entry.simulation)
                {
                    entry.simulation->SetPaused(true);
                }
                return true;
        }

        return false;
    }

    void Retire(Entry& entry)
    {
        entry.underwater.reset();
        entry.caustics.reset();
        entry.simulation.reset();
        entry.surface.reset();
        entry.config = {};
        entry.worldTransform = Mat4(1.0f);
        entry.payloadRevision = 0;
        entry.active = false;
        entry.allocated = false;
        ++entry.generation;
        if (entry.generation == 0)
        {
            ++entry.generation;
        }
    }

    std::thread::id ownerThread;
    std::vector<Entry> entries;
    std::vector<uint32> freeEntries;
};

WaterEcsRuntimeGateway::WaterEcsRuntimeGateway()
    : m_state(std::make_unique<State>())
{
}

WaterEcsRuntimeGateway::~WaterEcsRuntimeGateway() = default;

WaterEcsRuntimeHandle WaterEcsRuntimeGateway::Create(const WaterEcsRuntimeRequest& request)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return WaterEcsRuntimeHandle::Invalid();
    }

    State::Entry candidate;
    try
    {
        if (!IsKnownCommand(request.command) || !State::BuildEntry(request, candidate) ||
            !State::ApplyCommand(candidate, request.command))
        {
            return WaterEcsRuntimeHandle::Invalid();
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
        return WaterEcsRuntimeHandle::Create(index, entry.generation);
    }
    catch (...)
    {
        return WaterEcsRuntimeHandle::Invalid();
    }
}

bool WaterEcsRuntimeGateway::Update(WaterEcsRuntimeHandle handle,
                                    const WaterEcsRuntimeRequest& request)
{
    if (m_state == nullptr || !m_state->IsOwnerThread() || !IsValidRequest(request) ||
        !IsKnownCommand(request.command))
    {
        return false;
    }

    State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr)
    {
        return false;
    }

    try
    {
        const bool rebuild = entry->config.configurationRevision !=
                                 request.config.configurationRevision ||
                             request.command == WaterEcsCommand::Refresh;
        const bool wasActive = entry->active;
        if (rebuild)
        {
            State::Entry replacement;
            if (!State::BuildEntry(request, replacement))
            {
                return false;
            }
            replacement.generation = entry->generation;
            replacement.allocated = true;
            replacement.active = wasActive;
            if (!replacement.active && replacement.simulation)
            {
                replacement.simulation->SetPaused(true);
            }
            *entry = std::move(replacement);
        }

        entry->worldTransform = request.worldTransform;
        entry->config = request.config;
        entry->payloadRevision = request.config.configurationRevision;
        if (!State::ApplyCommand(*entry, request.command))
        {
            return false;
        }
        if (entry->active)
        {
            const float deltaSeconds = static_cast<float>(request.deltaSeconds);
            entry->simulation->Update(deltaSeconds);
            if (entry->caustics)
            {
                entry->caustics->Update(deltaSeconds, entry->simulation.get());
            }
            if (entry->underwater)
            {
                entry->underwater->Update(deltaSeconds);
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool WaterEcsRuntimeGateway::CaptureSnapshot(WaterEcsRuntimeHandle handle,
                                             WaterEcsRuntimeSnapshot& outSnapshot) const
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return false;
    }

    const State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr || entry->surface == nullptr || entry->simulation == nullptr)
    {
        return false;
    }

    const Vec3 worldPosition = ExtractTranslation(entry->worldTransform);
    const Vec2 size = entry->surface->GetSize();
    const float waveAmplitude = 5.0f;
    const Vec3 minimum = worldPosition -
                         Vec3(size.x * 0.5f, entry->config.depth, size.y * 0.5f);
    const Vec3 maximum = worldPosition + Vec3(size.x * 0.5f, waveAmplitude, size.y * 0.5f);
    const WaterVisualProperties& visual = entry->surface->GetVisualProperties();

    WaterRenderSnapshotItem item;
    item.componentId = handle.GetPackedValue();
    item.surfaceAssetId = entry->config.surfaceAssetId;
    item.materialAssetId = entry->config.materialAssetId;
    item.worldPosition = worldPosition;
    item.worldBounds = AABB(minimum, maximum);
    item.size = size;
    item.depth = entry->config.depth;
    item.resolution = entry->surface->GetResolution();
    item.surfaceType = entry->config.surfaceType;
    item.simulationType = entry->config.simulationType;
    item.shallowColor = visual.shallowColor;
    item.deepColor = visual.deepColor;
    item.foamColor = visual.foamColor;
    item.transparency = visual.transparency;
    item.reflectionStrength = visual.reflectionStrength;
    item.refractionStrength = visual.refractionStrength;
    item.roughness = visual.roughness;
    item.foamIntensity = visual.foamIntensity;
    item.reflectionEnabled = entry->config.reflectionEnabled;
    item.refractionEnabled = entry->config.refractionEnabled;
    item.causticsEnabled = entry->caustics != nullptr;
    item.underwaterEffectsEnabled = entry->underwater != nullptr;
    item.foamEnabled = entry->config.foamEnabled;
    item.gpuInitialized = false;
    item.cpuSimulationAvailable = true;
    item.renderGpuPathAvailable = false;
    item.gpuInitializationReason =
        "Water feature module exports CPU state only; GPU resources are owned by Render water passes";
    item.renderPathReason =
        "Render-owned water surface, caustics, and underwater passes are not connected to this snapshot yet";
    if (entry->config.simulationType == WaterRenderSnapshotSimulationType::FFT)
    {
        item.simulationFallbackReason =
            "FFT water simulation uses a deterministic CPU fallback until Render-owned GPU simulation is implemented";
    }

    outSnapshot.item = std::move(item);
    outSnapshot.payloadRevision = entry->payloadRevision;
    return true;
}

WaterEcsReleaseResult WaterEcsRuntimeGateway::Release(WaterEcsRuntimeHandle handle)
{
    if (m_state == nullptr || !m_state->IsOwnerThread())
    {
        return WaterEcsReleaseResult::Failed;
    }

    State::Entry* entry = m_state->Find(handle);
    if (entry == nullptr)
    {
        return WaterEcsReleaseResult::AlreadyAbsent;
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
        return WaterEcsReleaseResult::Failed;
    }

    const uint32 index = handle.GetIndex();
    m_state->Retire(*entry);
    m_state->freeEntries.push_back(index);
    return WaterEcsReleaseResult::Released;
}

bool WaterEcsRuntimeGateway::IsAlive(WaterEcsRuntimeHandle handle) const
{
    return m_state != nullptr && m_state->IsOwnerThread() && m_state->Find(handle) != nullptr;
}
} // namespace RVX::Water
