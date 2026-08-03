#pragma once

/**
 * @file GPUSceneUpdate.h
 * @brief Accepted RenderScene to persistent CPU GPU-scene-shadow publication.
 */

#include "GPUScene/GPUSceneDatabase.h"
#include "Render/GPUScene/GPUScenePublication.h"
#include "RenderContracts/RenderIdentity.h"

#include <unordered_map>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class RenderScene;

    /**
     * @brief Maintains a non-executable, exact-generation CPU mirror.
     *
     * This object has no RHI ownership and must remain behind SceneRenderer's
     * renderer-private boundary.  Its committed values are diagnostics/future
     * Tier 2 input only; direct and Tier 1 rendering never consume them.
     */
    class GPUSceneUpdate final : public NonMovable
    {
    public:
        /** @brief Private-cache value retained only by this renderer-private type. */
        struct PublishedObject
        {
            GPUSceneObjectData data;
            std::vector<RenderResourceHandle> requiredResources;
        };

        [[nodiscard]] GPUScenePublicationStats Publish(
            const RenderScene& scene,
            const RenderResourceRegistry& registry);
        [[nodiscard]] GPUScenePublicationStats Revalidate(
            const RenderResourceRegistry& registry);
        void Clear() noexcept;

        /** @brief Render-private deterministic failure seams for focused validation. */
        void SetDatabasePrepareAllocationFailureCountdownForTesting(
            int32 countdown) noexcept;
        void SetThrowOnPublishForTesting(bool enabled) noexcept;

        /** @brief Records a renderer-boundary failure without changing the mirror. */
        void RecordFailure(
            uint64 sourceSequence,
            GPUScenePublicationFailureReason reason) noexcept;
        void RecordUnexpectedFailure(uint64 sourceSequence) noexcept;

        [[nodiscard]] const GPUScenePublicationStats& GetStats() const noexcept
        {
            return m_stats;
        }

        /** @brief Render-private validation view; never an execution input. */
        [[nodiscard]] const GPUSceneCommittedMirror&
            GetCommittedMirrorForTesting() const noexcept
        {
            return m_database.GetCommittedMirror();
        }

    private:
        [[nodiscard]] GPUScenePublicationStats PublishImpl(
            const RenderScene& scene,
            const RenderResourceRegistry& registry);
        void PopulateCommittedIdentity(GPUScenePublicationStats& stats) const noexcept;
        [[nodiscard]] uint32 GetPublishedDrawCount() const noexcept;
        [[nodiscard]] bool IsEquivalent(
            const PublishedObject& lhs,
            const PublishedObject& rhs) const noexcept;
        [[nodiscard]] bool AreExactDependenciesReady(
            const std::vector<RenderResourceHandle>& roots,
            const RenderResourceRegistry& registry,
            GPUScenePublicationFailureReason& outReason) const;

        GPUSceneDatabase m_database;
        std::unordered_map<uint64, PublishedObject> m_publishedObjects;
        GPUScenePublicationStats m_stats;
        uint64 m_committedSourceSequence = 0;
        bool m_throwOnPublishForTesting = false;
    };
} // namespace RVX
