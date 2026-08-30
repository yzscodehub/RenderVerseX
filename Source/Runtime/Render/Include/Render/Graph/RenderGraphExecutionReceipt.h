#pragma once

/**
 * @file RenderGraphExecutionReceipt.h
 * @brief Lifetime-owned completion facts shared across typed feature boundaries.
 */

#include <atomic>

namespace RVX
{
    /**
     * @brief Completion fact for the immutable HDR copy used by translucent
     *        consumers.
     *
     * Frame, view, graph, and generation identity remain on the typed
     * blackboard entry and record input.  The receipt reports only actual GPU
     * command execution and therefore cannot be fabricated by graph setup.
     */
    struct OpaqueSceneSnapshotCopyReceipt
    {
        std::atomic_bool copied{false};
    };

    /**
     * @brief Completion fact for the immutable D32 snapshot shared by water
     *        and particle soft-depth consumers.
     *
     * The typed SceneDepthSnapshot carries the graph/frame/view provenance;
     * this receipt is intentionally limited to the copy command's execution
     * result so neither consumer can claim a prepared graph resource was
     * actually populated before it samples it.
     */
    struct SceneDepthSnapshotCopyReceipt
    {
        std::atomic_bool copied{false};
    };
} // namespace RVX
