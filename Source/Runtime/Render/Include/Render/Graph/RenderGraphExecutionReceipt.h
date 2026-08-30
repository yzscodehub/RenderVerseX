#pragma once

/**
 * @file RenderGraphExecutionReceipt.h
 * @brief Lifetime-owned completion facts shared across typed feature boundaries.
 */

#include <atomic>
#include <cstdint>

namespace RVX
{
    inline constexpr std::uint32_t RVX_OPAQUE_SCENE_COLOR_PYRAMID_MAX_MIP_COUNT =
        7;

    /**
     * @brief Completion fact for the late opaque-scene color pyramid.
     *
     * Frame, view, graph, and generation identity remain on the typed
     * blackboard entry and record input. The terminal pass marks this receipt
     * only after every requested mip has actually been rendered, preventing a
     * consumer from sampling a partial pyramid as if it were a valid input.
     */
    struct OpaqueSceneColorPyramidBuildReceipt
    {
        std::atomic<std::uint32_t> completedMipCount{0};
        std::atomic_bool failed{false};
        std::atomic_bool completed{false};
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
