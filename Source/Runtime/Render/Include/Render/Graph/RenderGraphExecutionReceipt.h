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
} // namespace RVX
