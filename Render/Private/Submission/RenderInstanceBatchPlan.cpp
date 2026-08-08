#include "Render/Submission/RenderInstanceBatchPlan.h"
#include "Render/Visibility/RenderVisibility.h"

#include <algorithm>
#include <tuple>

namespace RVX
{
namespace
{
    [[nodiscard]] bool HasFlag(RenderDrawFlags flags,
                               RenderDrawFlags flag) noexcept
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    [[nodiscard]] RenderInstanceBatchKey MakeBatchKey(
        const DirectDrawPacket& direct) noexcept
    {
        return MakeRenderInstanceBatchKey(direct.packet, direct.layout);
    }

    [[nodiscard]] RenderInstanceBatchReason GetEligibilityReason(
        const DirectDrawPacket& direct) noexcept
    {
        const RenderDrawPacket& packet = direct.packet;
        if (packet.pass != RenderPassKind::Opaque &&
            packet.pass != RenderPassKind::Depth &&
            packet.pass != RenderPassKind::Shadow)
        {
            return packet.pass == RenderPassKind::Transparent
                ? RenderInstanceBatchReason::Transparent
                : RenderInstanceBatchReason::UnsupportedPass;
        }
        if (HasFlag(packet.flags, RenderDrawFlags::Transparent) ||
            packet.materialKey.materialMode == MaterialRenderMode::Transparent)
        {
            return RenderInstanceBatchReason::Transparent;
        }
        if (HasFlag(packet.flags, RenderDrawFlags::Skinned) ||
            packet.pipelineKey.skinned)
        {
            return RenderInstanceBatchReason::Skinned;
        }
        if (HasFlag(packet.flags, RenderDrawFlags::Masked) ||
            HasFlag(packet.flags, RenderDrawFlags::MissingMaterial) ||
            packet.pipelineKey.materialVariant != MaterialPipelineVariant::Opaque ||
            packet.materialKey.materialMode != MaterialRenderMode::Opaque)
        {
            return RenderInstanceBatchReason::SpecialMaterial;
        }
        if (packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            return RenderInstanceBatchReason::InvalidDrawArguments;
        }
        return RenderInstanceBatchReason::None;
    }

    [[nodiscard]] bool MemberLess(const RenderInstanceBatchMember& lhs,
                                  const RenderInstanceBatchMember& rhs) noexcept
    {
        return lhs.packetId < rhs.packetId;
    }
} // namespace

RenderInstanceBatchPlan BuildRenderInstanceBatchPlan(
    const DirectDrawPacketBatch& directBatch,
    RenderInstancingMode mode)
{
    RenderInstanceBatchPlan plan;
    plan.pass = directBatch.pass;
    plan.executedPacketCount = static_cast<uint32>(directBatch.packets.size());
    plan.submittedInstanceCount = plan.executedPacketCount;

    struct Candidate
    {
        RenderInstanceBatchKey key{};
        RenderInstanceBatchMember member{};
        RenderInstanceBatchReason reason = RenderInstanceBatchReason::None;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(directBatch.packets.size());
    for (uint32 index = 0;
         index < static_cast<uint32>(directBatch.packets.size());
         ++index)
    {
        const DirectDrawPacket& direct = directBatch.packets[index];
        Candidate candidate;
        candidate.key = MakeBatchKey(direct);
        candidate.member = {direct.packetId, index};
        candidate.reason = mode == RenderInstancingMode::Disabled
            ? RenderInstanceBatchReason::ModeDisabled
            : GetEligibilityReason(direct);
        candidates.push_back(std::move(candidate));
    }

    const RenderDrawGroupKeyLess keyLess;
    std::sort(candidates.begin(), candidates.end(),
        [&keyLess](const Candidate& lhs, const Candidate& rhs)
        {
            if (keyLess(lhs.key, rhs.key))
            {
                return true;
            }
            if (keyLess(rhs.key, lhs.key))
            {
                return false;
            }
            if (lhs.reason != rhs.reason)
            {
                return static_cast<uint8>(lhs.reason) <
                       static_cast<uint8>(rhs.reason);
            }
            return MemberLess(lhs.member, rhs.member);
        });

    for (const Candidate& candidate : candidates)
    {
        const bool eligible = candidate.reason == RenderInstanceBatchReason::None;
        if (eligible && !plan.batches.empty() &&
            plan.batches.back().reason == RenderInstanceBatchReason::None &&
            plan.batches.back().key == candidate.key)
        {
            plan.batches.back().members.push_back(candidate.member);
            continue;
        }

        RenderInstanceBatch batch;
        batch.key = candidate.key;
        batch.reason = candidate.reason;
        batch.members.push_back(candidate.member);
        plan.batches.push_back(std::move(batch));
    }

    uint32 nextInstance = 0;
    for (RenderInstanceBatch& batch : plan.batches)
    {
        std::sort(batch.members.begin(), batch.members.end(), MemberLess);
        if (batch.reason == RenderInstanceBatchReason::None &&
            batch.members.size() > 1)
        {
            batch.instanced = true;
            batch.firstInstance = nextInstance;
            nextInstance += static_cast<uint32>(batch.members.size());
            ++plan.instancedBatchCount;
        }
        else if (batch.reason == RenderInstanceBatchReason::None)
        {
            batch.reason = RenderInstanceBatchReason::Singleton;
        }
    }
    plan.submittedDrawCount = static_cast<uint32>(plan.batches.size());
    return plan;
}

SceneRenderInstanceBatchPlans BuildSceneRenderInstanceBatchPlans(
    const RenderFrameExecutionPlan& framePlan,
    const SceneMeshPassPreparation& preparation,
    const RenderVisibilityResult& visibility,
    RenderInstancingMode mode)
{
    SceneRenderInstanceBatchPlans plans;
    const DirectDrawPacketBatchBuildResult depth = BuildDirectDrawPacketBatch(
        framePlan, RenderPassKind::Depth, preparation.depth, &visibility);
    const DirectDrawPacketBatchBuildResult opaque = BuildDirectDrawPacketBatch(
        framePlan, RenderPassKind::Opaque, preparation.opaque, &visibility);
    const DirectDrawPacketBatchBuildResult shadow = BuildDirectDrawPacketBatch(
        framePlan, RenderPassKind::Shadow, preparation.shadow, nullptr);
    if (depth.succeeded)
    {
        plans.depth = BuildRenderInstanceBatchPlan(depth.batch, mode);
        plans.depthValid = plans.depth.IsComplete();
    }
    if (opaque.succeeded)
    {
        plans.opaque = BuildRenderInstanceBatchPlan(opaque.batch, mode);
        plans.opaqueValid = plans.opaque.IsComplete();
    }
    if (shadow.succeeded)
    {
        plans.shadow = BuildRenderInstanceBatchPlan(shadow.batch, mode);
        plans.shadowValid = plans.shadow.IsComplete();
    }
    plans.structurallyValid = plans.depthValid && plans.opaqueValid &&
                              plans.shadowValid;
    return plans;
}
} // namespace RVX
