#include "Scene/ECS/SceneSkinningSnapshotStore.h"

#include "Core/Diagnostics/SkinningPaletteHash.h"

#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace RVX::SceneECS
{
SceneSkinningSnapshotStore::SceneSkinningSnapshotStore(
    ECS::SceneRuntimeId sceneRuntimeId)
    : m_sceneRuntimeId(sceneRuntimeId)
{
}

bool SceneSkinningSnapshotStore::Publish(
    const ECS::EntityRef& meshSource,
    SceneSkinningPaletteSnapshot snapshot)
{
    if (!IsOwnedMeshSource(meshSource) || !IsValidSnapshot(snapshot))
    {
        return false;
    }

    try
    {
        const ECS::EntityHandle mesh = meshSource.GetHandle();
        m_entries.insert_or_assign(mesh, std::move(snapshot));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SceneSkinningSnapshotStore::Remove(ECS::EntityHandle entity)
{
    if (!m_sceneRuntimeId.IsValid() || !entity.IsValid())
    {
        return false;
    }

    static_cast<void>(m_entries.erase(entity));
    for (auto entry = m_entries.begin(); entry != m_entries.end();)
    {
        if (entry->second.poseEntity == entity)
        {
            entry = m_entries.erase(entry);
        }
        else
        {
            ++entry;
        }
    }
    return true;
}

bool SceneSkinningSnapshotStore::Invalidate(ECS::EntityHandle meshEntity)
{
    if (!m_sceneRuntimeId.IsValid() || !meshEntity.IsValid())
    {
        return false;
    }

    static_cast<void>(m_entries.erase(meshEntity));
    return true;
}

void SceneSkinningSnapshotStore::Clear()
{
    m_entries.clear();
}

bool SceneSkinningSnapshotStore::FreezeInto(FrozenSceneSnapshot& destination) const
{
    if (!m_sceneRuntimeId.IsValid() || destination.sceneRuntimeId != m_sceneRuntimeId ||
        destination.revision == 0)
    {
        return false;
    }

    try
    {
        std::vector<std::optional<FrozenSceneSkinningPalette>> staged;
        staged.reserve(destination.meshes.size());
        for (const FrozenSceneMesh& mesh : destination.meshes)
        {
            if (!mesh.skinningBinding.has_value())
            {
                if (mesh.skinningPalette.has_value())
                {
                    return false;
                }
                staged.emplace_back();
                continue;
            }

            const SkinnedMeshBinding& binding = *mesh.skinningBinding;
            if (mesh.id.type != FrozenSceneObjectType::Mesh ||
                !mesh.id.IsValid() || mesh.sourceEntity != mesh.id.entity ||
                !binding.poseEntity.IsValid() || binding.sourceModelAssetValue == 0 ||
                binding.sourceSkinIndex < 0)
            {
                return false;
            }

            const auto found = m_entries.find(mesh.id.entity);
            if (found == m_entries.end() || !IsValidSnapshot(found->second))
            {
                return false;
            }

            const SceneSkinningPaletteSnapshot& source = found->second;
            if (source.poseEntity != binding.poseEntity ||
                source.sourceModelAssetValue != binding.sourceModelAssetValue ||
                source.sourceSkinIndex != binding.sourceSkinIndex)
            {
                return false;
            }

            FrozenSceneSkinningPalette frozen;
            frozen.id = MakeFrozenSceneObjectId(
                m_sceneRuntimeId, mesh.id.entity, FrozenSceneObjectType::SkinningPalette);
            frozen.poseEntity = source.poseEntity;
            frozen.sourceModelAssetValue = source.sourceModelAssetValue;
            frozen.sourceSkinIndex = source.sourceSkinIndex;
            frozen.poseSequence = source.poseSequence;
            frozen.paletteRevision = source.paletteRevision;
            frozen.paletteBoneCount = source.paletteBoneCount;
            frozen.matrices = source.matrices;
            staged.emplace_back(std::move(frozen));
        }

        for (size_t index = 0; index < destination.meshes.size(); ++index)
        {
            destination.meshes[index].skinningPalette = std::move(staged[index]);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

SceneSkinningSnapshotStoreCounts SceneSkinningSnapshotStore::GetCounts() const
{
    const size_t count = m_entries.size();
    return {
        .paletteCount = count > std::numeric_limits<uint32>::max() ?
                            std::numeric_limits<uint32>::max() :
                            static_cast<uint32>(count),
    };
}

bool SceneSkinningSnapshotStore::IsOwnedMeshSource(const ECS::EntityRef& source) const
{
    return m_sceneRuntimeId.IsValid() && source.IsValid() &&
           source.GetSceneRuntimeId() == m_sceneRuntimeId &&
           source.GetHandle().IsValid();
}

bool SceneSkinningSnapshotStore::IsValidSnapshot(
    const SceneSkinningPaletteSnapshot& snapshot) const
{
    if (!snapshot.poseEntity.IsValid() || snapshot.sourceModelAssetValue == 0 ||
        snapshot.sourceSkinIndex < 0 || snapshot.poseSequence == 0 ||
        snapshot.paletteRevision == 0 || snapshot.paletteBoneCount == 0 ||
        snapshot.paletteBoneCount != snapshot.matrices.size())
    {
        return false;
    }

    const SkinningPaletteHash hash = ComputeSkinningPaletteHash(snapshot.matrices);
    return hash.IsValid() && hash.matrixCount == snapshot.paletteBoneCount;
}
} // namespace RVX::SceneECS
