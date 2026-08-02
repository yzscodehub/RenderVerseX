#include "Render/Passes/DirectDrawPacketBatch.h"

#include <array>
#include <cstdint>

namespace RVX
{
namespace
{
    constexpr std::array<RenderPassKind, 4> kCanonicalPassOrder = {
        RenderPassKind::Depth,
        RenderPassKind::Opaque,
        RenderPassKind::Shadow,
        RenderPassKind::Transparent,
    };

    [[nodiscard]] bool IsValidDirectLayout(
        const RenderSubmissionLayout& layout) noexcept
    {
        const uint32 streams = static_cast<uint32>(layout.vertexStreams);
        const uint32 instanceIndex = static_cast<uint32>(
            MeshPassVertexStreams::InstanceIndex);
        const uint32 position = static_cast<uint32>(
            MeshPassVertexStreams::Position);
        return layout.primitiveDataBinding ==
               PrimitiveDataBinding::PerDrawConstants &&
               (streams & instanceIndex) == 0 &&
               (streams & position) != 0;
    }

    [[nodiscard]] bool IsCanonicalPass(RenderPassKind pass) noexcept
    {
        for (RenderPassKind canonical : kCanonicalPassOrder)
        {
            if (canonical == pass)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] const RenderPassExecutionPlan* FindPassPlan(
        const RenderFrameExecutionPlan& plan,
        RenderPassKind pass) noexcept
    {
        for (const RenderPassExecutionPlan& candidate : plan.passes)
        {
            if (candidate.pass == pass)
            {
                return &candidate;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool IsCanonicalPlan(
        const RenderFrameExecutionPlan& plan) noexcept
    {
        if (plan.passes.size() != kCanonicalPassOrder.size())
        {
            return false;
        }
        for (size_t index = 0; index < kCanonicalPassOrder.size(); ++index)
        {
            if (plan.passes[index].pass != kCanonicalPassOrder[index])
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool IsRangeInBounds(
        DrawPacketRange range,
        size_t size) noexcept
    {
        return static_cast<uint64>(range.first) + range.count <= size;
    }
} // namespace

DirectDrawPacketBatchBuildResult BuildDirectDrawPacketBatch(
    const RenderFrameExecutionPlan& plan,
    RenderPassKind pass,
    const MeshPassPacketStream& stream)
{
    DirectDrawPacketBatchBuildResult result;
    if (!IsCanonicalPass(pass) || !IsCanonicalPlan(plan) ||
        !ValidateRenderFrameExecutionPlan(plan))
    {
        return result;
    }

    const RenderPassExecutionPlan* passPlan = FindPassPlan(plan, pass);
    if (passPlan == nullptr || passPlan->preferredSubmission !=
                                   RenderSubmissionMode::Direct ||
        passPlan->partition.gpuDrivenPacketCount != 0 ||
        passPlan->directPackets.count !=
            passPlan->partition.directPacketCount ||
        !IsRangeInBounds(passPlan->directPackets,
                         plan.packetReferences.size()) ||
        stream.packets.size() != stream.stats.inputPacketCount ||
        !stream.stats.HasCompleteRelevantOutcome() ||
        !stream.stats.HasExactlyOneReasonPerRejectedPacket() ||
        stream.stats.relevantPacketCount + stream.stats.skippedPacketCount !=
            stream.stats.inputPacketCount)
    {
        return result;
    }

    const uint32 directCount = passPlan->directPackets.count;
    if (directCount > stream.packets.size())
    {
        return result;
    }

    std::vector<bool> seenSourceIndices(stream.packets.size(), false);
    DirectDrawPacketBatch batch;
    batch.pass = pass;
    batch.packets.reserve(directCount);
    for (uint32 offset = 0; offset < directCount; ++offset)
    {
        const uint64 referenceIndex =
            static_cast<uint64>(passPlan->directPackets.first) + offset;
        const RenderDrawPacketReference& reference =
            plan.packetReferences[static_cast<size_t>(referenceIndex)];
        if (reference.pass != pass ||
            reference.sourcePacketIndex >= stream.packets.size() ||
            seenSourceIndices[reference.sourcePacketIndex])
        {
            return result;
        }

        const MeshPassProcessorResult& source =
            stream.packets[reference.sourcePacketIndex];
        if (source.disposition == MeshPassDisposition::Skip ||
            source.packet.pass != pass ||
            source.sourceOrdinal != reference.sourceOrdinal ||
            !IsValidDirectLayout(source.directLayout))
        {
            return result;
        }

        // A true Direct source already carries the same layout in its group
        // key.  A GPU candidate may be here only as Task5 whole-pass fallback;
        // its GPU group layout is intentionally not compared.
        if (source.disposition == MeshPassDisposition::Direct &&
            source.groupKey.layout != source.directLayout)
        {
            return result;
        }

        seenSourceIndices[reference.sourcePacketIndex] = true;
        batch.packets.push_back(DirectDrawPacket{
            source.packet,
            source.directLayout,
            reference.sourcePacketIndex,
            reference.sourceOrdinal,
        });
    }

    // The Direct lane must contain every relevant source packet exactly once.
    // This protects consumers from malformed plans that happen to have a
    // numerically valid range but omit a source packet.
    uint32 expectedRelevant = 0;
    for (const MeshPassProcessorResult& source : stream.packets)
    {
        if (source.disposition != MeshPassDisposition::Skip)
        {
            ++expectedRelevant;
        }
    }
    if (expectedRelevant != directCount)
    {
        return result;
    }
    if (expectedRelevant != stream.stats.relevantPacketCount)
    {
        return result;
    }

    result.succeeded = true;
    result.reason = RenderPolicyReason::None;
    result.batch = std::move(batch);
    return result;
}
} // namespace RVX
