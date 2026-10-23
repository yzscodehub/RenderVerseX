/**
 * @file RenderDrawPlan.cpp
 * @brief Strategy-neutral per-view draw-plan construction and validation.
 */

#include "Render/Renderer/RenderDrawPlan.h"
#include "Render/Policy/RenderFramePlanCompiler.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;
    constexpr std::array<RenderPassKind, 4> CANONICAL_PASSES = {
        RenderPassKind::Depth,
        RenderPassKind::Opaque,
        RenderPassKind::Shadow,
        RenderPassKind::Transparent,
    };

    void HashByte(uint64& hash, uint8 value) noexcept
    {
        hash ^= value;
        hash *= FNV_PRIME;
    }

    template <typename TValue>
    void HashValue(uint64& hash, TValue value) noexcept
    {
        for (uint32 shift = 0; shift < sizeof(TValue) * 8U; shift += 8U)
        {
            HashByte(hash, static_cast<uint8>(value >> shift));
        }
    }

    void HashHandle(uint64& hash, RenderResourceHandle handle) noexcept
    {
        HashValue(hash, handle.slot);
        HashValue(hash, handle.generation);
    }

    void HashPacketIdentity(uint64& hash,
                            const RenderDrawPacket& packet) noexcept
    {
        HashValue(hash, static_cast<uint8>(packet.pass));
        HashValue(hash, packet.objectId);
        HashValue(hash, packet.primitiveData);
        HashValue(hash, packet.submeshIndex);
        HashValue(hash, GetStableHash(packet.pipelineKey));
        HashValue(hash, GetStableHash(packet.geometryKey));
        HashValue(hash, GetStableHash(packet.materialKey));
        HashValue(hash, packet.materialInstanceKey.textureBindingHash);
        HashValue(hash,
                  static_cast<uint8>(
                      packet.materialInstanceKey.parameterTableCompatible));
        HashValue(hash, packet.arguments.indexCount);
        HashValue(hash, packet.arguments.instanceCount);
        HashValue(hash, packet.arguments.firstIndex);
        HashValue(hash, static_cast<uint32>(packet.arguments.vertexOffset));
        HashValue(hash, packet.arguments.firstInstance);
        HashValue(hash, static_cast<uint32>(packet.flags));
    }

    [[nodiscard]] uint64 ComputePassIntegrityHash(
        const RenderPassDrawPlan& plan) noexcept
    {
        uint64 hash = FNV_OFFSET;
        HashValue(hash, static_cast<uint8>(plan.pass));
        for (const RenderPassDrawPlanEntry& entry : plan.entries)
        {
            HashValue(hash, entry.sourcePacketIndex);
            HashValue(hash, entry.sourceOrdinal);
            HashValue(hash, entry.classificationIndex);
            HashPacketIdentity(hash, entry.packet);
            HashValue(hash, GetStableHash(entry.semanticGroup));
            HashValue(hash, std::bit_cast<uint32>(entry.viewDepth));
            HashValue(hash, static_cast<uint8>(entry.passRelevant));
            HashValue(hash, static_cast<uint8>(entry.cpuVisible));
        }
        HashValue(hash, plan.stats.inputPacketCount);
        HashValue(hash, plan.stats.semanticPacketCount);
        HashValue(hash, plan.stats.viewCandidatePacketCount);
        HashValue(hash, plan.stats.visiblePacketCount);
        HashValue(hash, plan.stats.candidatePrimitiveCount);
        HashValue(hash, plan.stats.visiblePrimitiveCount);
        HashValue(hash, plan.stats.materialBindingCount);
        HashValue(hash, plan.stats.uniqueMaterialCount);
        HashValue(hash, plan.stats.semanticDrawGroupCount);
        HashValue(hash, plan.stats.candidateIdentityHash);
        HashValue(hash, plan.stats.visibleIdentityHash);
        HashValue(hash, plan.stats.materialIdentityHash);
        HashValue(hash, plan.stats.semanticGroupHash);
        return hash;
    }

    [[nodiscard]] uint64 ComputeViewPlanSemanticHash(
        const RenderViewDrawPlan& plan) noexcept
    {
        uint64 hash = FNV_OFFSET;
        HashValue(hash, plan.frameSequence);
        HashValue(hash, plan.viewOrdinal);
        HashValue(hash, plan.sceneClassificationHash);
        for (const RenderPassDrawPlan& pass : plan.passes)
        {
            HashValue(hash, static_cast<uint8>(pass.pass));
            HashValue(hash, pass.stats.integrityHash);
        }
        return hash;
    }

    [[nodiscard]] const MeshPassPacketStream* GetStream(
        const SceneMeshPassPreparation& preparation,
        RenderPassKind pass) noexcept
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

    [[nodiscard]] bool ClassificationMatches(
        const RenderSceneDrawClassification& classification,
        const MeshPassProcessorResult& source,
        RenderPassKind pass) noexcept
    {
        if (source.classificationIndex == RVX_INVALID_INDEX ||
            source.classificationIndex >= classification.entries.size())
        {
            return false;
        }
        const RenderSceneDrawClassificationEntry& classificationEntry =
            classification.entries[source.classificationIndex];
        RenderDrawPacket expected = classificationEntry.draw.packet;
        expected.pass = pass;
        if (pass == RenderPassKind::Shadow)
        {
            const auto setFlag = [](RenderDrawFlags flags,
                                    RenderDrawFlags flag,
                                    bool enabled) noexcept
            {
                const uint32 bits = static_cast<uint32>(flags);
                const uint32 flagBits = static_cast<uint32>(flag);
                return static_cast<RenderDrawFlags>(
                    enabled ? bits | flagBits : bits & ~flagBits);
            };
            expected.flags = setFlag(expected.flags,
                                     RenderDrawFlags::CastsShadow,
                                     classificationEntry.castsShadow);
            expected.flags = setFlag(expected.flags,
                                     RenderDrawFlags::Skinned,
                                     classificationEntry.skinned);
            expected.pipelineKey.skinned = classificationEntry.skinned;
        }
        return expected == source.packet;
    }

    [[nodiscard]] bool IsVisibleSource(
        const RenderVisibilityResult& visibility,
        RenderPassKind pass,
        uint32 sourcePacketIndex,
        bool& outVisible) noexcept
    {
        if (pass == RenderPassKind::Shadow)
        {
            outVisible = true;
            return true;
        }
        if (!visibility.HasDirectPacketSource(pass, sourcePacketIndex))
        {
            return false;
        }
        outVisible = visibility.IsDirectPacketVisible(pass, sourcePacketIndex);
        return true;
    }

    [[nodiscard]] bool IsClassificationExpectedInPass(
        const RenderSceneDrawClassificationEntry& entry,
        RenderPassKind pass,
        const RenderVisibilityResult& visibility) noexcept
    {
        if (pass == RenderPassKind::Shadow)
        {
            // Preserve one explicit outcome per classified surface. A surface
            // that does not cast shadows remains a skipped shadow input.
            return true;
        }

        if (!entry.sceneVisible || !entry.drawable ||
            !IsRenderLayerVisible(entry.layerMask,
                                  visibility.evaluatedCullingMask))
        {
            return false;
        }

        const bool transparent =
            entry.draw.renderMode == MaterialRenderMode::Transparent;
        const std::vector<uint8>& objectMask = transparent
            ? visibility.cpuVisibleObjectMask
            : visibility.coarseVisibleObjectMask;
        const uint32 objectIndex = entry.draw.objectIndex;
        if (objectIndex >= objectMask.size() || objectMask[objectIndex] == 0)
        {
            return false;
        }

        if (pass == RenderPassKind::Transparent)
        {
            return transparent;
        }
        return (pass == RenderPassKind::Depth ||
                pass == RenderPassKind::Opaque) && !transparent;
    }

    [[nodiscard]] bool BuildPassPlan(
        RenderPassKind pass,
        const MeshPassPacketStream& stream,
        const RenderSceneDrawClassification& classification,
        const RenderVisibilityResult& visibility,
        RenderPassDrawPlan& outPlan,
        RenderDrawPlanBuildCode& outFailure)
    {
        outPlan = {};
        outPlan.pass = pass;
        if (!ValidateMeshPassPacketStream(stream) ||
            stream.packets.size() > std::numeric_limits<uint32>::max())
        {
            outFailure = RenderDrawPlanBuildCode::InvalidPreparation;
            return false;
        }

        outPlan.entries.reserve(stream.packets.size());
        std::vector<uint32> candidatePrimitives;
        std::vector<uint32> visiblePrimitives;
        std::vector<RenderResourceHandle> materials;
        std::vector<RenderDrawGroupKey> semanticGroups;
        std::vector<uint8> classificationCoverage(
            classification.entries.size(), 0);
        uint64 candidateHash = FNV_OFFSET;
        uint64 visibleHash = FNV_OFFSET;
        uint64 materialHash = FNV_OFFSET;
        uint64 groupHash = FNV_OFFSET;

        for (uint32 sourcePacketIndex = 0;
             sourcePacketIndex < static_cast<uint32>(stream.packets.size());
             ++sourcePacketIndex)
        {
            const MeshPassProcessorResult& source =
                stream.packets[sourcePacketIndex];
            if (!ClassificationMatches(classification, source, pass))
            {
                outFailure = RenderDrawPlanBuildCode::ClassificationMismatch;
                return false;
            }
            if (classificationCoverage[source.classificationIndex] != 0)
            {
                outFailure =
                    RenderDrawPlanBuildCode::ClassificationCoverageMismatch;
                return false;
            }
            classificationCoverage[source.classificationIndex] = 1;

            RenderPassDrawPlanEntry entry;
            entry.sourcePacketIndex = sourcePacketIndex;
            entry.sourceOrdinal = source.sourceOrdinal;
            entry.classificationIndex = source.classificationIndex;
            entry.packet = source.packet;
            // Normalize to one lane-independent grouping contract. The source
            // key contains Direct/GPU submission layout choices and therefore
            // cannot participate in semantic DrawPlan identity.
            entry.semanticGroup = MakeRenderInstanceBatchKey(
                entry.packet, RenderSubmissionLayout{});
            entry.viewDepth = source.viewDepth;
            entry.passRelevant = source.IsRelevant();
            if (!IsVisibleSource(
                    visibility, pass, sourcePacketIndex, entry.cpuVisible))
            {
                outFailure = RenderDrawPlanBuildCode::MissingVisibility;
                return false;
            }
            if (!entry.passRelevant)
            {
                entry.cpuVisible = false;
            }
            outPlan.entries.push_back(entry);

            if (!entry.passRelevant)
            {
                continue;
            }
            ++outPlan.stats.semanticPacketCount;
            ++outPlan.stats.viewCandidatePacketCount;
            candidatePrimitives.push_back(entry.packet.primitiveData);
            HashValue(candidateHash, entry.sourcePacketIndex);
            HashValue(candidateHash, entry.sourceOrdinal);
            HashPacketIdentity(candidateHash, entry.packet);
            if (!entry.cpuVisible)
            {
                continue;
            }
            ++outPlan.stats.visiblePacketCount;
            ++outPlan.stats.materialBindingCount;
            visiblePrimitives.push_back(entry.packet.primitiveData);
            materials.push_back(entry.packet.materialKey.material);
            semanticGroups.push_back(entry.semanticGroup);
            HashValue(visibleHash, entry.sourcePacketIndex);
            HashPacketIdentity(visibleHash, entry.packet);
        }

        for (uint32 classificationIndex = 0;
             classificationIndex <
                 static_cast<uint32>(classification.entries.size());
             ++classificationIndex)
        {
            const bool expected = IsClassificationExpectedInPass(
                classification.entries[classificationIndex],
                pass,
                visibility);
            if ((classificationCoverage[classificationIndex] != 0) != expected)
            {
                outFailure =
                    RenderDrawPlanBuildCode::ClassificationCoverageMismatch;
                return false;
            }
        }

        const auto sortUnique = [](auto& values)
        {
            std::sort(values.begin(), values.end());
            values.erase(std::unique(values.begin(), values.end()), values.end());
        };
        sortUnique(candidatePrimitives);
        sortUnique(visiblePrimitives);
        sortUnique(materials);
        const RenderDrawGroupKeyLess groupLess;
        std::sort(semanticGroups.begin(), semanticGroups.end(), groupLess);
        semanticGroups.erase(
            std::unique(semanticGroups.begin(), semanticGroups.end()),
            semanticGroups.end());

        outPlan.stats.inputPacketCount =
            static_cast<uint32>(outPlan.entries.size());
        outPlan.stats.candidatePrimitiveCount =
            static_cast<uint32>(candidatePrimitives.size());
        outPlan.stats.visiblePrimitiveCount =
            static_cast<uint32>(visiblePrimitives.size());
        outPlan.stats.uniqueMaterialCount =
            static_cast<uint32>(materials.size());
        outPlan.stats.semanticDrawGroupCount =
            static_cast<uint32>(semanticGroups.size());
        for (RenderResourceHandle material : materials)
        {
            HashHandle(materialHash, material);
        }
        for (const RenderDrawGroupKey& group : semanticGroups)
        {
            HashValue(groupHash, GetStableHash(group));
        }
        outPlan.stats.candidateIdentityHash = candidateHash;
        outPlan.stats.visibleIdentityHash = visibleHash;
        outPlan.stats.materialIdentityHash = materialHash;
        outPlan.stats.semanticGroupHash = groupHash;
        outPlan.stats.integrityHash = ComputePassIntegrityHash(outPlan);
        outPlan.structurallyValid = true;
        return outPlan.IsValid();
    }
} // namespace

bool RenderPassDrawPlan::IsValid() const noexcept
{
    if (!structurallyValid || pass == RenderPassKind::None ||
        entries.size() != stats.inputPacketCount ||
        stats.visiblePacketCount > stats.viewCandidatePacketCount ||
        stats.viewCandidatePacketCount != stats.semanticPacketCount ||
        stats.candidatePrimitiveCount > stats.semanticPacketCount ||
        stats.visiblePrimitiveCount > stats.visiblePacketCount ||
        stats.materialBindingCount != stats.visiblePacketCount ||
        stats.uniqueMaterialCount > stats.materialBindingCount ||
        stats.semanticDrawGroupCount > stats.visiblePacketCount ||
        stats.integrityHash == 0 ||
        stats.integrityHash != ComputePassIntegrityHash(*this))
    {
        return false;
    }
    for (uint32 index = 0;
         index < static_cast<uint32>(entries.size());
         ++index)
    {
        if (entries[index].sourcePacketIndex != index ||
            entries[index].packet.pass != pass ||
            (entries[index].cpuVisible && !entries[index].passRelevant))
        {
            return false;
        }
    }
    return true;
}

const RenderPassDrawPlan* RenderViewDrawPlan::FindPass(
    RenderPassKind pass) const noexcept
{
    const auto found = std::find_if(
        passes.begin(), passes.end(),
        [pass](const RenderPassDrawPlan& candidate)
        {
            return candidate.pass == pass;
        });
    return found == passes.end() ? nullptr : &*found;
}

bool RenderViewDrawPlan::IsValid() const noexcept
{
    if (!structurallyValid || frameSequence == 0 ||
        sceneClassificationHash == 0 || semanticHash == 0)
    {
        return false;
    }
    for (size_t index = 0; index < passes.size(); ++index)
    {
        if (passes[index].pass != CANONICAL_PASSES[index] ||
            !passes[index].IsValid())
        {
            return false;
        }
    }
    return semanticHash == ComputeViewPlanSemanticHash(*this);
}

RenderViewDrawPlanBuildResult BuildRenderViewDrawPlan(
    uint64 frameSequence,
    uint32 viewOrdinal,
    const RenderSceneDrawClassification& classification,
    const SceneMeshPassPreparation& preparation,
    const RenderVisibilityResult& visibility)
{
    RenderViewDrawPlanBuildResult result;
    if (frameSequence == 0)
    {
        result.code = RenderDrawPlanBuildCode::InvalidIdentity;
        return result;
    }
    if (!classification.IsValid())
    {
        result.code = RenderDrawPlanBuildCode::InvalidClassification;
        return result;
    }
    if (!visibility.structurallyValid)
    {
        result.code = RenderDrawPlanBuildCode::MissingVisibility;
        return result;
    }

    result.plan.frameSequence = frameSequence;
    result.plan.viewOrdinal = viewOrdinal;
    result.plan.sceneClassificationHash = classification.stats.semanticHash;
    for (size_t index = 0; index < CANONICAL_PASSES.size(); ++index)
    {
        const RenderPassKind pass = CANONICAL_PASSES[index];
        const MeshPassPacketStream* stream = GetStream(preparation, pass);
        if (stream == nullptr ||
            !BuildPassPlan(pass,
                           *stream,
                           classification,
                           visibility,
                           result.plan.passes[index],
                           result.code))
        {
            result.plan = {};
            return result;
        }
    }
    result.plan.semanticHash = ComputeViewPlanSemanticHash(result.plan);
    result.plan.structurallyValid = true;
    if (!result.plan.IsValid())
    {
        result.plan = {};
        result.code = RenderDrawPlanBuildCode::InvalidPreparation;
        return result;
    }
    result.code = RenderDrawPlanBuildCode::Built;
    return result;
}

bool ValidateRenderFrameExecutionPlanDrawParity(
    const RenderFrameExecutionPlan& executionPlan,
    const RenderViewDrawPlan& drawPlan) noexcept
{
    if (!drawPlan.IsValid() ||
        !ValidateRenderFrameExecutionPlan(executionPlan) ||
        executionPlan.frameSequence != drawPlan.frameSequence ||
        executionPlan.viewOrdinal != drawPlan.viewOrdinal ||
        executionPlan.passes.size() != drawPlan.passes.size())
    {
        return false;
    }
    for (const RenderPassExecutionPlan& executionPass : executionPlan.passes)
    {
        const RenderPassDrawPlan* drawPass = drawPlan.FindPass(executionPass.pass);
        if (drawPass == nullptr ||
            executionPass.partition.inputPacketCount !=
                drawPass->stats.inputPacketCount ||
            executionPass.partition.relevantPacketCount !=
                drawPass->stats.semanticPacketCount ||
            !executionPass.identityAccounting.IsExactlyOnce())
        {
            return false;
        }
        const std::array<DrawPacketRange, 3> ranges = {
            executionPass.gpuEligiblePackets,
            executionPass.directPackets,
            executionPass.skippedPackets,
        };
        std::vector<uint8> seen(drawPass->entries.size(), 0);
        for (DrawPacketRange range : ranges)
        {
            const uint64 end = static_cast<uint64>(range.first) + range.count;
            if (end > executionPlan.packetReferences.size())
            {
                return false;
            }
            for (uint64 index = range.first; index < end; ++index)
            {
                const RenderDrawPacketReference& reference =
                    executionPlan.packetReferences[static_cast<size_t>(index)];
                if (reference.pass != executionPass.pass ||
                    reference.sourcePacketIndex >= drawPass->entries.size() ||
                    seen[reference.sourcePacketIndex] != 0)
                {
                    return false;
                }
                const RenderPassDrawPlanEntry& drawEntry =
                    drawPass->entries[reference.sourcePacketIndex];
                if (reference.sourceOrdinal != drawEntry.sourceOrdinal ||
                    reference.sourceSignature.packet != drawEntry.packet ||
                    reference.sourceSignature.viewDepth != drawEntry.viewDepth ||
                    reference.sourceSignature.classificationIndex !=
                        drawEntry.classificationIndex)
                {
                    return false;
                }
                seen[reference.sourcePacketIndex] = 1;
            }
        }
        if (std::find(seen.begin(), seen.end(), 0) != seen.end())
        {
            return false;
        }
    }
    return true;
}
} // namespace RVX
