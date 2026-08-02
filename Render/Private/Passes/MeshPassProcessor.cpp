/**
 * @file MeshPassProcessor.cpp
 * @brief Shared value-only mesh-pass processing and grouping helpers.
 */

#include "Render/Passes/MeshPassProcessor.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 FNV_OFFSET = 14695981039346656037ULL;
    constexpr uint64 FNV_PRIME = 1099511628211ULL;

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

    bool HasDrawFlag(RenderDrawFlags flags, RenderDrawFlags flag) noexcept
    {
        return (static_cast<uint32>(flags) & static_cast<uint32>(flag)) != 0;
    }

    bool IsMissingMaterial(const RenderDrawPacket& packet) noexcept
    {
        return HasDrawFlag(packet.flags, RenderDrawFlags::MissingMaterial);
    }

    bool IsPending(MeshPassResourceAvailability availability) noexcept
    {
        return availability == MeshPassResourceAvailability::Pending;
    }

    bool IsUnavailable(MeshPassResourceAvailability availability) noexcept
    {
        return availability == MeshPassResourceAvailability::Unavailable;
    }

    MeshPassEligibilityReason ResolveReason(
        const MeshPassProcessorInput& input,
        MeshPassEligibilityReason intrinsicDirectReason) noexcept
    {
        const RenderDrawPacket& packet = input.packet;
        if (packet.pipelineKey.materialVariant ==
            MaterialPipelineVariant::Transparent)
        {
            return MeshPassEligibilityReason::Transparent;
        }
        if (HasDrawFlag(packet.flags, RenderDrawFlags::Skinned))
        {
            return MeshPassEligibilityReason::Skinned;
        }
        if (input.availability.specialMaterial)
        {
            return MeshPassEligibilityReason::SpecialMaterial;
        }
        if (packet.pipelineKey.topology !=
            MeshUploadPrimitiveTopology::Triangles)
        {
            return MeshPassEligibilityReason::UnsupportedTopology;
        }
        if (packet.geometryKey.indexType != MeshUploadIndexType::UInt32)
        {
            return MeshPassEligibilityReason::UnsupportedIndexType;
        }
        if (IsUnavailable(input.availability.pipeline))
        {
            return MeshPassEligibilityReason::PipelineUnavailable;
        }
        if (!packet.geometryKey.mesh.IsValid() ||
            packet.arguments.indexCount == 0 ||
            IsUnavailable(input.availability.geometry))
        {
            return MeshPassEligibilityReason::GeometryUnavailable;
        }

        const bool missingMaterial = IsMissingMaterial(packet);
        if (IsPending(input.availability.pipeline) ||
            IsPending(input.availability.geometry) ||
            (!missingMaterial && IsPending(input.availability.material)))
        {
            return MeshPassEligibilityReason::ResourcePending;
        }
        if (!missingMaterial && IsUnavailable(input.availability.material))
        {
            return MeshPassEligibilityReason::ResourceUnavailable;
        }
        return intrinsicDirectReason;
    }

    auto KeyTuple(const RenderDrawGroupKey& key) noexcept
    {
        return std::tuple{
            static_cast<uint8>(key.pass),
            static_cast<uint8>(key.pipeline.materialVariant),
            static_cast<uint8>(key.pipeline.topology),
            key.pipeline.skinned,
            key.geometry.mesh.slot,
            key.geometry.mesh.generation,
            key.geometry.submeshIndex,
            static_cast<uint8>(key.geometry.indexType),
            key.material.material.slot,
            key.material.material.generation,
            static_cast<uint8>(key.material.materialMode),
            static_cast<uint32>(key.layout.vertexStreams),
            static_cast<uint32>(key.layout.bindings),
            static_cast<uint8>(key.layout.primitiveDataBinding)};
    }
} // namespace

uint64 GetStableHash(const RenderSubmissionLayout& layout) noexcept
{
    uint64 hash = FNV_OFFSET;
    HashValue(hash, static_cast<uint32>(layout.vertexStreams));
    HashValue(hash, static_cast<uint32>(layout.bindings));
    HashValue(hash, static_cast<uint8>(layout.primitiveDataBinding));
    return hash;
}

uint64 GetStableHash(const RenderDrawGroupKey& key) noexcept
{
    uint64 hash = FNV_OFFSET;
    HashValue(hash, static_cast<uint8>(key.pass));
    HashValue(hash, GetStableHash(key.pipeline));
    HashValue(hash, GetStableHash(key.geometry));
    HashValue(hash, GetStableHash(key.material));
    HashValue(hash, GetStableHash(key.layout));
    return hash;
}

size_t RenderDrawGroupKeyHasher::operator()(
    const RenderDrawGroupKey& key) const noexcept
{
    return static_cast<size_t>(GetStableHash(key));
}

bool RenderDrawGroupKeyLess::operator()(const RenderDrawGroupKey& lhs,
                                        const RenderDrawGroupKey& rhs) const noexcept
{
    return KeyTuple(lhs) < KeyTuple(rhs);
}

void MeshPassProcessorStats::Record(
    const MeshPassProcessorResult& result) noexcept
{
    ++inputPacketCount;
    const size_t reasonIndex = static_cast<size_t>(result.reason);
    if (reasonIndex < reasonCounts.size())
    {
        ++reasonCounts[reasonIndex];
    }

    switch (result.disposition)
    {
        case MeshPassDisposition::GPUCandidate:
            ++relevantPacketCount;
            ++gpuCandidatePacketCount;
            break;
        case MeshPassDisposition::Direct:
            ++relevantPacketCount;
            ++directPacketCount;
            break;
        case MeshPassDisposition::Skip:
            ++skippedPacketCount;
            break;
        default:
            break;
    }
}

uint32 MeshPassProcessorStats::GetReasonCount(
    MeshPassEligibilityReason reason) const noexcept
{
    const size_t index = static_cast<size_t>(reason);
    return index < reasonCounts.size() ? reasonCounts[index] : 0;
}

bool MeshPassProcessorStats::HasCompleteRelevantOutcome() const noexcept
{
    return relevantPacketCount ==
           gpuCandidatePacketCount + directPacketCount;
}

bool MeshPassProcessorStats::HasExactlyOneReasonPerRejectedPacket() const noexcept
{
    uint32 reasonTotal = 0;
    for (size_t index = 1; index < reasonCounts.size(); ++index)
    {
        if (index == static_cast<size_t>(
                         MeshPassEligibilityReason::PassIrrelevant))
        {
            continue;
        }
        reasonTotal += reasonCounts[index];
    }
    return reasonTotal == directPacketCount;
}

void MeshPassPacketStream::Clear()
{
    packets.clear();
    sortedGPUCandidates.clear();
    groups.clear();
    stats = {};
}

void MeshPassPacketStream::Record(MeshPassProcessorResult result)
{
    stats.Record(result);
    packets.push_back(std::move(result));
}

void MeshPassPacketStream::FinalizeGroups()
{
    BuildDeterministicRenderDrawGroups(
        packets, sortedGPUCandidates, groups);
}

void SceneMeshPassPreparation::Clear()
{
    depth.Clear();
    opaque.Clear();
    transparent.Clear();
    shadow.Clear();
}

void BuildDeterministicRenderDrawGroups(
    std::span<const MeshPassProcessorResult> results,
    std::vector<MeshPassProcessorResult>& outSortedCandidates,
    std::vector<RenderDrawGroupRange>& outGroups)
{
    outSortedCandidates.clear();
    outGroups.clear();
    outSortedCandidates.reserve(results.size());
    for (const MeshPassProcessorResult& result : results)
    {
        if (result.IsGPUCandidate())
        {
            outSortedCandidates.push_back(result);
        }
    }

    const RenderDrawGroupKeyLess keyLess;
    std::sort(outSortedCandidates.begin(), outSortedCandidates.end(),
              [&keyLess](const MeshPassProcessorResult& lhs,
                         const MeshPassProcessorResult& rhs)
              {
                  if (keyLess(lhs.groupKey, rhs.groupKey))
                  {
                      return true;
                  }
                  if (keyLess(rhs.groupKey, lhs.groupKey))
                  {
                      return false;
                  }
                  return std::tuple{lhs.sourceOrdinal,
                                    lhs.packet.objectId,
                                    lhs.packet.primitiveData,
                                    lhs.packet.submeshIndex} <
                         std::tuple{rhs.sourceOrdinal,
                                    rhs.packet.objectId,
                                    rhs.packet.primitiveData,
                                    rhs.packet.submeshIndex};
              });

    for (uint32 index = 0;
         index < static_cast<uint32>(outSortedCandidates.size());
         ++index)
    {
        const MeshPassProcessorResult& result = outSortedCandidates[index];
        if (!outGroups.empty() && outGroups.back().key == result.groupKey)
        {
            ++outGroups.back().count;
            continue;
        }
        outGroups.push_back(RenderDrawGroupRange{result.groupKey, index, 1});
    }
}

MeshPassProcessorResult MeshPassProcessor::MakeSkipped(
    const MeshPassProcessorInput& input) noexcept
{
    MeshPassProcessorResult result;
    result.packet = input.packet;
    result.sourceOrdinal = input.sourceOrdinal;
    result.viewDepth = input.viewDepth;
    return result;
}

MeshPassProcessorResult MeshPassProcessor::MakeRelevant(
    const MeshPassProcessorInput& input,
    RenderPassKind pass,
    MeshPassBindingRequirements bindings,
    MeshPassVertexStreams vertexStreams,
    MeshPassEligibilityReason intrinsicDirectReason) noexcept
{
    MeshPassProcessorResult result;
    result.packet = input.packet;
    result.packet.pass = pass;
    result.sourceOrdinal = input.sourceOrdinal;
    result.viewDepth = input.viewDepth;

    const bool missingMaterial = IsMissingMaterial(result.packet);
    if (HasDrawFlag(result.packet.flags, RenderDrawFlags::Skinned))
    {
        bindings |= MeshPassBindingRequirements::Skinning;
        vertexStreams |= MeshPassVertexStreams::BoneIndices;
        vertexStreams |= MeshPassVertexStreams::BoneWeights;
    }
    if (HasMeshPassBindingRequirement(
            bindings, MeshPassBindingRequirements::Material) &&
        missingMaterial)
    {
        bindings = static_cast<MeshPassBindingRequirements>(
            static_cast<uint32>(bindings) &
            ~static_cast<uint32>(MeshPassBindingRequirements::Material));
        bindings |= MeshPassBindingRequirements::DefaultMaterial;
    }

    result.reason = ResolveReason(input, intrinsicDirectReason);
    result.disposition = result.reason == MeshPassEligibilityReason::None
                             ? MeshPassDisposition::GPUCandidate
                             : MeshPassDisposition::Direct;

    if (result.disposition == MeshPassDisposition::GPUCandidate)
    {
        result.packet.pipelineKey.skinned = false;
        vertexStreams |= MeshPassVertexStreams::InstanceIndex;
    }

    result.groupKey.pass = result.packet.pass;
    result.groupKey.pipeline = result.packet.pipelineKey;
    result.groupKey.geometry = result.packet.geometryKey;
    result.groupKey.material = result.packet.materialKey;
    result.groupKey.layout.vertexStreams = vertexStreams;
    result.groupKey.layout.bindings = bindings;
    result.groupKey.layout.primitiveDataBinding =
        result.disposition == MeshPassDisposition::GPUCandidate
            ? PrimitiveDataBinding::InstanceBuffer
            : PrimitiveDataBinding::PerDrawConstants;
    return result;
}
} // namespace RVX
