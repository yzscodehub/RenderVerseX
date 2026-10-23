#pragma once

/**
 * @file RenderInstanceBatchPlan.h
 * @brief Value-only deterministic plan for Direct-lane raster instancing.
 */

#include "Render/Passes/DirectDrawPacketBatch.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <vector>

namespace RVX
{
    struct RenderFrameExecutionPlan;
    struct RenderVisibilityResult;
    struct SceneMeshPassPreparation;
    enum class RenderInstanceBatchReason : uint8
    {
        None = 0,
        ModeDisabled,
        Singleton,
        UnsupportedPass,
        Transparent,
        Skinned,
        SpecialMaterial,
        InvalidDrawArguments,
    };

    /** @brief Direct and GPU-driven consume one canonical batch-key contract. */
    using RenderInstanceBatchKey = RenderDrawGroupKey;

    struct RenderInstanceBatchMember
    {
        RenderDrawPacketId packetId{};
        uint32 directPacketIndex = 0;

        bool operator==(const RenderInstanceBatchMember&) const = default;
    };

    /** @brief One instanced draw or one explicitly retained Direct packet. */
    struct RenderInstanceBatch
    {
        RenderInstanceBatchKey key{};
        std::vector<RenderInstanceBatchMember> members{};
        uint32 firstInstance = 0;
        RenderInstanceBatchReason reason = RenderInstanceBatchReason::None;
        bool instanced = false;

        bool operator==(const RenderInstanceBatch&) const = default;
    };

    /** @brief Immutable frame-local execution plan; owns values and no RHI refs. */
    struct RenderInstanceBatchPlan
    {
        RenderPassKind pass = RenderPassKind::None;
        std::vector<RenderInstanceBatch> batches{};
        uint32 executedPacketCount = 0;
        uint32 submittedDrawCount = 0;
        uint32 submittedInstanceCount = 0;
        uint32 instancedBatchCount = 0;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return executedPacketCount == submittedInstanceCount;
        }

        bool operator==(const RenderInstanceBatchPlan&) const = default;
    };

    /**
     * @brief Build a deterministic Direct-instancing plan from compiled packets.
     *
     * The planner never drops a packet. Unsupported packets become singleton
     * batches with an explicit reason, which is the preflight fallback path.
     */
    [[nodiscard]] RenderInstanceBatchPlan BuildRenderInstanceBatchPlan(
        const DirectDrawPacketBatch& directBatch,
        RenderInstancingMode mode);

    /** @brief Canonical per-frame plans shared by all raster passes. */
    struct SceneRenderInstanceBatchPlans
    {
        RenderInstanceBatchPlan depth{};
        RenderInstanceBatchPlan opaque{};
        RenderInstanceBatchPlan shadow{};
        bool depthValid = false;
        bool opaqueValid = false;
        bool shadowValid = false;
        bool structurallyValid = false;
    };

    [[nodiscard]] SceneRenderInstanceBatchPlans
    BuildSceneRenderInstanceBatchPlans(
        const RenderFrameExecutionPlan& framePlan,
        const SceneMeshPassPreparation& preparation,
        const RenderVisibilityResult& visibility,
        RenderInstancingMode mode);
} // namespace RVX
