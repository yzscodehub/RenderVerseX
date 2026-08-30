#pragma once

/**
 * @file SceneDepthSnapshotBuilder.h
 * @brief Single graph-owned sampled-depth producer for special surfaces.
 */

#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Passes/RenderPassRecordContext.h"

#include <string>

namespace RVX
{
    /** Resolved union of executable Water/Particle sampled-depth demand. */
    struct SceneDepthSnapshotDemand
    {
        bool valid = false;
        bool requested = false;
        bool required = false;
        std::string failureReason{};
    };

    /**
     * @brief Resolve whether one shared copy is needed without admitting an
     *        unavailable required consumer or copying for skipped optional work.
     */
    [[nodiscard]] SceneDepthSnapshotDemand ResolveSceneDepthSnapshotDemand(
        bool waterDepthRequired,
        bool waterExecutable,
        bool particleDepthRequested,
        bool particleDepthRequired,
        bool particleExecutable);

    /**
     * @brief Creates the one full-resolution D32 copy shared by Water and
     *        Particle for the current special-surface composition.
     *
     * The caller owns blackboard publication because the selected Forward or
     * Deferred feature is the semantic producer. This helper only owns the
     * graph allocation/copy and the exact value contract passed to consumers.
     */
    [[nodiscard]] bool BuildSceneDepthSnapshot(
        RenderFeatureGraphBuilder& builder,
        const RenderPassRecordContext& context,
        RenderFrameIdentity frameIdentity,
        RenderViewIdentity viewIdentity,
        RenderOutputExtent expectedExtent,
        bool required,
        SceneDepthSnapshot& outSnapshot,
        SceneDepthSnapshotRecordInput& outRecordInput,
        std::string& outFailure);
} // namespace RVX
