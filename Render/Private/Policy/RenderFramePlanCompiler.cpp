#include "Render/Policy/RenderFramePlanCompiler.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <vector>

namespace RVX
{
RenderDrawPacketId BuildRenderDrawPacketId(
    uint64 frameSequence,
    uint32 viewOrdinal,
    RenderPassKind pass,
    uint32 sourcePacketIndex,
    const MeshPassProcessorResult& source) noexcept
{
    RenderDrawPacketId id;
    id.frameSequence = frameSequence;
    id.viewOrdinal = viewOrdinal;
    id.pass = pass;
    id.objectId = source.packet.objectId;
    id.primitiveData = source.packet.primitiveData;
    id.mesh = source.packet.geometryKey.mesh;
    id.logicalSubmeshIndex = source.packet.submeshIndex;
    id.geometrySubmeshIndex = source.packet.geometryKey.submeshIndex;
    id.sourcePacketIndex = sourcePacketIndex;
    id.sourceOrdinal = source.sourceOrdinal;
    return id;
}

RenderPreparedDrawPacketSignature BuildRenderPreparedDrawPacketSignature(
    const MeshPassProcessorResult& source) noexcept
{
    RenderPreparedDrawPacketSignature signature;
    signature.disposition = source.disposition;
    signature.reason = source.reason;
    signature.packet = source.packet;
    signature.groupKey = source.groupKey;
    signature.directLayout = source.directLayout;
    signature.sourceOrdinal = source.sourceOrdinal;
    signature.viewDepth = source.viewDepth;
    return signature;
}

bool RenderDrawPacketReferenceMatchesSource(
    const RenderFrameExecutionPlan& plan,
    const RenderDrawPacketReference& reference,
    const MeshPassProcessorResult& source) noexcept
{
    return reference.pass == source.packet.pass &&
           reference.sourceOrdinal == source.sourceOrdinal &&
           reference.packetId == BuildRenderDrawPacketId(
               plan.frameSequence,
               plan.viewOrdinal,
               reference.pass,
               reference.sourcePacketIndex,
               source) &&
           reference.sourceSignature ==
               BuildRenderPreparedDrawPacketSignature(source);
}

namespace
{
    constexpr std::array<RenderPassKind, 4> kCanonicalPassOrder = {
        RenderPassKind::Depth,
        RenderPassKind::Opaque,
        RenderPassKind::Shadow,
        RenderPassKind::Transparent,
    };

    const MeshPassPacketStream* GetStream(
        const SceneMeshPassPreparation& preparation,
        RenderPassKind pass)
    {
        switch (pass)
        {
            case RenderPassKind::Depth: return &preparation.depth;
            case RenderPassKind::Opaque: return &preparation.opaque;
            case RenderPassKind::Shadow: return &preparation.shadow;
            case RenderPassKind::Transparent: return &preparation.transparent;
            default: return nullptr;
        }
    }

    RenderPolicyReason MapSourceReason(MeshPassEligibilityReason reason)
    {
        switch (reason)
        {
            case MeshPassEligibilityReason::None:
                return RenderPolicyReason::None;
            case MeshPassEligibilityReason::PassIrrelevant:
                return RenderPolicyReason::PacketIrrelevant;
            case MeshPassEligibilityReason::PipelineUnavailable:
                return RenderPolicyReason::PipelineUnavailable;
            case MeshPassEligibilityReason::GeometryUnavailable:
            case MeshPassEligibilityReason::ResourceUnavailable:
                return RenderPolicyReason::ResourcesUnavailable;
            case MeshPassEligibilityReason::ResourcePending:
                return RenderPolicyReason::ResourcesPending;
            case MeshPassEligibilityReason::Transparent:
            case MeshPassEligibilityReason::Skinned:
            case MeshPassEligibilityReason::SpecialMaterial:
            case MeshPassEligibilityReason::UnsupportedTopology:
            case MeshPassEligibilityReason::UnsupportedIndexType:
            case MeshPassEligibilityReason::PassRequiresDirect:
                return RenderPolicyReason::PacketRequiresDirect;
            default:
                return RenderPolicyReason::InconsistentFacts;
        }
    }

    bool ValidatePreparedStream(const MeshPassPacketStream& stream)
    {
        if (stream.packets.size() > std::numeric_limits<uint32>::max() ||
            stream.packets.size() != stream.stats.inputPacketCount ||
            stream.sortedGPUCandidates.size() !=
                stream.stats.gpuCandidatePacketCount ||
            stream.sortedGPUCandidatePacketIndices.size() !=
                stream.sortedGPUCandidates.size() ||
            !stream.stats.HasCompleteRelevantOutcome())
        {
            return false;
        }

        uint32 relevant = 0;
        uint32 candidates = 0;
        uint32 direct = 0;
        uint32 skipped = 0;
        std::array<uint32,
                   static_cast<size_t>(MeshPassEligibilityReason::Count)>
            sourceReasonCounts{};
        for (const MeshPassProcessorResult& packet : stream.packets)
        {
            const size_t reasonIndex = static_cast<size_t>(packet.reason);
            if (reasonIndex >= sourceReasonCounts.size())
            {
                return false;
            }
            ++sourceReasonCounts[reasonIndex];
            switch (packet.disposition)
            {
                case MeshPassDisposition::GPUCandidate:
                    if (packet.reason != MeshPassEligibilityReason::None)
                    {
                        return false;
                    }
                    ++relevant;
                    ++candidates;
                    break;
                case MeshPassDisposition::Direct:
                    if (packet.reason == MeshPassEligibilityReason::None ||
                        packet.reason ==
                            MeshPassEligibilityReason::PassIrrelevant)
                    {
                        return false;
                    }
                    ++relevant;
                    ++direct;
                    break;
                case MeshPassDisposition::Skip:
                    if (packet.reason !=
                        MeshPassEligibilityReason::PassIrrelevant)
                    {
                        return false;
                    }
                    ++skipped;
                    break;
                default:
                    return false;
            }
        }
        if (relevant != stream.stats.relevantPacketCount ||
            candidates != stream.stats.gpuCandidatePacketCount ||
            direct != stream.stats.directPacketCount ||
            skipped != stream.stats.skippedPacketCount ||
            sourceReasonCounts != stream.stats.reasonCounts)
        {
            return false;
        }

        std::vector<bool> seen(stream.packets.size(), false);
        for (size_t sortedIndex = 0;
             sortedIndex < stream.sortedGPUCandidatePacketIndices.size();
             ++sortedIndex)
        {
            const uint32 sourceIndex =
                stream.sortedGPUCandidatePacketIndices[sortedIndex];
            if (sourceIndex >= stream.packets.size() || seen[sourceIndex])
            {
                return false;
            }
            seen[sourceIndex] = true;
            const MeshPassProcessorResult& source = stream.packets[sourceIndex];
            const MeshPassProcessorResult& sorted =
                stream.sortedGPUCandidates[sortedIndex];
            if (!source.IsGPUCandidate() ||
                source.packet != sorted.packet ||
                source.groupKey != sorted.groupKey ||
                source.directLayout != sorted.directLayout ||
                source.sourceOrdinal != sorted.sourceOrdinal ||
                source.viewDepth != sorted.viewDepth)
            {
                return false;
            }
        }

        uint32 groupCursor = 0;
        for (const RenderDrawGroupRange& group : stream.groups)
        {
            if (group.count == 0 || group.first != groupCursor ||
                static_cast<uint64>(group.first) + group.count >
                    stream.sortedGPUCandidates.size())
            {
                return false;
            }
            for (uint32 offset = 0; offset < group.count; ++offset)
            {
                if (stream.sortedGPUCandidates[group.first + offset].groupKey !=
                    group.key)
                {
                    return false;
                }
            }
            groupCursor += group.count;
        }
        return groupCursor == stream.sortedGPUCandidates.size() &&
               (stream.groups.empty() == stream.sortedGPUCandidates.empty());
    }

    bool DecisionMatchesStream(const RenderPassPolicyDecision& decision,
                               const MeshPassPacketStream& stream)
    {
        const RenderPacketPartitionSummary& partition = decision.partition;
        return partition.inputPacketCount == stream.stats.inputPacketCount &&
               partition.relevantPacketCount ==
                   stream.stats.relevantPacketCount &&
               partition.candidatePacketCount ==
                   stream.stats.gpuCandidatePacketCount &&
               (partition.gpuDrivenPacketCount == 0 ||
                partition.drawGroupCount == stream.groups.size());
    }

    bool AppendReference(RenderFrameExecutionPlan& plan,
                         RenderPassKind pass,
                         uint32 sourceIndex,
                         const MeshPassPacketStream& stream)
    {
        if (sourceIndex >= stream.packets.size() ||
            plan.packetReferences.size() >= std::numeric_limits<uint32>::max())
        {
            return false;
        }
        const MeshPassProcessorResult& source = stream.packets[sourceIndex];
        if (source.packet.pass != pass)
        {
            return false;
        }
        plan.packetReferences.push_back(RenderDrawPacketReference{
            pass,
            sourceIndex,
            source.sourceOrdinal,
            BuildRenderDrawPacketId(plan.frameSequence,
                                    plan.viewOrdinal,
                                    pass,
                                    sourceIndex,
                                    source),
            BuildRenderPreparedDrawPacketSignature(source)});
        return true;
    }

    void CountReason(RenderPassExecutionPlan& passPlan,
                     RenderPolicyReason reason)
    {
        const size_t index = static_cast<size_t>(reason);
        if (index < passPlan.reasonCounts.size())
        {
            ++passPlan.reasonCounts[index];
        }
    }

    RenderPolicyReason GetPacketOutcomeReason(
        const MeshPassProcessorResult& packet,
        bool plannedGPU,
        bool plannedDirect,
        bool directSourcesSkipped,
        RenderPolicyReason passReason)
    {
        if (packet.disposition == MeshPassDisposition::Skip)
        {
            return MapSourceReason(packet.reason);
        }
        if (packet.disposition == MeshPassDisposition::Direct)
        {
            return directSourcesSkipped
                       ? passReason
                       : (plannedDirect || plannedGPU
                              ? MapSourceReason(packet.reason)
                              : passReason);
        }
        if (plannedGPU)
        {
            // A Direct-only readiness failure may set the pass-level reason
            // while the GPU lane remains executable. Preserve the successful
            // GPU packet outcome instead of attributing the Direct failure to
            // every candidate in the pass.
            return directSourcesSkipped ? RenderPolicyReason::None
                                        : passReason;
        }
        if (plannedDirect)
        {
            return passReason == RenderPolicyReason::None ||
                           passReason == RenderPolicyReason::ForcedGPUDriven
                       ? RenderPolicyReason::PlannedFallback
                       : passReason;
        }
        return passReason;
    }

    bool AppendSourceOrderedLane(RenderFrameExecutionPlan& plan,
                                 RenderPassExecutionPlan& passPlan,
                                 const MeshPassPacketStream& stream,
                                 MeshPassDisposition disposition,
                                 bool includeCandidates,
                                 DrawPacketRange& outRange)
    {
        outRange.first = static_cast<uint32>(plan.packetReferences.size());
        for (uint32 sourceIndex = 0;
             sourceIndex < static_cast<uint32>(stream.packets.size());
             ++sourceIndex)
        {
            const MeshPassProcessorResult& packet = stream.packets[sourceIndex];
            if (packet.disposition != disposition &&
                !(includeCandidates &&
                  packet.disposition == MeshPassDisposition::GPUCandidate))
            {
                continue;
            }
            if (!AppendReference(plan, passPlan.pass, sourceIndex, stream))
            {
                return false;
            }
            ++outRange.count;
        }
        return true;
    }

    bool FinalizeIdentityAccounting(
        const RenderFrameExecutionPlan& plan,
        RenderPassExecutionPlan& passPlan)
    {
        RenderPacketIdentityAccounting accounting;
        accounting.expectedPacketCount = passPlan.partition.inputPacketCount;

        const std::array<DrawPacketRange, 3> ranges = {
            passPlan.gpuEligiblePackets,
            passPlan.directPackets,
            passPlan.skippedPackets,
        };
        std::vector<bool> sourceIndices(accounting.expectedPacketCount, false);
        std::set<RenderDrawPacketId> packetIds;
        uint64 terminalPacketCount = 0;
        for (const DrawPacketRange range : ranges)
        {
            const uint64 end = static_cast<uint64>(range.first) + range.count;
            if (end > plan.packetReferences.size())
            {
                return false;
            }
            terminalPacketCount += range.count;
            for (uint64 index = range.first; index < end; ++index)
            {
                const RenderDrawPacketReference& reference =
                    plan.packetReferences[static_cast<size_t>(index)];
                if (reference.sourcePacketIndex >= sourceIndices.size() ||
                    sourceIndices[reference.sourcePacketIndex])
                {
                    return false;
                }
                sourceIndices[reference.sourcePacketIndex] = true;
                if (!packetIds.insert(reference.packetId).second)
                {
                    ++accounting.duplicatePacketIdCount;
                }
            }
        }
        if (terminalPacketCount > std::numeric_limits<uint32>::max() ||
            packetIds.size() > std::numeric_limits<uint32>::max())
        {
            return false;
        }
        accounting.terminalPacketCount =
            static_cast<uint32>(terminalPacketCount);
        accounting.uniquePacketIdCount =
            static_cast<uint32>(packetIds.size());
        accounting.unaccountedPacketIdCount = static_cast<uint32>(
            std::count(sourceIndices.begin(), sourceIndices.end(), false));
        passPlan.identityAccounting = accounting;
        return accounting.IsExactlyOnce();
    }

    bool CompilePass(const RenderPassPolicyDecision& decision,
                     const MeshPassPacketStream& stream,
                     RenderFrameExecutionPlan& plan,
                     RenderPassExecutionPlan& outPass)
    {
        if (!ValidatePreparedStream(stream) ||
            !DecisionMatchesStream(decision, stream))
        {
            return false;
        }
        if ((decision.pass == RenderPassKind::Shadow ||
             decision.pass == RenderPassKind::Transparent) &&
            stream.stats.gpuCandidatePacketCount != 0)
        {
            return false;
        }

        outPass.pass = decision.pass;
        outPass.visibility = decision.visibility;
        outPass.preferredSubmission = decision.preferredSubmission;
        outPass.fallbackSubmission = decision.fallbackSubmission;
        outPass.reason = decision.reason;
        outPass.partition = decision.partition;

        const uint32 sourceCandidates = stream.stats.gpuCandidatePacketCount;

        const bool plannedGPU =
            outPass.partition.gpuDrivenPacketCount == sourceCandidates &&
            sourceCandidates != 0;
        const bool plannedAllRelevantDirect =
            outPass.partition.directPacketCount ==
                stream.stats.relevantPacketCount &&
            outPass.partition.gpuDrivenPacketCount == 0;
        const bool plannedAllSkip =
            outPass.partition.skippedPacketCount ==
                stream.stats.inputPacketCount &&
            outPass.partition.gpuDrivenPacketCount == 0 &&
            outPass.partition.directPacketCount == 0;
        const bool directSourcesSkipped =
            plannedGPU &&
            stream.stats.directPacketCount != 0 &&
            outPass.partition.directPacketCount == 0 &&
            outPass.partition.skippedPacketCount ==
                stream.stats.skippedPacketCount +
                    stream.stats.directPacketCount;

        if (plannedGPU)
        {
            outPass.gpuEligiblePackets.first =
                static_cast<uint32>(plan.packetReferences.size());
            for (uint32 sourceIndex :
                 stream.sortedGPUCandidatePacketIndices)
            {
                if (!AppendReference(plan, outPass.pass, sourceIndex, stream))
                {
                    return false;
                }
                ++outPass.gpuEligiblePackets.count;
            }
        }
        else if (!plannedAllRelevantDirect && !plannedAllSkip)
        {
            return false;
        }
        else
        {
            outPass.gpuEligiblePackets.first =
                static_cast<uint32>(plan.packetReferences.size());
        }

        if (plannedAllRelevantDirect)
        {
            if (!AppendSourceOrderedLane(plan,
                                         outPass,
                                         stream,
                                         MeshPassDisposition::Direct,
                                         true,
                                         outPass.directPackets))
            {
                return false;
            }
        }
        else
        {
            outPass.directPackets.first =
                static_cast<uint32>(plan.packetReferences.size());
            if (plannedGPU && decision.partition.directPacketCount != 0 &&
                !AppendSourceOrderedLane(plan,
                                         outPass,
                                         stream,
                                         MeshPassDisposition::Direct,
                                         false,
                                         outPass.directPackets))
            {
                return false;
            }
        }

        outPass.skippedPackets.first =
            static_cast<uint32>(plan.packetReferences.size());
        if (plannedAllSkip)
        {
            for (uint32 sourceIndex = 0;
                 sourceIndex < static_cast<uint32>(stream.packets.size());
                 ++sourceIndex)
            {
                if (!AppendReference(plan, outPass.pass, sourceIndex, stream))
                {
                    return false;
                }
                ++outPass.skippedPackets.count;
            }
        }
        else if (
            plannedGPU &&
            outPass.partition.directPacketCount == 0 &&
            outPass.partition.skippedPacketCount ==
                stream.stats.inputPacketCount -
                    stream.stats.gpuCandidatePacketCount)
        {
            for (uint32 sourceIndex = 0;
                 sourceIndex < static_cast<uint32>(stream.packets.size());
                 ++sourceIndex)
            {
                const MeshPassProcessorResult& packet =
                    stream.packets[sourceIndex];
                if (packet.disposition != MeshPassDisposition::Skip &&
                    packet.disposition != MeshPassDisposition::Direct)
                {
                    continue;
                }
                if (!AppendReference(plan, outPass.pass, sourceIndex, stream))
                {
                    return false;
                }
                ++outPass.skippedPackets.count;
            }
        }
        else if (!AppendSourceOrderedLane(plan,
                                          outPass,
                                          stream,
                                          MeshPassDisposition::Skip,
                                          false,
                                          outPass.skippedPackets))
        {
            return false;
        }

        for (const MeshPassProcessorResult& packet : stream.packets)
        {
            CountReason(
                outPass,
                GetPacketOutcomeReason(packet,
                                       plannedGPU,
                                       plannedAllRelevantDirect,
                                       directSourcesSkipped,
                                       outPass.reason));
        }
        if (outPass.gpuEligiblePackets.count !=
                outPass.partition.gpuDrivenPacketCount ||
            outPass.directPackets.count !=
                outPass.partition.directPacketCount ||
            outPass.skippedPackets.count !=
                outPass.partition.skippedPacketCount)
        {
            return false;
        }
        return FinalizeIdentityAccounting(plan, outPass);
    }
} // namespace

bool ValidateMeshPassPacketStream(const MeshPassPacketStream& stream)
{
    return ValidatePreparedStream(stream);
}

RenderFramePlanCompileResult CompileRenderFrameExecutionPlan(
    const RenderPolicyResolution& resolution,
    const SceneMeshPassPreparation& preparation)
{
    RenderFramePlanCompileResult result;
    if (!ValidateRenderPolicyResolution(resolution))
    {
        return result;
    }

    if (resolution.canonicalPassDecisions.size() !=
        kCanonicalPassOrder.size())
    {
        return result;
    }
    for (size_t index = 0; index < kCanonicalPassOrder.size(); ++index)
    {
        if (resolution.canonicalPassDecisions[index].pass !=
            kCanonicalPassOrder[index])
        {
            return result;
        }
    }

    result.plan.frameSequence = resolution.frameSequence;
    result.plan.viewOrdinal = resolution.viewOrdinal;
    result.plan.viewPolicy = resolution.viewPolicy;
    result.plan.capabilities = resolution.capabilities;
    result.plan.qualification = resolution.qualification;

    for (const RenderPassPolicyDecision& decision :
         resolution.canonicalPassDecisions)
    {
        const MeshPassPacketStream* stream = GetStream(preparation, decision.pass);
        if (!stream)
        {
            return result;
        }
        RenderPassExecutionPlan passPlan;
        if (!CompilePass(decision, *stream, result.plan, passPlan))
        {
            return result;
        }
        result.plan.passes.push_back(std::move(passPlan));
    }

    if (!ValidateRenderFrameExecutionPlan(result.plan))
    {
        result.plan = {};
        return result;
    }
    result.succeeded = true;
    result.reason = RenderPolicyReason::None;
    return result;
}
} // namespace RVX
