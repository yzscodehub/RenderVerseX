#pragma once

/**
 * @file SceneSkinningSnapshotStore.h
 * @brief Scene-owned value palette cache copied into frozen ECS snapshots.
 */

#include "Scene/ECS/FrozenSceneSnapshot.h"

#include <unordered_map>
#include <vector>

namespace RVX::SceneECS
{
    /** @brief Complete value palette published for one exact skinned mesh source. */
    struct SceneSkinningPaletteSnapshot
    {
        ECS::EntityHandle poseEntity = ECS::EntityHandle::Invalid();
        uint64 sourceModelAssetValue = 0;
        int32 sourceSkinIndex = -1;
        uint64 poseSequence = 0;
        uint64 paletteRevision = 0;
        uint32 paletteBoneCount = 0;
        std::vector<Mat4> matrices;
    };

    /** @brief Point-in-time count for diagnostics and focused validation. */
    struct SceneSkinningSnapshotStoreCounts
    {
        uint32 paletteCount = 0;
    };

    /**
     * @brief Owner-thread cache of animation-produced palettes for one Scene runtime.
     *
     * Entries are indexed by the exact generation-qualified mesh handle. They
     * retain only values, and FreezeInto validates every live skinned mesh
     * against its recorded pose owner and source identity before copying it.
     */
    class SceneSkinningSnapshotStore final
    {
    public:
        explicit SceneSkinningSnapshotStore(ECS::SceneRuntimeId sceneRuntimeId);

        [[nodiscard]] bool Publish(const ECS::EntityRef& meshSource,
                                   SceneSkinningPaletteSnapshot snapshot);

        /** @brief Erase only one mesh-keyed palette when Feature data becomes incoherent. */
        [[nodiscard]] bool Invalidate(ECS::EntityHandle meshEntity);

        /**
         * @brief Remove an exact mesh entry and every palette whose pose owner matches it.
         *
         * End-frame cleanup may call this after the Registry no longer exposes
         * a live EntityRef, so the operation intentionally takes a handle.
         */
        [[nodiscard]] bool Remove(ECS::EntityHandle entity);
        void Clear();

        /**
         * @brief Deep-copy validated palette values into an immutable Scene snapshot.
         *
         * Every live mesh with a SkinnedMeshBinding must have one matching,
         * finite, complete entry. On failure destination remains unchanged.
         * Entries for meshes excluded from the snapshot are intentionally
         * ignored so pending-destroy cleanup may happen after presentation.
         */
        [[nodiscard]] bool FreezeInto(FrozenSceneSnapshot& destination) const;

        [[nodiscard]] SceneSkinningSnapshotStoreCounts GetCounts() const;

    private:
        [[nodiscard]] bool IsOwnedMeshSource(const ECS::EntityRef& source) const;
        [[nodiscard]] bool IsValidSnapshot(
            const SceneSkinningPaletteSnapshot& snapshot) const;

        ECS::SceneRuntimeId m_sceneRuntimeId;
        std::unordered_map<ECS::EntityHandle, SceneSkinningPaletteSnapshot> m_entries;
    };
} // namespace RVX::SceneECS
