#include "Scene/ECS/SceneFeatureSnapshotStore.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace RVX::SceneECS
{
namespace
{
    template<typename FrozenValue, typename Entries>
    [[nodiscard]] bool CopyFrozenEntries(
        const Entries& entries,
        ECS::SceneRuntimeId sceneRuntimeId,
        FrozenSceneObjectType type,
        std::vector<FrozenValue>& outValues)
    {
        try
        {
            outValues.clear();
            outValues.reserve(entries.size());
            for (const auto& [entity, entry] : entries)
            {
                if (!entity.IsValid())
                {
                    return false;
                }
                outValues.push_back({
                    .id = MakeFrozenSceneObjectId(sceneRuntimeId, entity, type),
                    .sourceEntity = entity,
                    .state = entry.state,
                    .payloadRevision = entry.payloadRevision,
                });
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
    : m_sceneRuntimeId(sceneRuntimeId)
{
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
    try
    {
        m_particles[identity->entity] = {
            .state = std::move(state),
            .payloadRevision = payloadRevision,
        };
        return true;
    }
    catch (...)
    {
        return false;
    }
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
    try
    {
        m_water[identity->entity] = {
            .state = std::move(state),
            .payloadRevision = payloadRevision,
        };
        return true;
    }
    catch (...)
    {
        return false;
    }
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
    try
    {
        m_terrain[identity->entity] = {
            .state = std::move(state),
            .payloadRevision = payloadRevision,
        };
        return true;
    }
    catch (...)
    {
        return false;
    }
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
    if (!m_sceneRuntimeId.IsValid() || destination.sceneRuntimeId != m_sceneRuntimeId ||
        destination.revision == 0)
    {
        return false;
    }

    std::vector<FrozenSceneParticle> particles;
    std::vector<FrozenSceneWater> water;
    std::vector<FrozenSceneTerrain> terrain;
    if (!CopyFrozenEntries<FrozenSceneParticle>(
            m_particles, m_sceneRuntimeId, FrozenSceneObjectType::Particle,
            particles) ||
        !CopyFrozenEntries<FrozenSceneWater>(
            m_water, m_sceneRuntimeId, FrozenSceneObjectType::Water, water) ||
        !CopyFrozenEntries<FrozenSceneTerrain>(
            m_terrain, m_sceneRuntimeId, FrozenSceneObjectType::Terrain, terrain))
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
