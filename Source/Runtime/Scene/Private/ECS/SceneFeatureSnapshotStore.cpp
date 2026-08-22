#include "Scene/ECS/SceneFeatureSnapshotStore.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace RVX::SceneECS
{
namespace
{
    template<typename State, typename Entries>
    [[nodiscard]] bool PublishTypedEntry(
        const RenderFeatureChannelRegistry& channelRegistry,
        Entries& entries,
        ECS::EntityHandle entity,
        const RenderFeatureSourceIdentity& expectedSource,
        State state)
    {
        if (channelRegistry.ValidateProducer(
                State::ChannelId,
                State::SchemaVersion,
                RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore) !=
            RenderFeatureChannelRegistryCode::Success)
        {
            return false;
        }

        const auto existing = entries.find(entity);
        const uint64 lastAcceptedCaptureSequence =
            existing != entries.end() ? existing->second.header.captureSequence : 0;
        if (state.ValidateChannel(expectedSource, lastAcceptedCaptureSequence) !=
            RenderFeatureChannelValidationCode::Valid)
        {
            return false;
        }

        try
        {
            entries.insert_or_assign(entity, std::move(state));
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    template<typename State, typename Entries>
    [[nodiscard]] bool PublishLegacyEntry(
        const RenderFeatureChannelRegistry& channelRegistry,
        Entries& entries,
        ECS::EntityHandle entity,
        const RenderFeatureSourceIdentity& expectedSource,
        State state,
        uint64 configurationRevision)
    {
        uint64 captureSequence = 1;
        const auto existing = entries.find(entity);
        if (existing != entries.end())
        {
            if (existing->second.header.captureSequence ==
                std::numeric_limits<uint64>::max())
            {
                return false;
            }
            captureSequence = existing->second.header.captureSequence + 1;
        }

        state.header = {
            .channelId = State::ChannelId,
            .schemaVersion = State::SchemaVersion,
            .captureSequence = captureSequence,
            .configurationRevision = configurationRevision,
            .source = expectedSource,
        };
        return PublishTypedEntry(
            channelRegistry, entries, entity, expectedSource, std::move(state));
    }

    template<typename FrozenValue, typename Entries>
    [[nodiscard]] bool CopyFrozenEntries(
        const RenderFeatureChannelRegistry& channelRegistry,
        const Entries& entries,
        ECS::SceneRuntimeId sceneRuntimeId,
        FrozenSceneObjectType type,
        std::vector<FrozenValue>& outValues)
    {
        using State = typename Entries::mapped_type;
        try
        {
            outValues.clear();
            outValues.reserve(entries.size());
            for (const auto& [entity, state] : entries)
            {
                if (channelRegistry.ValidateRoute(
                        State::ChannelId,
                        State::SchemaVersion,
                        RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
                        RenderFeatureConsumerEndpointId::RenderSceneDatabase) !=
                    RenderFeatureChannelRegistryCode::Success)
                {
                    return false;
                }
                if (!entity.IsValid())
                {
                    return false;
                }
                const RenderFeatureSourceIdentity expectedSource{
                    .sceneRuntimeId = sceneRuntimeId.GetValue(),
                    .entitySlot = entity.GetIndex(),
                    .entityGeneration = entity.GetGeneration(),
                };
                if (state.ValidateChannel(expectedSource) !=
                    RenderFeatureChannelValidationCode::Valid)
                {
                    return false;
                }

                FrozenValue frozen;
                frozen.id = MakeFrozenSceneObjectId(sceneRuntimeId, entity, type);
                frozen.sourceEntity = entity;
                frozen.state = state;
                frozen.payloadRevision = state.header.configurationRevision;
                outValues.push_back(std::move(frozen));
            }
            std::sort(outValues.begin(), outValues.end(),
                      [](const FrozenValue& left, const FrozenValue& right)
                      {
                          return left.id.entity < right.id.entity;
                      });
            return true;
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace

SceneFeatureSnapshotStore::SceneFeatureSnapshotStore(
    ECS::SceneRuntimeId sceneRuntimeId)
    : SceneFeatureSnapshotStore(
          sceneRuntimeId, CreateBuiltinRenderFeatureChannelRegistry())
{
}

SceneFeatureSnapshotStore::SceneFeatureSnapshotStore(
    ECS::SceneRuntimeId sceneRuntimeId,
    RenderFeatureChannelRegistry channelRegistry)
    : m_sceneRuntimeId(sceneRuntimeId),
      m_channelRegistry(std::move(channelRegistry))
{
    if (!m_channelRegistry.IsSealed())
    {
        static_cast<void>(m_channelRegistry.Seal());
    }
}

std::optional<SceneFeatureSnapshotSource>
SceneFeatureSnapshotStore::MakeSourceIdentity(const ECS::EntityRef& source) const
{
    if (!m_sceneRuntimeId.IsValid() || !source.IsValid() ||
        source.GetSceneRuntimeId() != m_sceneRuntimeId ||
        !source.GetHandle().IsValid())
    {
        return std::nullopt;
    }
    return SceneFeatureSnapshotSource{m_sceneRuntimeId, source.GetHandle()};
}

bool SceneFeatureSnapshotStore::PublishParticle(const ECS::EntityRef& source,
                                                ParticleRenderSnapshotItem state,
                                                uint64 payloadRevision)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    if (!identity.has_value())
    {
        return false;
    }
    return PublishLegacyEntry(m_channelRegistry,
                              m_particles,
                              identity->entity,
                              identity->ToRenderIdentity(),
                              std::move(state),
                              payloadRevision);
}

bool SceneFeatureSnapshotStore::PublishWater(const ECS::EntityRef& source,
                                             WaterRenderSnapshotItem state,
                                             uint64 payloadRevision)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    if (!identity.has_value())
    {
        return false;
    }
    return PublishLegacyEntry(m_channelRegistry,
                              m_water,
                              identity->entity,
                              identity->ToRenderIdentity(),
                              std::move(state),
                              payloadRevision);
}

bool SceneFeatureSnapshotStore::PublishTerrain(const ECS::EntityRef& source,
                                               TerrainRenderSnapshotItem state,
                                               uint64 payloadRevision)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    if (!identity.has_value())
    {
        return false;
    }
    return PublishLegacyEntry(m_channelRegistry,
                              m_terrain,
                              identity->entity,
                              identity->ToRenderIdentity(),
                              std::move(state),
                              payloadRevision);
}

bool SceneFeatureSnapshotStore::PublishParticleChannel(
    const ECS::EntityRef& source,
    ParticleRenderSnapshotItem state)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    return identity.has_value() &&
           PublishTypedEntry(m_channelRegistry,
                             m_particles,
                             identity->entity,
                             identity->ToRenderIdentity(),
                             std::move(state));
}

bool SceneFeatureSnapshotStore::PublishWaterChannel(
    const ECS::EntityRef& source,
    WaterRenderSnapshotItem state)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    return identity.has_value() &&
           PublishTypedEntry(m_channelRegistry,
                             m_water,
                             identity->entity,
                             identity->ToRenderIdentity(),
                             std::move(state));
}

bool SceneFeatureSnapshotStore::PublishTerrainChannel(
    const ECS::EntityRef& source,
    TerrainRenderSnapshotItem state)
{
    const std::optional<SceneFeatureSnapshotSource> identity =
        MakeSourceIdentity(source);
    return identity.has_value() &&
           PublishTypedEntry(m_channelRegistry,
                             m_terrain,
                             identity->entity,
                             identity->ToRenderIdentity(),
                             std::move(state));
}

bool SceneFeatureSnapshotStore::RemoveParticle(
    const SceneFeatureSnapshotSource& source)
{
    if (!IsOwnedSource(source))
    {
        return false;
    }

    static_cast<void>(m_particles.erase(source.entity));
    return true;
}

bool SceneFeatureSnapshotStore::RemoveWater(
    const SceneFeatureSnapshotSource& source)
{
    if (!IsOwnedSource(source))
    {
        return false;
    }

    static_cast<void>(m_water.erase(source.entity));
    return true;
}

bool SceneFeatureSnapshotStore::RemoveTerrain(
    const SceneFeatureSnapshotSource& source)
{
    if (!IsOwnedSource(source))
    {
        return false;
    }

    static_cast<void>(m_terrain.erase(source.entity));
    return true;
}

bool SceneFeatureSnapshotStore::Remove(const SceneFeatureSnapshotSource& source)
{
    if (!IsOwnedSource(source))
    {
        return false;
    }

    static_cast<void>(m_particles.erase(source.entity));
    static_cast<void>(m_water.erase(source.entity));
    static_cast<void>(m_terrain.erase(source.entity));
    return true;
}

void SceneFeatureSnapshotStore::Clear()
{
    m_particles.clear();
    m_water.clear();
    m_terrain.clear();
}

bool SceneFeatureSnapshotStore::FreezeInto(FrozenSceneSnapshot& destination) const
{
    if (!m_channelRegistry.IsSealed() ||
        !m_sceneRuntimeId.IsValid() || destination.sceneRuntimeId != m_sceneRuntimeId ||
        destination.revision == 0)
    {
        return false;
    }

    std::vector<FrozenSceneParticle> particles;
    std::vector<FrozenSceneWater> water;
    std::vector<FrozenSceneTerrain> terrain;
    if (!CopyFrozenEntries<FrozenSceneParticle>(
            m_channelRegistry, m_particles, m_sceneRuntimeId,
            FrozenSceneObjectType::Particle,
            particles) ||
        !CopyFrozenEntries<FrozenSceneWater>(
            m_channelRegistry, m_water, m_sceneRuntimeId,
            FrozenSceneObjectType::Water, water) ||
        !CopyFrozenEntries<FrozenSceneTerrain>(
            m_channelRegistry, m_terrain, m_sceneRuntimeId,
            FrozenSceneObjectType::Terrain, terrain))
    {
        return false;
    }

    destination.particles = std::move(particles);
    destination.water = std::move(water);
    destination.terrain = std::move(terrain);
    return true;
}

SceneFeatureSnapshotStoreCounts SceneFeatureSnapshotStore::GetCounts() const
{
    const auto countOf = [](size_t count)
    {
        return count > std::numeric_limits<uint32>::max()
                   ? std::numeric_limits<uint32>::max()
                   : static_cast<uint32>(count);
    };
    return {
        .particleCount = countOf(m_particles.size()),
        .waterCount = countOf(m_water.size()),
        .terrainCount = countOf(m_terrain.size()),
    };
}

bool SceneFeatureSnapshotStore::IsOwnedSource(
    const SceneFeatureSnapshotSource& source) const
{
    return m_sceneRuntimeId.IsValid() && source.IsValid() &&
           source.sceneRuntimeId == m_sceneRuntimeId;
}
} // namespace RVX::SceneECS
