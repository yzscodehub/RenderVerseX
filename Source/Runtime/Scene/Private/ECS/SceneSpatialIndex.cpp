#include "Scene/ECS/SceneSpatialIndex.h"

#include "ECS/ECS.h"
#include "Scene/ECS/Fragments.h"

#include <algorithm>
#include <cmath>

namespace RVX::SceneECS
{
namespace
{
    bool SameBounds(const AABB& left, const AABB& right)
    {
        return glm::all(glm::equal(left.GetMin(), right.GetMin())) &&
               glm::all(glm::equal(left.GetMax(), right.GetMax()));
    }

    void SortResults(std::vector<SpatialQueryResult>& results)
    {
        std::sort(results.begin(),
                  results.end(),
                  [](const SpatialQueryResult& left, const SpatialQueryResult& right)
                  {
                      return left.id.entity < right.id.entity;
                  });
    }
} // namespace

SceneSpatialIndex::SceneSpatialIndex(ECS::Registry& registry)
    : m_registry(&registry)
    , m_sceneRuntimeId(registry.GetSceneRuntimeId())
{
}

SpatialSynchronizeStats SceneSpatialIndex::Synchronize()
{
    SpatialSynchronizeStats stats;
    if (m_registry == nullptr || m_sceneRuntimeId != m_registry->GetSceneRuntimeId())
    {
        stats.removed = static_cast<uint32>(m_entries.size());
        m_entries.clear();
        return stats;
    }

    std::unordered_map<ECS::EntityHandle, Entry> synchronized;
    synchronized.reserve(m_entries.size());
    m_registry
        ->Query<ECS::Read<Bounds>,
                 ECS::Read<RenderWorldTransform>,
                 ECS::Read<Active>,
                 ECS::Read<Layer>,
                 ECS::Read<EntityLifecycleState>>()
        .Each([this, &synchronized, &stats](ECS::EntityHandle entity,
                                            const Bounds& bounds,
                                            const RenderWorldTransform& transform,
                                            const Active& active,
                                            const Layer& layer,
                                            const EntityLifecycleState& lifecycle)
        {
            if (!active.value || lifecycle.phase != EntityLifecyclePhase::Alive)
            {
                return;
            }

            Entry entry;
            entry.value.id = {.sceneRuntimeId = m_sceneRuntimeId, .entity = entity};
            entry.value.worldBounds = MakeWorldBounds(bounds.center, bounds.extents, transform.matrix);
            entry.value.layer = layer.value;
            entry.boundsWriteVersion = m_registry->GetFragmentWriteVersion<Bounds>(entity);
            entry.transformWriteVersion =
                m_registry->GetFragmentWriteVersion<RenderWorldTransform>(entity);
            entry.activeWriteVersion = m_registry->GetFragmentWriteVersion<Active>(entity);
            entry.layerWriteVersion = m_registry->GetFragmentWriteVersion<Layer>(entity);
            entry.lifecycleWriteVersion =
                m_registry->GetFragmentWriteVersion<EntityLifecycleState>(entity);

            const auto previous = m_entries.find(entity);
            if (previous == m_entries.end())
            {
                ++stats.inserted;
            }
            else if (previous->second.boundsWriteVersion != entry.boundsWriteVersion ||
                     previous->second.transformWriteVersion != entry.transformWriteVersion ||
                     previous->second.activeWriteVersion != entry.activeWriteVersion ||
                     previous->second.layerWriteVersion != entry.layerWriteVersion ||
                     previous->second.lifecycleWriteVersion != entry.lifecycleWriteVersion ||
                     !SameBounds(previous->second.value.worldBounds, entry.value.worldBounds) ||
                     previous->second.value.layer != entry.value.layer)
            {
                ++stats.updated;
            }
            else
            {
                ++stats.unchanged;
            }

            synchronized.emplace(entity, std::move(entry));
        });

    for (const auto& [entity, ignored] : m_entries)
    {
        (void)ignored;
        if (!synchronized.contains(entity))
        {
            ++stats.removed;
        }
    }

    m_entries.swap(synchronized);
    stats.indexed = static_cast<uint32>(m_entries.size());
    return stats;
}

std::vector<SpatialQueryResult> SceneSpatialIndex::QueryAabb(const AABB& bounds,
                                                              uint32 layerMask) const
{
    std::vector<SpatialQueryResult> results;
    if (!bounds.IsValid())
    {
        return results;
    }

    for (const auto& [entity, entry] : m_entries)
    {
        (void)entity;
        if (IsLayerSelected(entry.value.layer, layerMask) &&
            entry.value.worldBounds.Overlaps(bounds))
        {
            results.push_back(entry.value);
        }
    }
    SortResults(results);
    return results;
}

std::vector<SpatialQueryResult> SceneSpatialIndex::QueryPoint(const Vec3& point,
                                                               uint32 layerMask) const
{
    std::vector<SpatialQueryResult> results;
    for (const auto& [entity, entry] : m_entries)
    {
        (void)entity;
        if (IsLayerSelected(entry.value.layer, layerMask) &&
            entry.value.worldBounds.Contains(point))
        {
            results.push_back(entry.value);
        }
    }
    SortResults(results);
    return results;
}

bool SceneSpatialIndex::IsLayerSelected(uint32 layer, uint32 mask)
{
    return layer < 32u && (mask & (1u << layer)) != 0u;
}

AABB SceneSpatialIndex::MakeWorldBounds(const Vec3& center,
                                        const Vec3& extents,
                                        const Mat4& worldTransform)
{
    const Vec3 safeExtents = glm::max(glm::abs(extents), Vec3(0.0f));
    const AABB local(center - safeExtents, center + safeExtents);
    return local.Transformed(worldTransform);
}
} // namespace RVX::SceneECS
