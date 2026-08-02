#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Policy/RenderFramePlanCompiler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

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

    enum class PacketPlanShape : uint8
    {
        Invalid = 0,
        Hybrid,
        GPUWithDirectSkipped,
        AllDirect,
        AllSkip,
    };

    [[nodiscard]] PacketPlanShape ClassifyPacketPlanShape(
        const RenderPassExecutionPlan& passPlan,
        const MeshPassPacketStream& stream) noexcept
    {
        const uint32 candidates = stream.stats.gpuCandidatePacketCount;
        const uint32 direct = stream.stats.directPacketCount;
        const uint32 skipped = stream.stats.skippedPacketCount;
        const RenderPacketPartitionSummary& partition = passPlan.partition;
        if (candidates != 0 &&
            partition.gpuDrivenPacketCount == candidates &&
            partition.directPacketCount == direct &&
            partition.skippedPacketCount == skipped)
        {
            return PacketPlanShape::Hybrid;
        }
        if (candidates != 0 &&
            partition.gpuDrivenPacketCount == candidates &&
            partition.directPacketCount == 0 &&
            partition.skippedPacketCount == skipped + direct)
        {
            return PacketPlanShape::GPUWithDirectSkipped;
        }
        if (partition.gpuDrivenPacketCount == 0 &&
            partition.directPacketCount == candidates + direct &&
            partition.skippedPacketCount == skipped)
        {
            return PacketPlanShape::AllDirect;
        }
        if (partition.gpuDrivenPacketCount == 0 &&
            partition.directPacketCount == 0 &&
            partition.skippedPacketCount == partition.inputPacketCount)
        {
            return PacketPlanShape::AllSkip;
        }
        return PacketPlanShape::Invalid;
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
    const PacketPlanShape shape = passPlan != nullptr
        ? ClassifyPacketPlanShape(*passPlan, stream)
        : PacketPlanShape::Invalid;
    const uint32 directCount = passPlan != nullptr ? passPlan->directPackets.count : 0u;

    if (passPlan == nullptr || shape == PacketPlanShape::Invalid ||
        !ValidateMeshPassPacketStream(stream) ||
        passPlan->directPackets.count != passPlan->partition.directPacketCount ||
        !IsRangeInBounds(passPlan->directPackets,
                         plan.packetReferences.size()) ||
        !IsRangeInBounds(passPlan->gpuEligiblePackets,
                         plan.packetReferences.size()) ||
        !IsRangeInBounds(passPlan->skippedPackets,
                         plan.packetReferences.size()) ||
        stream.packets.size() != stream.stats.inputPacketCount ||
        passPlan->partition.inputPacketCount != stream.stats.inputPacketCount ||
        passPlan->partition.relevantPacketCount !=
            stream.stats.relevantPacketCount ||
        passPlan->partition.candidatePacketCount !=
            stream.stats.gpuCandidatePacketCount ||
        !stream.stats.HasCompleteRelevantOutcome() ||
        !stream.stats.HasExactlyOneReasonPerRejectedPacket() ||
        stream.stats.relevantPacketCount + stream.stats.skippedPacketCount !=
            stream.stats.inputPacketCount)
    {
        return result;
    }

    std::vector<bool> seenSourceIndices(stream.packets.size(), false);
    DirectDrawPacketBatch batch;
    batch.pass = pass;
    batch.packets.reserve(directCount);
    const bool allowGPUCandidatesInDirectLane =
        shape == PacketPlanShape::AllDirect;
    for (uint32 offset = 0; offset < passPlan->directPackets.count; ++offset)
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
        if ((source.disposition != MeshPassDisposition::Direct &&
             !(allowGPUCandidatesInDirectLane &&
               source.disposition == MeshPassDisposition::GPUCandidate)) ||
            !RenderDrawPacketReferenceMatchesSource(plan,
                                                  reference,
                                                  source) ||
            !IsValidDirectLayout(source.directLayout) ||
            (source.disposition == MeshPassDisposition::Direct &&
             source.groupKey.layout != source.directLayout))
        {
            return result;
        }

        seenSourceIndices[reference.sourcePacketIndex] = true;
        if (source.disposition == MeshPassDisposition::Direct ||
            source.disposition == MeshPassDisposition::GPUCandidate)
        {
            batch.packets.push_back(DirectDrawPacket{
                source.packet,
                source.directLayout,
                reference.sourcePacketIndex,
                reference.sourceOrdinal,
            });
        }
    }

    for (uint32 offset = 0; offset < passPlan->gpuEligiblePackets.count; ++offset)
    {
        if (offset >= stream.sortedGPUCandidatePacketIndices.size())
        {
            return result;
        }
        const RenderDrawPacketReference& reference =
            plan.packetReferences[static_cast<size_t>(
                static_cast<uint64>(passPlan->gpuEligiblePackets.first) + offset)];
        const uint32 sourceIndex = reference.sourcePacketIndex;
        if (sourceIndex != stream.sortedGPUCandidatePacketIndices[offset] ||
            sourceIndex >= stream.packets.size() ||
            seenSourceIndices[sourceIndex] ||
            stream.packets[sourceIndex].disposition !=
                MeshPassDisposition::GPUCandidate ||
            !RenderDrawPacketReferenceMatchesSource(
                plan, reference, stream.packets[sourceIndex]))
        {
            return result;
        }
        seenSourceIndices[sourceIndex] = true;
    }

    for (uint32 offset = 0; offset < passPlan->skippedPackets.count; ++offset)
    {
        const RenderDrawPacketReference& reference =
            plan.packetReferences[static_cast<size_t>(
                static_cast<uint64>(passPlan->skippedPackets.first) + offset)];
        const uint32 sourceIndex = reference.sourcePacketIndex;
        const MeshPassProcessorResult& source =
            sourceIndex < stream.packets.size() ? stream.packets[sourceIndex]
                                                : MeshPassProcessorResult{};
        if (reference.pass != pass ||
            sourceIndex >= stream.packets.size() ||
            seenSourceIndices[sourceIndex] ||
            (shape == PacketPlanShape::AllSkip
                 ? false
                 : (source.disposition != MeshPassDisposition::Skip &&
                    !(shape == PacketPlanShape::GPUWithDirectSkipped &&
                      source.disposition == MeshPassDisposition::Direct))) ||
            !RenderDrawPacketReferenceMatchesSource(plan, reference, source))
        {
            return result;
        }
        seenSourceIndices[sourceIndex] = true;
    }

    if (!std::all_of(seenSourceIndices.begin(),
                     seenSourceIndices.end(),
                     [](bool value) { return value; }))
    {
        return result;
    }

    result.succeeded = true;
    result.reason = RenderPolicyReason::None;
    result.batch = std::move(batch);
    return result;
}

bool ValidatePlannedGPUDrivenPacketRange(
    const RenderFrameExecutionPlan& plan,
    RenderPassKind pass,
    const MeshPassPacketStream& stream)
{
    if (!IsCanonicalPass(pass) || !IsCanonicalPlan(plan) ||
        !ValidateRenderFrameExecutionPlan(plan) ||
        !ValidateMeshPassPacketStream(stream))
    {
        return false;
    }

    const RenderPassExecutionPlan* passPlan = FindPassPlan(plan, pass);
    const PacketPlanShape shape = passPlan != nullptr
        ? ClassifyPacketPlanShape(*passPlan, stream)
        : PacketPlanShape::Invalid;
    if (passPlan == nullptr ||
        (shape != PacketPlanShape::Hybrid &&
         shape != PacketPlanShape::GPUWithDirectSkipped) ||
        passPlan->preferredSubmission == RenderSubmissionMode::Direct ||
        passPlan->visibility == RenderVisibilityMode::Cpu ||
        passPlan->partition.inputPacketCount != stream.stats.inputPacketCount ||
        passPlan->partition.relevantPacketCount !=
            stream.stats.relevantPacketCount ||
        passPlan->partition.candidatePacketCount !=
            stream.stats.gpuCandidatePacketCount ||
        passPlan->partition.drawGroupCount != stream.groups.size() ||
        passPlan->gpuEligiblePackets.count !=
            stream.sortedGPUCandidatePacketIndices.size() ||
        passPlan->gpuEligiblePackets.count !=
            passPlan->partition.gpuDrivenPacketCount ||
        passPlan->directPackets.count != passPlan->partition.directPacketCount ||
        passPlan->skippedPackets.count != passPlan->partition.skippedPacketCount ||
        !IsRangeInBounds(passPlan->gpuEligiblePackets,
                         plan.packetReferences.size()) ||
        !IsRangeInBounds(passPlan->directPackets,
                         plan.packetReferences.size()) ||
        !IsRangeInBounds(passPlan->skippedPackets,
                         plan.packetReferences.size()))
    {
        return false;
    }

    std::vector<bool> seenSourceIndices(stream.packets.size(), false);
    for (uint32 offset = 0; offset < passPlan->gpuEligiblePackets.count; ++offset)
    {
        const uint32 sourceIndex =
            stream.sortedGPUCandidatePacketIndices[offset];
        const RenderDrawPacketReference& reference =
            plan.packetReferences[static_cast<size_t>(
                static_cast<uint64>(passPlan->gpuEligiblePackets.first) + offset)];
        if (seenSourceIndices[sourceIndex] ||
            reference.sourcePacketIndex != sourceIndex ||
            !RenderDrawPacketReferenceMatchesSource(plan,
                                                   reference,
                                                   stream.packets[sourceIndex]))
        {
            return false;
        }
        seenSourceIndices[sourceIndex] = true;
    }

    for (uint32 offset = 0; offset < passPlan->directPackets.count; ++offset)
    {
        const RenderDrawPacketReference& reference =
            plan.packetReferences[static_cast<size_t>(
                static_cast<uint64>(passPlan->directPackets.first) + offset)];
        const uint32 sourceIndex = reference.sourcePacketIndex;
        if (sourceIndex >= stream.packets.size() ||
            seenSourceIndices[sourceIndex] ||
            stream.packets[sourceIndex].disposition !=
                MeshPassDisposition::Direct ||
            !RenderDrawPacketReferenceMatchesSource(plan,
                                                  reference,
                                                  stream.packets[sourceIndex]))
        {
            return false;
        }
        seenSourceIndices[sourceIndex] = true;
    }

    for (uint32 offset = 0; offset < passPlan->skippedPackets.count; ++offset)
    {
        const RenderDrawPacketReference& reference =
            plan.packetReferences[
                static_cast<size_t>(passPlan->skippedPackets.first) + offset];
        const uint32 sourceIndex = reference.sourcePacketIndex;
        if (reference.pass != pass ||
            sourceIndex >= stream.packets.size() ||
            seenSourceIndices[sourceIndex] ||
            (stream.packets[sourceIndex].disposition != MeshPassDisposition::Skip &&
             !(shape == PacketPlanShape::GPUWithDirectSkipped &&
               stream.packets[sourceIndex].disposition ==
                   MeshPassDisposition::Direct)) ||
            !RenderDrawPacketReferenceMatchesSource(plan,
                                                   reference,
                                                   stream.packets[sourceIndex]))
        {
            return false;
        }
        seenSourceIndices[sourceIndex] = true;
    }

    if (!std::all_of(seenSourceIndices.begin(),
                     seenSourceIndices.end(),
                     [](bool value) { return value; }))
    {
        return false;
    }

    return true;
}

} // namespace RVX
