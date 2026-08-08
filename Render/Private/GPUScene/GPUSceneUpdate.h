#pragma once

/**
 * @file GPUSceneUpdate.h
 * @brief Accepted RenderScene to persistent CPU GPUScene publication.
 */

#include "GPUScene/GPUSceneDatabase.h"
#include "Render/GPUScene/GPUScenePublication.h"
#include "RenderContracts/RenderIdentity.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class RenderScene;
    struct RenderDrawPacket;
    struct RenderVisibilityCandidate;

    /** @brief Exact CPU-side stable table references for one accepted draw packet. */
    struct GPUSceneAcceptedDrawLookup
    {
        uint64 committedVersion = 0;
        GPUScenePrimitiveRef primitive;
        GPUSceneDrawRef draw;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return committedVersion != 0 && primitive.IsValid() && draw.IsValid();
        }
    };

    /**
     * @brief Maintains the exact-generation CPU source for resident GPUScene rows.
     *
     * This object has no RHI ownership and must remain behind SceneRenderer's
     * renderer-private boundary. Its committed values feed the GPUScene uploader
     * and Tier 2 execution; direct and Tier 1 rendering never consume them.
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
        /** @brief Publish only changed/removed retained object identities. */
        [[nodiscard]] GPUScenePublicationStats PublishIncremental(
            const RenderScene& scene,
            std::span<const uint64> changedObjectIds,
            std::span<const uint64> removedObjectIds,
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

        /** @brief Value-only CPU mirror and allocator lifecycle snapshot. */
        [[nodiscard]] GPUSceneDiagnostics GetDiagnostics() const noexcept
        {
            return m_database.GetDiagnostics();
        }

        /** @brief Render-private validation view of the CPU publication. */
        [[nodiscard]] const GPUSceneCommittedMirror&
            GetCommittedMirrorForTesting() const noexcept
        {
            return m_database.GetCommittedMirror();
        }

        /** @brief Renderer-private upload input; never a shader or policy input. */
        [[nodiscard]] const GPUSceneCommittedMirror&
            GetCommittedMirrorForUpload() const noexcept
        {
            return m_database.GetCommittedMirror();
        }

        /** @brief Exact delta for the current committed mirror generation. */
        [[nodiscard]] const GPUSceneChangeSet&
            GetLastChangeSetForUpload() const noexcept
        {
            return m_database.GetLastChangeSet();
        }

        /** @brief Admit retired identities only after real GPU completion. */
        [[nodiscard]] bool ReclaimRetiredThrough(uint64 safeVersion)
        {
            return m_database.ReclaimRetiredThrough(safeVersion);
        }

        /**
         * @brief Resolve one accepted pass candidate to exact live scene-table refs.
         *
         * This is a renderer-private value lookup only. It validates packet,
         * primitive, contiguous draw-block, and typed row linkage without
         * exposing RHI/backend state to callers.
         */
        [[nodiscard]] std::optional<GPUSceneAcceptedDrawLookup>
            ResolveAcceptedDraw(
                const RenderScene& scene,
                const RenderVisibilityCandidate& candidate,
                const RenderDrawPacket& packet) const noexcept;

    private:
        [[nodiscard]] GPUScenePublicationStats PublishImpl(
            const RenderScene& scene,
            const RenderResourceRegistry& registry);
        [[nodiscard]] GPUScenePublicationStats PublishIncrementalImpl(
            const RenderScene& scene,
            std::span<const uint64> changedObjectIds,
            std::span<const uint64> removedObjectIds,
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
