#include "Water/ECS/WaterEcsRuntimeGateway.h"

#include <cmath>
#include <thread>
#include <utility>
#include <vector>

namespace RVX::Water
{
namespace
{
    [[nodiscard]] bool IsFinite(float32 value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool IsFinite(const Vec2& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y);
    }

    [[nodiscard]] bool IsFinite(const Vec3& value) noexcept
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    [[nodiscard]] bool IsTranslationOnlyYUp(const Mat4& value) noexcept
    {
        constexpr float32 tolerance = 0.0001f;
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
        return std::abs(value[0][0] - 1.0f) <= tolerance &&
               std::abs(value[1][1] - 1.0f) <= tolerance &&
               std::abs(value[2][2] - 1.0f) <= tolerance &&
               std::abs(value[3][3] - 1.0f) <= tolerance &&
               std::abs(value[0][1]) <= tolerance && std::abs(value[0][2]) <= tolerance &&
               std::abs(value[0][3]) <= tolerance && std::abs(value[1][0]) <= tolerance &&
               std::abs(value[1][2]) <= tolerance && std::abs(value[1][3]) <= tolerance &&
               std::abs(value[2][0]) <= tolerance && std::abs(value[2][1]) <= tolerance &&
               std::abs(value[2][3]) <= tolerance;
    }

    [[nodiscard]] bool IsKnownCommand(WaterEcsCommand command) noexcept
    {
        return command >= WaterEcsCommand::None && command <= WaterEcsCommand::Refresh;
    }

    [[nodiscard]] Vec3 ExtractTranslation(const Mat4& transform) noexcept
    {
        return {transform[3][0], transform[3][1], transform[3][2]};
    }

    [[nodiscard]] bool IsKnownSurfaceType(
        WaterRenderSnapshotSurfaceType type) noexcept
    {
        switch (type)
        {
            case WaterRenderSnapshotSurfaceType::Ocean:
            case WaterRenderSnapshotSurfaceType::Lake:
            case WaterRenderSnapshotSurfaceType::River:
            case WaterRenderSnapshotSurfaceType::Pool:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] bool IsFiniteAndNonNegative(const Vec3& value) noexcept
    {
        return IsFinite(value) && value.x >= 0.0f && value.y >= 0.0f &&
               value.z >= 0.0f;
    }

    [[nodiscard]] bool IsAuthoringConfigValid(const WaterEcsConfig& config) noexcept
    {
        return config.surfaceAssetId.IsValid() && config.materialAssetId.IsValid() &&
               config.configurationRevision != 0 && IsFinite(config.size) &&
               config.size.x > 0.0f && config.size.y > 0.0f &&
               IsFinite(config.depth) && config.depth >= 0.0f &&
               IsKnownSurfaceType(config.surfaceType) &&
               config.simulationType == WaterRenderSnapshotSimulationType::Gerstner &&
               config.gerstnerWaves.IsStructurallyValid() &&
               IsFiniteAndNonNegative(config.shallowColor) &&
               IsFiniteAndNonNegative(config.deepColor) &&
               IsFiniteAndNonNegative(config.foamColor) &&
               IsFiniteAndNonNegative(config.absorptionCoefficient) &&
               IsFinite(config.transparency) && IsFinite(config.reflectionStrength) &&
               IsFinite(config.refractionStrength) && IsFinite(config.roughness) &&
               IsFinite(config.normalScale) && IsFinite(config.foamIntensity) &&
               IsFinite(config.foamCrestThreshold);
    }

    [[nodiscard]] WaterRenderSnapshotItem MakeReadyPacket(
        const WaterEcsConfig& config,
        const Mat4& transform,
        uint64 componentId) noexcept
    {
        WaterRenderSnapshotItem item;
        if (!IsAuthoringConfigValid(config) || !IsTranslationOnlyYUp(transform) ||
            componentId == 0)
        {
            return item;
        }

        constexpr float32 twoPi = 6.28318530717958647692f;
        float32 verticalEnvelope = 0.0f;
        float32 horizontalEnvelope = 0.0f;
        for (uint32 index = 0; index < config.gerstnerWaves.waveCount; ++index)
        {
            const WaterGerstnerWave& wave = config.gerstnerWaves.waves[index];
            verticalEnvelope += wave.amplitude;
            horizontalEnvelope += wave.steepness /
                (twoPi / wave.wavelength);
        }

        const Vec3 worldPosition = ExtractTranslation(transform);
        item.payloadStatus = WaterRenderSnapshotPayloadStatus::Ready;
        item.componentId = componentId;
        item.surfaceAssetId = config.surfaceAssetId;
        item.materialAssetId = config.materialAssetId;
        item.worldPosition = worldPosition;
        item.worldBounds = AABB(
            worldPosition - Vec3(config.size.x * 0.5f + horizontalEnvelope,
                                 config.depth + verticalEnvelope,
                                 config.size.y * 0.5f + horizontalEnvelope),
            worldPosition + Vec3(config.size.x * 0.5f + horizontalEnvelope,
                                 verticalEnvelope,
                                 config.size.y * 0.5f + horizontalEnvelope));
        item.size = config.size;
        item.depth = config.depth;
        item.surfaceType = config.surfaceType;
        item.simulationType = config.simulationType;
        item.gerstnerWaves = config.gerstnerWaves;
        item.shallowColor = config.shallowColor;
        item.deepColor = config.deepColor;
        item.foamColor = config.foamColor;
        item.absorptionCoefficient = config.absorptionCoefficient;
        item.transparency = config.transparency;
        item.reflectionStrength = config.reflectionStrength;
        item.refractionStrength = config.refractionStrength;
        item.roughness = config.roughness;
        item.normalScale = config.normalScale;
        item.foamIntensity = config.foamIntensity;
        item.foamCrestThreshold = config.foamCrestThreshold;
        item.reflectionEnabled = config.reflectionEnabled;
        item.refractionEnabled = config.refractionEnabled;
        item.foamEnabled = config.foamEnabled;
        item.contentHash = item.ComputeContentHash();
        return item.IsStructurallyValid() ? item : WaterRenderSnapshotItem{};
    }
} // namespace

struct WaterEcsRuntimeGateway::State
{
    struct Entry
    {
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

    [[nodiscard]] bool IsOwnerThread() const noexcept
    {
        return std::this_thread::get_id() == ownerThread;
    }

    [[nodiscard]] Entry* Find(WaterEcsRuntimeHandle handle) noexcept
    {
        if (!handle.IsValid() || handle.GetIndex() >= entries.size())
        {
            return nullptr;
        }
        Entry& entry = entries[handle.GetIndex()];
        return entry.allocated && entry.generation == handle.GetGeneration() ? &entry : nullptr;
    }

    [[nodiscard]] const Entry* Find(WaterEcsRuntimeHandle handle) const noexcept
    {
        return const_cast<State*>(this)->Find(handle);
    }

    [[nodiscard]] static bool BuildEntry(const WaterEcsRuntimeRequest& request,
                                         Entry& outEntry) noexcept
    {
        if (!IsKnownCommand(request.command) || !IsTranslationOnlyYUp(request.worldTransform))
        {
            return false;
        }
        WaterEcsConfig canonicalConfig = request.config;
        if (!CanonicalizeWaterGerstnerWaveSet(canonicalConfig.gerstnerWaves) ||
            !IsAuthoringConfigValid(canonicalConfig))
        {
            return false;
        }
        if (!MakeReadyPacket(canonicalConfig, request.worldTransform, 1).IsStructurallyValid())
        {
            return false;
        }
        outEntry.config = std::move(canonicalConfig);
        outEntry.worldTransform = request.worldTransform;
        outEntry.payloadRevision = outEntry.config.configurationRevision;
        outEntry.active = outEntry.config.autoActivate;
        return true;
    }

    [[nodiscard]] static bool ApplyCommand(Entry& entry,
                                           WaterEcsCommand command) noexcept
    {
        switch (command)
        {
            case WaterEcsCommand::None:
                return true;
            case WaterEcsCommand::Activate:
            case WaterEcsCommand::Refresh:
                entry.active = true;
                return true;
            case WaterEcsCommand::Deactivate:
                entry.active = false;
                return true;
            default:
                return false;
        }
    }

    void Retire(Entry& entry) noexcept
    {
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

    try
    {
        State::Entry candidate;
        if (!State::BuildEntry(request, candidate) ||
            !State::ApplyCommand(candidate, request.command))
        {
            return WaterEcsRuntimeHandle::Invalid();
        }

        uint32 index = RVX_INVALID_INDEX;
        if (m_state->freeEntries.empty())
        {
            if (m_state->entries.size() == m_state->entries.capacity())
            {
                m_state->entries.reserve(m_state->entries.size() + 1U);
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
    if (m_state == nullptr || !m_state->IsOwnerThread())
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
        State::Entry replacement;
        if (!State::BuildEntry(request, replacement))
        {
            return false;
        }
        replacement.generation = entry->generation;
        replacement.allocated = true;
        replacement.active = entry->active;
        *entry = std::move(replacement);
        return State::ApplyCommand(*entry, request.command);
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
    if (entry == nullptr)
    {
        return false;
    }

    outSnapshot = {};
    outSnapshot.payloadRevision = entry->payloadRevision;
    if (!entry->active)
    {
        return true;
    }
    outSnapshot.item = MakeReadyPacket(entry->config,
                                        entry->worldTransform,
                                        handle.GetPackedValue());
    return outSnapshot.item.IsStructurallyValid();
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
            m_state->freeEntries.reserve(m_state->freeEntries.size() + 1U);
        }
        const uint32 index = handle.GetIndex();
        m_state->Retire(*entry);
        m_state->freeEntries.push_back(index);
        return WaterEcsReleaseResult::Released;
    }
    catch (...)
    {
        return WaterEcsReleaseResult::Failed;
    }
}

bool WaterEcsRuntimeGateway::IsAlive(WaterEcsRuntimeHandle handle) const
{
    return m_state != nullptr && m_state->IsOwnerThread() &&
           m_state->Find(handle) != nullptr;
}
} // namespace RVX::Water
