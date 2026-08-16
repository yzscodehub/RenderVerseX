#include "Render/Submission/RasterInstanceStream.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Renderer/RenderScene.h"
#include "Resources/RenderResourceRegistry.h"
#include "Render/Submission/RenderInstanceBatchPlan.h"
#include "Render/Visibility/RenderVisibility.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 RASTER_TRANSCRIPT_FNV_OFFSET = 0xCBF29CE484222325ull;
    constexpr uint64 RASTER_TRANSCRIPT_FNV_PRIME = 0x100000001B3ull;

    void HashRasterTranscriptBytes(uint64& hash,
                                   const void* bytes,
                                   size_t byteCount) noexcept
    {
        const auto* const values = static_cast<const uint8*>(bytes);
        for (size_t byteIndex = 0; byteIndex < byteCount; ++byteIndex)
        {
            hash ^= values[byteIndex];
            hash *= RASTER_TRANSCRIPT_FNV_PRIME;
        }
    }

    template <typename TValue>
    void HashRasterTranscriptValue(uint64& hash, const TValue& value) noexcept
    {
        static_assert(std::is_trivially_copyable_v<TValue>);
        HashRasterTranscriptBytes(hash, &value, sizeof(TValue));
    }

    [[nodiscard]] uint64 MixRasterTranscriptHash(uint64 hash,
                                                  uint64 value) noexcept
    {
        HashRasterTranscriptValue(hash, value);
        return hash;
    }

    [[nodiscard]] uint64 MakeRasterTranscriptMultisetContribution(
        uint64 value,
        uint64 domain) noexcept
    {
        uint64 hash = RASTER_TRANSCRIPT_FNV_OFFSET;
        HashRasterTranscriptValue(hash, domain);
        HashRasterTranscriptValue(hash, value);
        return hash;
    }

    void SaturatingAdd(uint64& total, uint64 value, bool& saturated) noexcept
    {
        const uint64 maximum = std::numeric_limits<uint64>::max();
        if (saturated || value > maximum - total)
        {
            total = maximum;
            saturated = true;
            return;
        }
        total += value;
    }

    [[nodiscard]] bool IsValidRasterInstanceStreamKey(
        const RasterInstanceStreamKey& key) noexcept
    {
        return key.objectId != 0;
    }

    [[nodiscard]] bool RasterInstanceStreamKeyLess(
        const RasterInstanceStreamKey& lhs,
        const RasterInstanceStreamKey& rhs) noexcept
    {
        return lhs.objectId < rhs.objectId ||
               (lhs.objectId == rhs.objectId &&
                lhs.submeshIndex < rhs.submeshIndex);
    }

    struct RasterInstanceStreamKeyHash
    {
        [[nodiscard]] size_t operator()(
            const RasterInstanceStreamKey& key) const noexcept
        {
            const size_t objectHash = std::hash<uint64>{}(key.objectId);
            const size_t submeshHash = std::hash<uint32>{}(key.submeshIndex);
            return objectHash ^
                (submeshHash + static_cast<size_t>(0x9e3779b9U) +
                 (objectHash << 6U) + (objectHash >> 2U));
        }
    };

    [[nodiscard]] bool IsSameInstanceData(const GPUInstanceData& lhs,
                                           const GPUInstanceData& rhs) noexcept
    {
        static_assert(std::is_trivially_copyable_v<GPUInstanceData>);
        return std::memcmp(&lhs, &rhs, sizeof(GPUInstanceData)) == 0;
    }

    struct RasterUploadRange
    {
        uint32 first = 0;
        uint32 count = 0;
    };

    [[nodiscard]] std::vector<RasterUploadRange> MakeContiguousRanges(
        std::vector<uint32> rows)
    {
        std::vector<RasterUploadRange> ranges;
        if (rows.empty())
        {
            return ranges;
        }

        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        ranges.reserve(rows.size());
        uint32 first = rows.front();
        uint32 previous = first;
        for (size_t index = 1; index < rows.size(); ++index)
        {
            const uint32 row = rows[index];
            if (row == previous + 1U)
            {
                previous = row;
                continue;
            }
            ranges.push_back({first, previous - first + 1U});
            first = row;
            previous = row;
        }
        ranges.push_back({first, previous - first + 1U});
        return ranges;
    }

    [[nodiscard]] bool GrowRasterInstanceSegmentCapacity(
        uint32 currentCapacity,
        uint32 requiredCapacity,
        uint32& outCapacity) noexcept
    {
        if (requiredCapacity == 0)
        {
            return false;
        }
        uint32 capacity = std::max(currentCapacity, 1U);
        while (capacity < requiredCapacity)
        {
            if (capacity > std::numeric_limits<uint32>::max() - capacity)
            {
                capacity = requiredCapacity;
                break;
            }
            capacity *= 2U;
        }
        outCapacity = capacity;
        return true;
    }

    [[nodiscard]] uint32 GetHostWriteSynchronizationScopeIndex(
        RHIHostWriteSynchronization synchronization) noexcept
    {
        const uint32 scope = static_cast<uint32>(synchronization);
        return scope < RVX_RENDER_HOST_WRITE_SYNCHRONIZATION_SCOPE_COUNT
            ? scope
            : static_cast<uint32>(
                  RenderHostWriteSynchronizationScope::Unavailable);
    }

    void RecordMappedUploadRange(RenderUploadWorkDiagnostics& diagnostics,
                                 uint64 copiedBytes) noexcept
    {
        ++diagnostics.mappedRangeCount;
        diagnostics.cpuCopiedPayloadBytes += copiedBytes;
    }

    void RecordCommittedUploadRange(
        RenderUploadWorkDiagnostics& diagnostics,
        const RHIHostWriteReceipt& receipt,
        uint64 copiedBytes) noexcept
    {
        if (!receipt.IsPublished())
        {
            return;
        }

        const bool hadCommittedRanges = diagnostics.committedRangeCount != 0;
        ++diagnostics.committedRangeCount;
        diagnostics.committedPayloadBytes += copiedBytes;
        const uint32 scope = GetHostWriteSynchronizationScopeIndex(
            receipt.synchronization);
        ++diagnostics.hostVisibilitySynchronizationScopeRangeCounts[scope];
        const bool coherent = receipt.synchronization ==
            RHIHostWriteSynchronization::CoherentNoExplicitSync;
        const bool quantitativeEvidence = coherent || receipt.HasSynchronizedRange();
        const uint64 synchronizedBytes = receipt.HasSynchronizedRange()
            ? receipt.synchronizedSize
            : (coherent ? copiedBytes : 0);
        diagnostics.hostVisibilitySynchronizationScopeBytes[scope] +=
            synchronizedBytes;
        if (!quantitativeEvidence || scope == static_cast<uint32>(
                RenderHostWriteSynchronizationScope::Unavailable))
        {
            diagnostics.hostVisibilitySynchronizedBytes =
                DiagnosticValue<uint64>::Unavailable(
                    "A committed mapped range did not report quantitative host-visibility evidence");
            return;
        }
        if (hadCommittedRanges &&
            !diagnostics.hostVisibilitySynchronizedBytes.IsAvailable())
        {
            // Receipt aggregation is monotonic: later quantitative evidence
            // cannot repair an earlier committed range whose visibility was
            // not quantitatively evidenced.
            return;
        }
        if (diagnostics.hostVisibilitySynchronizationScopeRangeCounts[
                static_cast<uint32>(RenderHostWriteSynchronizationScope::Unavailable)] != 0)
        {
            return;
        }
        const uint64 prior = diagnostics.hostVisibilitySynchronizedBytes
            .GetValue()
            .value_or(0);
        diagnostics.hostVisibilitySynchronizedBytes =
            DiagnosticValue<uint64>::Available(prior + synchronizedBytes);
    }

    [[nodiscard]] bool CreatePersistentRasterInstanceBuffers(
        IRHIDevice& device,
        uint32 instanceCapacity,
        uint32 indexCapacity,
        const char* debugName,
        RHIBufferRef& outInstances,
        RHIBufferRef& outIndices)
    {
        if (instanceCapacity == 0 || indexCapacity == 0)
        {
            return false;
        }

        RHIBufferDesc instanceDesc;
        instanceDesc.size = static_cast<uint64>(instanceCapacity) *
            sizeof(GPUInstanceData);
        instanceDesc.usage = RHIBufferUsage::Structured |
            RHIBufferUsage::ShaderResource;
        instanceDesc.memoryType = RHIMemoryType::Upload;
        instanceDesc.stride = sizeof(GPUInstanceData);
        instanceDesc.debugName = debugName ? debugName : "RasterInstanceStream";
        outInstances = device.CreateBuffer(instanceDesc);
        if (!outInstances)
        {
            return false;
        }

        RHIBufferDesc indexDesc;
        indexDesc.size = static_cast<uint64>(indexCapacity) * sizeof(uint32);
        indexDesc.usage = RHIBufferUsage::Vertex;
        indexDesc.memoryType = RHIMemoryType::Upload;
        indexDesc.stride = sizeof(uint32);
        indexDesc.debugName = "RasterInstanceStreamIndices";
        outIndices = device.CreateBuffer(indexDesc);
        return outIndices != nullptr;
    }

    void PublishActiveRasterInstanceStream(
        RasterInstanceStream& stream,
        const std::array<RasterInstanceStreamFrameSlot,
                         RVX_MAX_FRAME_COUNT>& frameSlots,
        uint32 frameSlot,
        uint64 instanceUploadBytes,
        uint64 indexUploadBytes,
        uint32 instancePatchedRowCount,
        uint32 indexPatchedRowCount,
        bool instanceFullMaterialization,
        bool indexFullMaterialization,
        RenderUploadWorkDiagnostics instanceUploadWork,
        RenderUploadWorkDiagnostics indexUploadWork,
        std::vector<RasterInstanceStreamBatchBinding> batchBindings)
    {
        const RasterInstanceStreamFrameSlot& resident =
            frameSlots[frameSlot];
        stream.instances = resident.instances;
        stream.instanceIndices = resident.instanceIndices;
        stream.instanceCount = resident.instanceCount;
        stream.instanceUploadBytes = instanceUploadBytes;
        stream.indexUploadBytes = indexUploadBytes;
        stream.instancePatchedRowCount = instancePatchedRowCount;
        stream.indexPatchedRowCount = indexPatchedRowCount;
        stream.activeInstanceCount = resident.instanceCount;
        stream.activeInstanceCapacity = resident.instanceCapacity;
        stream.activeIndexCapacity = resident.indexCapacity;
        stream.instanceFullMaterialization = instanceFullMaterialization;
        stream.indexFullMaterialization = indexFullMaterialization;
        stream.instanceUploadWork = std::move(instanceUploadWork);
        stream.indexUploadWork = std::move(indexUploadWork);
        stream.batchBindings = std::move(batchBindings);
        stream.activeFrameSlot = frameSlot;
    }

    void ResetRasterInstanceStreamAttemptDiagnostics(
        RasterInstanceStream& stream) noexcept
    {
        stream.instanceUploadBytes = 0;
        stream.indexUploadBytes = 0;
        stream.instancePatchedRowCount = 0;
        stream.indexPatchedRowCount = 0;
        stream.activeInstanceCount = 0;
        stream.activeInstanceCapacity = 0;
        stream.activeIndexCapacity = 0;
        stream.instanceFullMaterialization = false;
        stream.indexFullMaterialization = false;
        stream.instanceUploadWork = {};
        stream.indexUploadWork = {};
        stream.batchBindings.clear();
    }

    void FailClosedRasterInstanceStreamSlot(
        RasterInstanceStream& stream,
        std::array<RasterInstanceStreamFrameSlot, RVX_MAX_FRAME_COUNT>&
            frameSlots,
        uint32 frameSlot)
    {
        // A multi-buffer materialization can publish its instance rows before
        // the index commit reports failure. That leaves the old CPU metadata
        // incompatible with the resident instance bytes, so this slot and its
        // current aliases must not remain observable as a valid generation.
        RenderUploadWorkDiagnostics instanceUploadWork =
            std::move(stream.instanceUploadWork);
        RenderUploadWorkDiagnostics indexUploadWork =
            std::move(stream.indexUploadWork);
        frameSlots[frameSlot] = {};
        stream.instances.Reset();
        stream.instanceIndices.Reset();
        stream.instanceCount = 0;
        ResetRasterInstanceStreamAttemptDiagnostics(stream);
        stream.instanceUploadWork = std::move(instanceUploadWork);
        stream.indexUploadWork = std::move(indexUploadWork);
        stream.batchBindings.clear();
        stream.activeFrameSlot = RVX_INVALID_INDEX;
    }
} // namespace

uint64 GetRasterTranscriptGroupIdentity(const RenderDrawGroupKey& key) noexcept
{
    // Resource slots/generations do not name a cross-process raster group.
    // Per-entry payload evidence carries the resolved mesh/material semantic
    // identity; no resource handle field participates here.
    uint64 hash = RASTER_TRANSCRIPT_FNV_OFFSET;
    HashRasterTranscriptValue(hash, static_cast<uint8>(key.pass));
    HashRasterTranscriptValue(
        hash, static_cast<uint8>(key.pipeline.materialVariant));
    HashRasterTranscriptValue(
        hash, static_cast<uint8>(key.pipeline.topology));
    HashRasterTranscriptValue(hash, key.pipeline.skinned);
    HashRasterTranscriptValue(hash, key.geometry.submeshIndex);
    HashRasterTranscriptValue(
        hash, static_cast<uint8>(key.geometry.indexType));
    HashRasterTranscriptValue(
        hash, static_cast<uint8>(key.material.materialMode));
    // textureBindingHash includes generational texture handles. It is useful
    // for resident binding, but is not a cross-process raster identity.
    HashRasterTranscriptValue(
        hash, key.instanceMaterial.parameterTableCompatible);
    HashRasterTranscriptValue(
        hash, static_cast<uint32>(key.layout.vertexStreams));
    HashRasterTranscriptValue(
        hash, static_cast<uint32>(key.layout.bindings));
    HashRasterTranscriptValue(
        hash, static_cast<uint8>(key.layout.primitiveDataBinding));
    HashRasterTranscriptValue(hash, key.indexCount);
    HashRasterTranscriptValue(hash, key.firstIndex);
    HashRasterTranscriptValue(hash, key.vertexOffset);
    HashRasterTranscriptValue(hash, static_cast<uint32>(key.flags));
    HashRasterTranscriptValue(hash, key.usesMaterialParameterTable);
    return hash;
}

RasterTranscriptEntryDigest MakeRasterTranscriptEntryDigest(
    uint64 groupIdentity,
    const RasterInstanceStreamKey& key,
    const GPUInstanceData& instance,
    uint64 semanticIdentity,
    bool materialParameterSlotConsumed,
    uint32 indexCount,
    uint32 firstIndex,
    int32 vertexOffset) noexcept
{
    RasterTranscriptEntryDigest digest;
    if (semanticIdentity == 0)
    {
        return digest;
    }
    digest.available = true;
    digest.identityHash = RASTER_TRANSCRIPT_FNV_OFFSET;
    HashRasterTranscriptValue(digest.identityHash, groupIdentity);
    HashRasterTranscriptValue(digest.identityHash, key.objectId);
    HashRasterTranscriptValue(digest.identityHash, key.submeshIndex);
    HashRasterTranscriptValue(
        digest.identityHash, semanticIdentity);

    digest.consumedPayloadHash = RASTER_TRANSCRIPT_FNV_OFFSET;
    HashRasterTranscriptValue(digest.consumedPayloadHash, groupIdentity);
    HashRasterTranscriptValue(digest.consumedPayloadHash, key.objectId);
    HashRasterTranscriptValue(digest.consumedPayloadHash, key.submeshIndex);
    HashRasterTranscriptValue(digest.consumedPayloadHash, instance.worldMatrix);
    HashRasterTranscriptValue(digest.consumedPayloadHash, instance.normalMatrix);
    // The physical mesh/material slots are renderer-local allocation details.
    // Every raster lane instead contributes the same Registry-derived
    // semantic identity, independent of descriptor vs parameter-table use.
    static_cast<void>(materialParameterSlotConsumed);
    HashRasterTranscriptValue(
        digest.consumedPayloadHash, semanticIdentity);
    // Index arguments are consumed from the submitted draw rather than the
    // private per-instance bookkeeping fields.  Callers provide the actual
    // direct or indirect command values so this remains an execution trace.
    HashRasterTranscriptValue(digest.consumedPayloadHash, indexCount);
    HashRasterTranscriptValue(digest.consumedPayloadHash, firstIndex);
    HashRasterTranscriptValue(digest.consumedPayloadHash, vertexOffset);
    // Bounds and force-visible are cull-stage values that choose this exact
    // indirect transcript. Including them distinguishes a visibility drift
    // without treating source/candidate/draw-group indices as identity.
    HashRasterTranscriptValue(
        digest.consumedPayloadHash, instance.boundingSphere);
    HashRasterTranscriptValue(digest.consumedPayloadHash, instance.aabbMin);
    HashRasterTranscriptValue(digest.consumedPayloadHash, instance.aabbMax);
    HashRasterTranscriptValue(digest.consumedPayloadHash, instance.forceVisible);
    return digest;
}

void BeginRasterTranscript(RasterTranscriptDigest& outDigest) noexcept
{
    outDigest.available = true;
    outDigest.entryCount = 0;
    outDigest.orderedIdentityHash = RASTER_TRANSCRIPT_FNV_OFFSET;
    outDigest.consumedPayloadHash = RASTER_TRANSCRIPT_FNV_OFFSET;
    outDigest.unorderedIdentityHash = 0;
    outDigest.unorderedIdentityHashSecondary = 0;
    outDigest.unorderedConsumedPayloadHash = 0;
    outDigest.unorderedConsumedPayloadHashSecondary = 0;
}

void AppendRasterTranscriptEntry(
    RasterTranscriptDigest& digest,
    const RasterTranscriptEntryDigest& entry) noexcept
{
    if (!digest.available || !entry.available ||
        digest.entryCount == std::numeric_limits<uint32>::max())
    {
        digest = {};
        return;
    }
    digest.orderedIdentityHash = MixRasterTranscriptHash(
        digest.orderedIdentityHash, entry.identityHash);
    digest.consumedPayloadHash = MixRasterTranscriptHash(
        digest.consumedPayloadHash, entry.consumedPayloadHash);
    // Addition makes this order-insensitive while preserving each duplicate
    // contribution (unlike XOR). Two independent domains reduce collisions.
    digest.unorderedIdentityHash += MakeRasterTranscriptMultisetContribution(
        entry.identityHash, 0x6A09E667F3BCC909ull);
    digest.unorderedIdentityHashSecondary +=
        MakeRasterTranscriptMultisetContribution(
            entry.identityHash, 0xBB67AE8584CAA73Bull);
    digest.unorderedConsumedPayloadHash +=
        MakeRasterTranscriptMultisetContribution(
            entry.consumedPayloadHash, 0x3C6EF372FE94F82Bull);
    digest.unorderedConsumedPayloadHashSecondary +=
        MakeRasterTranscriptMultisetContribution(
            entry.consumedPayloadHash, 0xA54FF53A5F1D36F1ull);
    ++digest.entryCount;
}

bool BuildRasterInstanceStreamTranscript(
    const RasterInstanceStream& stream,
    const RasterInstanceStreamCache& cache,
    RasterTranscriptDigest& outDigest) noexcept
{
    outDigest = {};
    if (!stream.IsValid() || stream.activeFrameSlot >= RVX_MAX_FRAME_COUNT)
    {
        return false;
    }

    const RasterInstanceStreamFrameSlot& resident =
        cache.frameSlots[stream.activeFrameSlot];
    if (!resident.IsValid() || resident.instances.Get() != stream.instances.Get() ||
        resident.instanceIndices.Get() != stream.instanceIndices.Get())
    {
        return false;
    }

    BeginRasterTranscript(outDigest);
    for (const RasterInstanceStreamBatchBinding& binding : stream.batchBindings)
    {
        if (binding.instanceCount == 0 ||
            binding.firstInstance > resident.residentDrawOrder.size() ||
            binding.instanceCount > resident.residentDrawOrder.size() -
                binding.firstInstance)
        {
            outDigest = {};
            return false;
        }

        const uint64 groupIdentity = GetRasterTranscriptGroupIdentity(binding.key);
        for (uint32 instanceOffset = 0;
             instanceOffset < binding.instanceCount;
             ++instanceOffset)
        {
            const uint32 drawOrderIndex = binding.firstInstance + instanceOffset;
            const uint32 residentRow = resident.residentDrawOrder[drawOrderIndex];
            if (residentRow >= resident.residentInstances.size() ||
                residentRow >= resident.residentRasterSemanticIdentities.size() ||
                residentRow >= resident.residentKeys.size())
            {
                outDigest = {};
                return false;
            }
            AppendRasterTranscriptEntry(
                outDigest,
                MakeRasterTranscriptEntryDigest(
                    groupIdentity,
                    resident.residentKeys[residentRow],
                    resident.residentInstances[residentRow],
                    resident.residentRasterSemanticIdentities[residentRow],
                    binding.key.usesMaterialParameterTable,
                    binding.key.indexCount,
                    binding.key.firstIndex,
                    binding.key.vertexOffset));
            // Semantic identity is diagnostic-only. An unavailable identity
            // invalidates this transcript without invalidating the already
            // materialized raster stream.
            if (!outDigest.available)
            {
                return true;
            }
        }
    }
    return true;
}

bool BuildCompleteDirectRasterTranscript(
    std::span<const DirectRasterTranscriptPlannedDraw> plannedDraws,
    const RenderScene& scene,
    const RenderResourceRegistry& resourceRegistry,
    const RasterInstanceStream* stream,
    const RasterInstanceStreamCache* cache,
    std::span<const uint64> rasterMaterialSemanticKeysBySlot,
    RasterTranscriptDigest& outDigest) noexcept
{
    outDigest = {};
    BeginRasterTranscript(outDigest);
    uint32 representedPacketCount = 0;
    const RasterInstanceStreamFrameSlot* resident = nullptr;
    if (std::any_of(plannedDraws.begin(), plannedDraws.end(),
                    [](const DirectRasterTranscriptPlannedDraw& planned)
                    { return planned.instanced; }))
    {
        if (stream == nullptr || cache == nullptr || !stream->IsValid() ||
            stream->activeFrameSlot >= RVX_MAX_FRAME_COUNT)
        {
            return false;
        }
        resident = &cache->frameSlots[stream->activeFrameSlot];
        if (!resident->IsValid() ||
            resident->instances.Get() != stream->instances.Get() ||
            resident->instanceIndices.Get() != stream->instanceIndices.Get())
        {
            return false;
        }
    }

    const auto append = [&outDigest, &representedPacketCount](
                            const RasterTranscriptEntryDigest& entry) noexcept
        -> bool
    {
        AppendRasterTranscriptEntry(outDigest, entry);
        if (!outDigest.available ||
            representedPacketCount == std::numeric_limits<uint32>::max())
        {
            outDigest = {};
            return false;
        }
        ++representedPacketCount;
        return true;
    };
    const auto resolveObject = [&scene](RenderObjectId objectId,
                                        PrimitiveDataIndex primitiveData)
        -> const RenderObject*
    {
        if (objectId == 0 || primitiveData >= scene.GetObjectCount())
        {
            return nullptr;
        }
        const RenderObject& indexedObject = scene.GetObject(primitiveData);
        if (indexedObject.entityId != objectId)
        {
            return nullptr;
        }
        // RenderScene publishes one object per stable object id. The packet's
        // primitiveData is the exact canonical row, so the row-local identity
        // check above is sufficient and keeps this CPU transcript helper free
        // of the non-inline RenderScene lookup implementation.
        return &indexedObject;
    };

    for (const DirectRasterTranscriptPlannedDraw& planned : plannedDraws)
    {
        const RenderDrawPacket& packet = planned.packet.packet;
        const RenderDrawArguments& arguments = packet.arguments;
        if (planned.representedPacketCount == 0 ||
            arguments.instanceCount != planned.representedPacketCount ||
            representedPacketCount >
                std::numeric_limits<uint32>::max() -
                    planned.representedPacketCount)
        {
            outDigest = {};
            return false;
        }

        if (planned.instanced)
        {
            const RasterInstanceStreamBatchBinding* binding = nullptr;
            for (const RasterInstanceStreamBatchBinding& candidate :
                 stream->batchBindings)
            {
                if (candidate.firstInstance == arguments.firstInstance &&
                    candidate.instanceCount == arguments.instanceCount)
                {
                    if (binding != nullptr)
                    {
                        outDigest = {};
                        return false;
                    }
                    binding = &candidate;
                }
            }
            if (binding == nullptr || resident == nullptr ||
                arguments.firstInstance > resident->residentDrawOrder.size() ||
                arguments.instanceCount > resident->residentDrawOrder.size() -
                    arguments.firstInstance)
            {
                outDigest = {};
                return false;
            }
            for (uint32 instanceOffset = 0;
                 instanceOffset < arguments.instanceCount;
                 ++instanceOffset)
            {
                const uint32 drawOrderIndex =
                    arguments.firstInstance + instanceOffset;
                const uint32 residentRow =
                    resident->residentDrawOrder[drawOrderIndex];
                if (residentRow >= resident->residentInstances.size() ||
                    residentRow >= resident->residentKeys.size() ||
                    residentRow >=
                        resident->residentRasterSemanticIdentities.size() ||
                    !append(MakeRasterTranscriptEntryDigest(
                        GetRasterTranscriptGroupIdentity(binding->key),
                        resident->residentKeys[residentRow],
                        resident->residentInstances[residentRow],
                        resident->residentRasterSemanticIdentities[residentRow],
                        binding->key.usesMaterialParameterTable,
                        arguments.indexCount,
                        arguments.firstIndex,
                        arguments.vertexOffset)))
                {
                    outDigest = {};
                    return false;
                }
            }
            continue;
        }

        const RenderObject* const object = resolveObject(
            packet.objectId, packet.primitiveData);
        if (object == nullptr || object->mesh != packet.geometryKey.mesh)
        {
            outDigest = {};
            return false;
        }
        const RenderVisibilityGPUInput visibility =
            MakeRenderVisibilityGPUInput(object->bounds);
        GPUInstanceData instance{};
        instance.worldMatrix = object->worldMatrix;
        instance.normalMatrix = object->normalMatrix;
        const Vec3 center = visibility.forceVisible == 0
            ? object->bounds.GetCenter() : Vec3(0.0f);
        const float radius = visibility.forceVisible == 0
            ? length(object->bounds.GetExtent()) : 0.0f;
        instance.boundingSphere = Vec4(center, radius);
        instance.aabbMin = visibility.aabbMin;
        instance.aabbMax = visibility.aabbMax;
        instance.meshId = packet.geometryKey.mesh.slot;
        instance.materialId = planned.usesMaterialParameterTable
            ? packet.materialKey.material.slot : RVX_INVALID_INDEX;
        instance.indexCount = arguments.indexCount;
        instance.firstIndex = arguments.firstIndex;
        instance.vertexOffset = arguments.vertexOffset;
        instance.forceVisible = visibility.forceVisible;

        uint64 rasterMaterialKey = planned.fixedRasterMaterialKey;
        if (planned.usesMaterialParameterTable)
        {
            if (instance.materialId >= rasterMaterialSemanticKeysBySlot.size())
            {
                outDigest = {};
                return false;
            }
            rasterMaterialKey =
                rasterMaterialSemanticKeysBySlot[instance.materialId];
        }
        const std::optional<uint64> semanticIdentity =
            resourceRegistry.CombineRasterMeshAndMaterialSemanticIdentity(
                packet.geometryKey.mesh, rasterMaterialKey);
        if (!semanticIdentity ||
            !append(MakeRasterTranscriptEntryDigest(
                GetRasterTranscriptGroupIdentity(MakeRenderInstanceBatchKey(
                    packet, planned.packet.layout)),
                {packet.objectId, packet.submeshIndex},
                instance,
                *semanticIdentity,
                planned.usesMaterialParameterTable,
                arguments.indexCount,
                arguments.firstIndex,
                arguments.vertexOffset)))
        {
            outDigest = {};
            return false;
        }
    }

    if (outDigest.entryCount != representedPacketCount)
    {
        outDigest = {};
        return false;
    }
    return true;
}

bool FinalizeRasterInstanceStreamSemanticSidecar(
    const RasterInstanceStream& stream,
    RasterInstanceStreamCache& cache,
    std::vector<uint64>&& semanticIdentitiesByResidentRow) noexcept
{
    if (!stream.IsValid() || stream.activeFrameSlot >= RVX_MAX_FRAME_COUNT)
    {
        return false;
    }
    RasterInstanceStreamFrameSlot& resident =
        cache.frameSlots[stream.activeFrameSlot];
    if (!resident.IsValid() ||
        resident.instances.Get() != stream.instances.Get() ||
        resident.instanceIndices.Get() != stream.instanceIndices.Get() ||
        semanticIdentitiesByResidentRow.size() != resident.instanceCapacity)
    {
        return false;
    }

    // Move assignment is allocation-free for the caller-owned vector and is
    // intentionally the only mutation: diagnostic evidence never dirties a
    // canonical row or changes the submitted instance/index buffers.
    resident.residentRasterSemanticIdentities =
        std::move(semanticIdentitiesByResidentRow);
    return true;
}

RHIBufferRef CreateRasterInstanceIndexBuffer(IRHIDevice& device,
                                             uint32 instanceCapacity,
                                             const char* debugName)
{
    if (instanceCapacity == 0)
    {
        return {};
    }
    RHIBufferDesc desc;
    desc.size = static_cast<uint64>(instanceCapacity) * sizeof(uint32);
    desc.usage = RHIBufferUsage::Vertex;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(uint32);
    desc.debugName = debugName ? debugName : "RasterInstanceIndices";
    RHIBufferRef buffer = device.CreateBuffer(desc);
    RHIMappedWriteAccess access = buffer
        ? buffer->MapWriteRange(0, static_cast<uint64>(instanceCapacity) * sizeof(uint32))
        : RHIMappedWriteAccess{};
    uint32* mapped = static_cast<uint32*>(access.GetData());
    if (mapped == nullptr)
    {
        return {};
    }
    for (uint32 index = 0; index < instanceCapacity; ++index)
    {
        mapped[index] = index;
    }
    if (!buffer->CommitMappedWriteRange(std::move(access)).IsPublished())
    {
        return {};
    }
    return buffer;
}

namespace
{
    [[nodiscard]] bool CreateRasterInstanceStreamWithCache(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        std::span<const RasterInstanceStreamKey> keys,
        std::span<const RasterInstanceStreamBatch> batches,
        std::span<const uint64> semanticIdentities,
        const char* debugName,
        std::array<RasterInstanceStreamFrameSlot, RVX_MAX_FRAME_COUNT>&
            frameSlots,
        RasterInstanceStreamCache* cache,
        RasterInstanceStream& outStream)
{
    // Failures must not report any rows as successfully materialized, even
    // when a prior slot remains resident for a later retry.
    ResetRasterInstanceStreamAttemptDiagnostics(outStream);
    if (instances.empty() ||
        instances.size() != keys.size() ||
        (!semanticIdentities.empty() &&
         semanticIdentities.size() != instances.size()) ||
        batches.empty() ||
        instances.size() > static_cast<size_t>(std::numeric_limits<uint32>::max()) ||
        instances.size() >
            std::numeric_limits<size_t>::max() / sizeof(GPUInstanceData))
    {
        return false;
    }

    const uint32 instanceCount = static_cast<uint32>(instances.size());
    const uint32 frameSlot =
        device.GetCurrentFrameIndex() % RVX_MAX_FRAME_COUNT;
    RasterInstanceStreamFrameSlot& resident =
        frameSlots[frameSlot];
    const bool residentValid = resident.IsValid();

    std::vector<RasterInstanceStreamKey> candidateKeys;
    std::vector<uint32> targetRows;
    std::vector<uint32> addedInstanceIndices;
    std::vector<uint32> changedInstanceRows;
    std::vector<uint32> changedIndexRows;
    std::vector<uint32> candidateDrawOrder;
    std::vector<GPUInstanceData> candidateInstances;
    std::vector<uint64> candidateSemanticIdentities;
    std::vector<bool> addedInstanceFlags;
    std::vector<uint32> inputIndexByRow;
    std::vector<RasterInstanceStreamBatchResidency> candidateBatches;
    std::vector<RasterInstanceStreamBatchBinding> candidateBindings;
    std::unordered_set<RasterInstanceStreamKey, RasterInstanceStreamKeyHash>
        incomingKeys;
    std::unordered_map<RasterInstanceStreamKey,
                       uint32,
                       RasterInstanceStreamKeyHash> rowByKey;
    std::unordered_set<RenderDrawGroupKey, RenderDrawGroupKeyHasher>
        incomingBatchKeys;
    std::unordered_map<RenderDrawGroupKey,
                       uint32,
                       RenderDrawGroupKeyHasher> batchIndexByKey;
    try
    {
        uint64 expectedFirstInstance = 0;
        incomingBatchKeys.reserve(batches.size());
        for (const RasterInstanceStreamBatch& batch : batches)
        {
            if (batch.instanceCount == 0 ||
                batch.inputFirstInstance != expectedFirstInstance ||
                batch.instanceCount > instanceCount - batch.inputFirstInstance ||
                !incomingBatchKeys.emplace(batch.key).second)
            {
                return false;
            }
            expectedFirstInstance += batch.instanceCount;
        }
        if (expectedFirstInstance != instanceCount)
        {
            return false;
        }

        incomingKeys.reserve(instanceCount);
        for (const RasterInstanceStreamKey& key : keys)
        {
            if (!IsValidRasterInstanceStreamKey(key) ||
                !incomingKeys.emplace(key).second)
            {
                return false;
            }
        }

        const uint32 initialCapacity = residentValid
            ? resident.instanceCapacity
            : 0;
        const uint32 candidateCapacity = std::max(initialCapacity,
                                                   instanceCount);
        candidateKeys = residentValid ? resident.residentKeys
                                       : std::vector<RasterInstanceStreamKey>{};
        candidateKeys.resize(candidateCapacity);
        candidateSemanticIdentities = residentValid
            ? resident.residentRasterSemanticIdentities
            : std::vector<uint64>{};
        candidateSemanticIdentities.resize(candidateCapacity, 0);
        targetRows.assign(instanceCount, RVX_INVALID_INDEX);

        for (uint32 row = 0; row < candidateCapacity; ++row)
        {
            RasterInstanceStreamKey& key = candidateKeys[row];
            if (IsValidRasterInstanceStreamKey(key) &&
                !incomingKeys.contains(key))
            {
                key = {};
                candidateSemanticIdentities[row] = 0;
            }
        }

        rowByKey.reserve(instanceCount);
        for (uint32 row = 0; row < candidateCapacity; ++row)
        {
            const RasterInstanceStreamKey& key = candidateKeys[row];
            if (IsValidRasterInstanceStreamKey(key) &&
                !rowByKey.emplace(key, row).second)
            {
                return false;
            }
        }
        for (uint32 index = 0; index < instanceCount; ++index)
        {
            const auto existing = rowByKey.find(keys[index]);
            if (existing != rowByKey.end())
            {
                targetRows[index] = existing->second;
            }
            else
            {
                addedInstanceIndices.push_back(index);
            }
        }
        std::sort(addedInstanceIndices.begin(), addedInstanceIndices.end(),
            [&keys](uint32 lhs, uint32 rhs)
            {
                return RasterInstanceStreamKeyLess(keys[lhs], keys[rhs]);
            });
        uint32 nextFreeRow = 0;
        for (uint32 index : addedInstanceIndices)
        {
            while (nextFreeRow < candidateCapacity &&
                   IsValidRasterInstanceStreamKey(
                       candidateKeys[nextFreeRow]))
            {
                ++nextFreeRow;
            }
            if (nextFreeRow == candidateCapacity)
            {
                return false;
            }
            candidateKeys[nextFreeRow] = keys[index];
            if (!rowByKey.emplace(keys[index], nextFreeRow).second)
            {
                return false;
            }
            targetRows[index] = nextFreeRow++;
        }

        candidateDrawOrder = targetRows;
        addedInstanceFlags.assign(instanceCount, false);
        for (uint32 index : addedInstanceIndices)
        {
            addedInstanceFlags[index] = true;
        }
        inputIndexByRow.assign(candidateCapacity, RVX_INVALID_INDEX);
        for (uint32 index = 0; index < instanceCount; ++index)
        {
            const uint32 row = targetRows[index];
            if (row >= candidateCapacity ||
                inputIndexByRow[row] != RVX_INVALID_INDEX)
            {
                return false;
            }
            inputIndexByRow[row] = index;
            candidateSemanticIdentities[row] = semanticIdentities.empty()
                ? 0
                : semanticIdentities[index];
        }

        candidateBatches = residentValid
            ? resident.residentBatches
            : std::vector<RasterInstanceStreamBatchResidency>{};
        candidateDrawOrder = residentValid
            ? resident.residentDrawOrder
            : std::vector<uint32>{};
        uint32 candidateIndexCapacity = residentValid
            ? resident.indexCapacity
            : 0;
        batchIndexByKey.reserve(candidateBatches.size() + batches.size());
        for (uint32 index = 0;
             index < static_cast<uint32>(candidateBatches.size());
             ++index)
        {
            const RasterInstanceStreamBatchResidency& batch =
                candidateBatches[index];
            if (!batch.IsValid(candidateIndexCapacity) ||
                !batchIndexByKey.emplace(batch.key, index).second)
            {
                return false;
            }
        }

        std::vector<bool> batchSeen(candidateBatches.size(), false);
        candidateBindings.reserve(batches.size());
        for (const RasterInstanceStreamBatch& inputBatch : batches)
        {
            uint32 batchIndex = RVX_INVALID_INDEX;
            const auto existingBatch = batchIndexByKey.find(inputBatch.key);
            if (existingBatch == batchIndexByKey.end())
            {
                if (inputBatch.instanceCount >
                    std::numeric_limits<uint32>::max() - candidateIndexCapacity)
                {
                    return false;
                }
                batchIndex = static_cast<uint32>(candidateBatches.size());
                RasterInstanceStreamBatchResidency newBatch;
                newBatch.key = inputBatch.key;
                newBatch.indexSegmentBase = candidateIndexCapacity;
                newBatch.indexSegmentCapacity = inputBatch.instanceCount;
                candidateIndexCapacity += inputBatch.instanceCount;
                candidateBatches.push_back(std::move(newBatch));
                batchSeen.push_back(false);
                if (!batchIndexByKey.emplace(inputBatch.key, batchIndex).second)
                {
                    return false;
                }
            }
            else
            {
                batchIndex = existingBatch->second;
            }

            RasterInstanceStreamBatchResidency& candidateBatch =
                candidateBatches[batchIndex];
            if (inputBatch.instanceCount > candidateBatch.indexSegmentCapacity)
            {
                uint32 expandedCapacity = 0;
                if (!GrowRasterInstanceSegmentCapacity(
                        candidateBatch.indexSegmentCapacity,
                        inputBatch.instanceCount,
                        expandedCapacity))
                {
                    return false;
                }
                const bool isPhysicalTail =
                    candidateBatch.indexSegmentBase <= candidateIndexCapacity &&
                    candidateBatch.indexSegmentCapacity <=
                        candidateIndexCapacity - candidateBatch.indexSegmentBase &&
                    candidateBatch.indexSegmentBase +
                            candidateBatch.indexSegmentCapacity ==
                        candidateIndexCapacity;
                if (isPhysicalTail)
                {
                    const uint32 growth = expandedCapacity -
                        candidateBatch.indexSegmentCapacity;
                    if (growth > std::numeric_limits<uint32>::max() -
                        candidateIndexCapacity)
                    {
                        return false;
                    }
                    candidateBatch.indexSegmentCapacity = expandedCapacity;
                    candidateIndexCapacity += growth;
                }
                else
                {
                    if (expandedCapacity >
                        std::numeric_limits<uint32>::max() - candidateIndexCapacity)
                    {
                        return false;
                    }
                    // A non-tail segment cannot grow without moving a later
                    // group. Migrate only this group to an appended geometric
                    // segment and retain its active keys for local patching.
                    candidateBatch.indexSegmentBase = candidateIndexCapacity;
                    candidateBatch.indexSegmentCapacity = expandedCapacity;
                    candidateIndexCapacity += expandedCapacity;
                }
            }
            if (candidateDrawOrder.size() < candidateIndexCapacity)
            {
                candidateDrawOrder.resize(candidateIndexCapacity, 0);
            }

            std::unordered_map<RasterInstanceStreamKey,
                               uint32,
                               RasterInstanceStreamKeyHash> inputIndexByKey;
            inputIndexByKey.reserve(inputBatch.instanceCount);
            const uint32 inputEnd = inputBatch.inputFirstInstance +
                inputBatch.instanceCount;
            for (uint32 inputIndex = inputBatch.inputFirstInstance;
                 inputIndex < inputEnd;
                 ++inputIndex)
            {
                if (!inputIndexByKey.emplace(keys[inputIndex], inputIndex).second)
                {
                    return false;
                }
            }

            if (inputBatch.preserveInputOrder)
            {
                candidateBatch.activeKeys.clear();
                candidateBatch.activeKeys.reserve(inputBatch.instanceCount);
                for (uint32 inputIndex = inputBatch.inputFirstInstance;
                     inputIndex < inputEnd;
                     ++inputIndex)
                {
                    candidateBatch.activeKeys.push_back(keys[inputIndex]);
                }
            }
            else
            {
                // Preserve every still-live key already located in the new
                // active prefix. Removed positions are holes. Fill those holes
                // from old tail survivors first, then deterministic additions.
                // This bounds replacement, removal, and addition churn by the
                // identity delta instead of reordering the whole group.
                const uint32 oldActiveCount = static_cast<uint32>(
                    candidateBatch.activeKeys.size());
                const uint32 preservedPrefixCount = std::min(
                    oldActiveCount, inputBatch.instanceCount);
                std::vector<RasterInstanceStreamKey> nextActiveKeys(
                    inputBatch.instanceCount);
                std::vector<uint32> holes;
                holes.reserve(inputBatch.instanceCount);
                std::unordered_set<RasterInstanceStreamKey,
                                   RasterInstanceStreamKeyHash> oldGroupKeys;
                std::unordered_set<RasterInstanceStreamKey,
                                   RasterInstanceStreamKeyHash> assignedKeys;
                oldGroupKeys.reserve(oldActiveCount);
                assignedKeys.reserve(inputBatch.instanceCount);
                for (const RasterInstanceStreamKey& key :
                     candidateBatch.activeKeys)
                {
                    if (!oldGroupKeys.emplace(key).second)
                    {
                        return false;
                    }
                }
                for (uint32 index = 0; index < preservedPrefixCount; ++index)
                {
                    const RasterInstanceStreamKey& key =
                        candidateBatch.activeKeys[index];
                    if (inputIndexByKey.contains(key))
                    {
                        nextActiveKeys[index] = key;
                        assignedKeys.emplace(key);
                    }
                    else
                    {
                        holes.push_back(index);
                    }
                }
                for (uint32 index = preservedPrefixCount;
                     index < inputBatch.instanceCount;
                     ++index)
                {
                    holes.push_back(index);
                }

                std::vector<RasterInstanceStreamKey> holeFillKeys;
                holeFillKeys.reserve(holes.size());
                for (uint32 index = preservedPrefixCount;
                     index < oldActiveCount;
                     ++index)
                {
                    const RasterInstanceStreamKey& key =
                        candidateBatch.activeKeys[index];
                    if (inputIndexByKey.contains(key) &&
                        assignedKeys.emplace(key).second)
                    {
                        holeFillKeys.push_back(key);
                    }
                }

                std::vector<uint32> addedInputIndices;
                addedInputIndices.reserve(inputBatch.instanceCount);
                for (uint32 inputIndex = inputBatch.inputFirstInstance;
                     inputIndex < inputEnd;
                     ++inputIndex)
                {
                    if (!oldGroupKeys.contains(keys[inputIndex]))
                    {
                        addedInputIndices.push_back(inputIndex);
                    }
                }
                std::sort(addedInputIndices.begin(), addedInputIndices.end(),
                    [&keys](uint32 lhs, uint32 rhs)
                    {
                        return RasterInstanceStreamKeyLess(keys[lhs], keys[rhs]);
                    });
                for (uint32 inputIndex : addedInputIndices)
                {
                    if (!assignedKeys.emplace(keys[inputIndex]).second)
                    {
                        return false;
                    }
                    holeFillKeys.push_back(keys[inputIndex]);
                }
                if (holeFillKeys.size() != holes.size())
                {
                    return false;
                }
                for (uint32 index = 0;
                     index < static_cast<uint32>(holes.size());
                     ++index)
                {
                    nextActiveKeys[holes[index]] = holeFillKeys[index];
                }
                candidateBatch.activeKeys = std::move(nextActiveKeys);
            }
            candidateBatch.activeCount = static_cast<uint32>(
                candidateBatch.activeKeys.size());
            if (candidateBatch.activeCount != inputBatch.instanceCount)
            {
                return false;
            }

            for (uint32 index = 0; index < candidateBatch.activeCount; ++index)
            {
                const auto input = inputIndexByKey.find(
                    candidateBatch.activeKeys[index]);
                if (input == inputIndexByKey.end())
                {
                    return false;
                }
                const uint32 targetRow = targetRows[input->second];
                const uint32 indexRow = candidateBatch.indexSegmentBase + index;
                if (targetRow >= candidateCapacity ||
                    indexRow >= candidateDrawOrder.size())
                {
                    return false;
                }
                if (candidateDrawOrder[indexRow] != targetRow)
                {
                    candidateDrawOrder[indexRow] = targetRow;
                    changedIndexRows.push_back(indexRow);
                }
            }
            batchSeen[batchIndex] = true;
            candidateBindings.push_back({inputBatch.key,
                                         candidateBatch.indexSegmentBase,
                                         candidateBatch.activeCount});
        }
        for (uint32 index = 0;
             index < static_cast<uint32>(candidateBatches.size());
             ++index)
        {
            if (!batchSeen[index])
            {
                candidateBatches[index].activeKeys.clear();
                candidateBatches[index].activeCount = 0;
            }
        }

        uint64 activeInstanceCount = 0;
        for (const RasterInstanceStreamBatchResidency& batch : candidateBatches)
        {
            if (!batch.IsValid(candidateIndexCapacity))
            {
                return false;
            }
            activeInstanceCount += batch.activeCount;
        }
        if (activeInstanceCount != instanceCount ||
            candidateDrawOrder.size() != candidateIndexCapacity)
        {
            return false;
        }

        if (residentValid)
        {
            for (uint32 index = 0; index < instanceCount; ++index)
            {
                const uint32 row = targetRows[index];
                if (addedInstanceFlags[index] ||
                    !IsSameInstanceData(resident.residentInstances[row],
                                        instances[index]))
                {
                    changedInstanceRows.push_back(row);
                }
            }
        }
        else
        {
            candidateInstances.assign(candidateCapacity, {});
            for (uint32 index = 0; index < instanceCount; ++index)
            {
                candidateInstances[targetRows[index]] = instances[index];
            }
        }
    }
    catch (...)
    {
        return false;
    }

    const uint32 candidateIndexCapacity = static_cast<uint32>(
        candidateDrawOrder.size());
    const bool needsNewGeneration = !residentValid ||
        resident.instanceCapacity != candidateKeys.size() ||
        resident.indexCapacity != candidateIndexCapacity;
    if (needsNewGeneration && candidateInstances.empty())
    {
        try
        {
            candidateInstances.assign(candidateKeys.size(), {});
            for (uint32 index = 0; index < instanceCount; ++index)
            {
                const uint32 row = targetRows[index];
                if (row >= candidateInstances.size())
                {
                    return false;
                }
                candidateInstances[row] = instances[index];
            }
        }
        catch (...)
        {
            return false;
        }
    }
    RHIBufferRef instanceBuffer = needsNewGeneration ? RHIBufferRef{}
                                                       : resident.instances;
    RHIBufferRef indexBuffer = needsNewGeneration ? RHIBufferRef{}
                                                    : resident.instanceIndices;
    if (needsNewGeneration &&
        !CreatePersistentRasterInstanceBuffers(device,
                                               static_cast<uint32>(
                                                   candidateKeys.size()),
                                               candidateIndexCapacity,
                                               debugName,
                                               instanceBuffer,
                                               indexBuffer))
    {
        return false;
    }

    RenderUploadWorkDiagnostics instanceUploadWork;
    RenderUploadWorkDiagnostics indexUploadWork;
    const std::vector<RasterUploadRange> instanceRanges = needsNewGeneration
        ? std::vector<RasterUploadRange>{{0,
            static_cast<uint32>(candidateInstances.size())}}
        : MakeContiguousRanges(changedInstanceRows);
    const std::vector<RasterUploadRange> indexRanges = needsNewGeneration
        ? std::vector<RasterUploadRange>{{0,
            candidateIndexCapacity}}
        : MakeContiguousRanges(changedIndexRows);
    const auto writeInstanceRanges = [&]()
    {
        for (const RasterUploadRange range : instanceRanges)
        {
            const uint64 byteOffset = static_cast<uint64>(range.first) *
                sizeof(GPUInstanceData);
            const uint64 byteCount = static_cast<uint64>(range.count) *
                sizeof(GPUInstanceData);
            RHIMappedWriteAccess access = instanceBuffer->MapWriteRange(
                byteOffset, byteCount);
            if (!access.IsValid())
            {
                return false;
            }
            GPUInstanceData* destination = static_cast<GPUInstanceData*>(
                access.GetData());
            if (needsNewGeneration)
            {
                std::memcpy(destination,
                            candidateInstances.data() + range.first,
                            static_cast<size_t>(byteCount));
            }
            else
            {
                for (uint32 offset = 0; offset < range.count; ++offset)
                {
                    const uint32 row = range.first + offset;
                    const uint32 inputIndex = inputIndexByRow[row];
                    if (inputIndex == RVX_INVALID_INDEX)
                    {
                        return false;
                    }
                    destination[offset] = instances[inputIndex];
                }
            }
            RecordMappedUploadRange(instanceUploadWork, byteCount);
            const RHIHostWriteReceipt receipt =
                instanceBuffer->CommitMappedWriteRange(std::move(access));
            if (!receipt.IsPublished())
            {
                return false;
            }
            RecordCommittedUploadRange(instanceUploadWork, receipt, byteCount);
        }
        return true;
    };
    const auto writeIndexRanges = [&]()
    {
        for (const RasterUploadRange range : indexRanges)
        {
            const uint64 byteOffset = static_cast<uint64>(range.first) *
                sizeof(uint32);
            const uint64 byteCount = static_cast<uint64>(range.count) *
                sizeof(uint32);
            RHIMappedWriteAccess access = indexBuffer->MapWriteRange(
                byteOffset, byteCount);
            if (!access.IsValid())
            {
                return false;
            }
            std::memcpy(access.GetData(),
                        candidateDrawOrder.data() + range.first,
                        static_cast<size_t>(byteCount));
            RecordMappedUploadRange(indexUploadWork, byteCount);
            const RHIHostWriteReceipt receipt =
                indexBuffer->CommitMappedWriteRange(std::move(access));
            if (!receipt.IsPublished())
            {
                return false;
            }
            RecordCommittedUploadRange(indexUploadWork, receipt, byteCount);
        }
        return true;
    };

    if (!writeInstanceRanges() || !writeIndexRanges())
    {
        outStream.instanceUploadWork = std::move(instanceUploadWork);
        outStream.indexUploadWork = std::move(indexUploadWork);
        FailClosedRasterInstanceStreamSlot(outStream, frameSlots, frameSlot);
        return false;
    }

    const uint64 instanceUploadBytes = needsNewGeneration
        ? static_cast<uint64>(candidateInstances.size()) *
              sizeof(GPUInstanceData)
        : static_cast<uint64>(changedInstanceRows.size()) *
              sizeof(GPUInstanceData);
    const uint64 indexUploadBytes = needsNewGeneration
        ? static_cast<uint64>(candidateIndexCapacity) * sizeof(uint32)
        : static_cast<uint64>(changedIndexRows.size()) * sizeof(uint32);
    const uint32 instancePatchedRowCount = needsNewGeneration
        ? static_cast<uint32>(candidateInstances.size())
        : static_cast<uint32>(changedInstanceRows.size());
    const uint32 indexPatchedRowCount = needsNewGeneration
        ? candidateIndexCapacity
        : static_cast<uint32>(changedIndexRows.size());
    if (needsNewGeneration)
    {
        resident.instances = std::move(instanceBuffer);
        resident.instanceIndices = std::move(indexBuffer);
        resident.residentInstances = std::move(candidateInstances);
        resident.residentRasterSemanticIdentities =
            std::move(candidateSemanticIdentities);
        resident.residentKeys = std::move(candidateKeys);
    }
    else
    {
        for (uint32 row : changedInstanceRows)
        {
            const uint32 inputIndex = inputIndexByRow[row];
            resident.residentInstances[row] = instances[inputIndex];
        }
        resident.residentRasterSemanticIdentities =
            std::move(candidateSemanticIdentities);
        resident.residentKeys = std::move(candidateKeys);
    }
    resident.residentDrawOrder = std::move(candidateDrawOrder);
    resident.residentBatches = std::move(candidateBatches);
    resident.instanceCount = instanceCount;
    resident.instanceCapacity = static_cast<uint32>(resident.residentKeys.size());
    resident.indexCapacity = candidateIndexCapacity;
    PublishActiveRasterInstanceStream(outStream,
                                      frameSlots,
                                      frameSlot,
                                      instanceUploadBytes,
                                      indexUploadBytes,
                                      instancePatchedRowCount,
                                      indexPatchedRowCount,
                                      needsNewGeneration,
                                      needsNewGeneration,
                                      std::move(instanceUploadWork),
                                      std::move(indexUploadWork),
                                      std::move(candidateBindings));
    if (cache != nullptr)
    {
        SaturatingAdd(cache->mutationTotals.instancePatchedRowCount,
                      instancePatchedRowCount,
                      cache->mutationTotalsSaturated);
        SaturatingAdd(cache->mutationTotals.indexPatchedRowCount,
                      indexPatchedRowCount,
                      cache->mutationTotalsSaturated);
        SaturatingAdd(cache->mutationTotals.instanceUploadBytes,
                      instanceUploadBytes,
                      cache->mutationTotalsSaturated);
        SaturatingAdd(cache->mutationTotals.indexUploadBytes,
                      indexUploadBytes,
                      cache->mutationTotalsSaturated);
        if (needsNewGeneration)
        {
            SaturatingAdd(cache->mutationTotals.instanceFullMaterializationCount,
                          1,
                          cache->mutationTotalsSaturated);
            SaturatingAdd(cache->mutationTotals.indexFullMaterializationCount,
                          1,
                          cache->mutationTotalsSaturated);
        }
    }
    return true;
}
} // namespace

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                std::span<const RasterInstanceStreamKey> keys,
                                std::span<const RasterInstanceStreamBatch> batches,
                                const char* debugName,
                                RasterInstanceStream& outStream,
                                std::span<const uint64> semanticIdentities)
{
    return CreateRasterInstanceStreamWithCache(device,
                                               instances,
                                               keys,
                                               batches,
                                               semanticIdentities,
                                               debugName,
                                               outStream.frameSlots,
                                               nullptr,
                                               outStream);
}

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                std::span<const RasterInstanceStreamKey> keys,
                                std::span<const RasterInstanceStreamBatch> batches,
                                const char* debugName,
                                RasterInstanceStreamCache& cache,
                                RasterInstanceStream& outStream,
                                std::span<const uint64> semanticIdentities)
{
    return CreateRasterInstanceStreamWithCache(device,
                                               instances,
                                               keys,
                                               batches,
                                               semanticIdentities,
                                               debugName,
                                               cache.frameSlots,
                                               &cache,
                                               outStream);
}

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                std::span<const RasterInstanceStreamKey> keys,
                                const char* debugName,
                                RasterInstanceStream& outStream,
                                std::span<const uint64> semanticIdentities)
{
    const RasterInstanceStreamBatch implicitBatch{
        {}, 0, static_cast<uint32>(instances.size()), true};
    return CreateRasterInstanceStream(device,
                                      instances,
                                      keys,
                                      std::span(&implicitBatch, 1),
                                      debugName,
                                      outStream,
                                      semanticIdentities);
}

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                std::span<const RasterInstanceStreamKey> keys,
                                const char* debugName,
                                RasterInstanceStreamCache& cache,
                                RasterInstanceStream& outStream,
                                std::span<const uint64> semanticIdentities)
{
    const RasterInstanceStreamBatch implicitBatch{
        {}, 0, static_cast<uint32>(instances.size()), true};
    return CreateRasterInstanceStream(device,
                                      instances,
                                      keys,
                                      std::span(&implicitBatch, 1),
                                      debugName,
                                      cache,
                                      outStream,
                                      semanticIdentities);
}

bool BuildRasterInstanceData(
    const RenderInstanceBatchPlan& plan,
    const DirectDrawPacketBatch& directBatch,
    const RenderScene& scene,
    std::vector<GPUInstanceData>& outInstances,
    std::vector<uint64>& outSemanticIdentities,
    std::vector<RasterInstanceStreamKey>& outKeys,
    std::vector<RasterInstanceStreamBatch>& outBatches,
    const RenderResourceRegistry* resourceRegistry)
{
    // Material semantics must be finalized from the actual descriptor/table
    // outcome in OpaquePass. Stream construction deliberately carries only
    // placeholder sidecar values and performs no material resolution.
    static_cast<void>(resourceRegistry);
    outInstances.clear();
    outSemanticIdentities.clear();
    outKeys.clear();
    outBatches.clear();
    if (!plan.IsComplete() ||
        plan.executedPacketCount != directBatch.packets.size())
    {
        return false;
    }

    uint32 expectedInstanceCount = 0;
    for (const RenderInstanceBatch& batch : plan.batches)
    {
        if (batch.instanced)
        {
            expectedInstanceCount += static_cast<uint32>(batch.members.size());
        }
    }
    outInstances.reserve(expectedInstanceCount);
    outSemanticIdentities.reserve(expectedInstanceCount);
    outKeys.reserve(expectedInstanceCount);
    outBatches.reserve(plan.instancedBatchCount);
    for (const RenderInstanceBatch& batch : plan.batches)
    {
        if (!batch.instanced)
        {
            continue;
        }
        if (batch.members.empty() || batch.firstInstance != outInstances.size())
        {
            outInstances.clear();
            outSemanticIdentities.clear();
            outKeys.clear();
            outBatches.clear();
            return false;
        }
        const uint32 inputFirstInstance = static_cast<uint32>(outInstances.size());
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= directBatch.packets.size())
            {
                outInstances.clear();
                outSemanticIdentities.clear();
                outKeys.clear();
                outBatches.clear();
                return false;
            }
            const DirectDrawPacket& draw =
                directBatch.packets[member.directPacketIndex];
            const RenderDrawPacket& packet = draw.packet;
            if (packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
                packet.primitiveData >= scene.GetObjectCount())
            {
                outInstances.clear();
                outSemanticIdentities.clear();
                outKeys.clear();
                outBatches.clear();
                return false;
            }
            const RenderObject& object = scene.GetObject(packet.primitiveData);
            if (packet.objectId == 0 || object.entityId != packet.objectId ||
                object.mesh != packet.geometryKey.mesh)
            {
                outInstances.clear();
                outSemanticIdentities.clear();
                outKeys.clear();
                outBatches.clear();
                return false;
            }

            GPUInstanceData instance{};
            instance.worldMatrix = object.worldMatrix;
            instance.normalMatrix = object.normalMatrix;
            const RenderVisibilityGPUInput visibility =
                MakeRenderVisibilityGPUInput(object.bounds);
            const Vec3 center = visibility.forceVisible == 0
                ? object.bounds.GetCenter() : Vec3(0.0f);
            const float radius = visibility.forceVisible == 0
                ? length(object.bounds.GetExtent()) : 0.0f;
            instance.boundingSphere = Vec4(center, radius);
            instance.aabbMin = visibility.aabbMin;
            instance.aabbMax = visibility.aabbMax;
            instance.meshId = packet.geometryKey.mesh.slot;
            instance.materialId = batch.key.usesMaterialParameterTable
                ? packet.materialKey.material.slot
                : RVX_INVALID_INDEX;
            instance.indexCount = packet.arguments.indexCount;
            instance.firstIndex = packet.arguments.firstIndex;
            instance.vertexOffset = packet.arguments.vertexOffset;
            // Direct raster resolves source order through the persistent index
            // stream. Keep GPU-culling-only packet indices invalid so a
            // draw-order change cannot dirty an otherwise static instance row.
            instance.forceVisible = visibility.forceVisible;
            outInstances.push_back(instance);
            outSemanticIdentities.push_back(0);
            outKeys.push_back({packet.objectId, packet.submeshIndex});
        }
        outBatches.push_back({batch.key,
                              inputFirstInstance,
                              static_cast<uint32>(batch.members.size()),
                              true});
    }
    if (outInstances.size() != expectedInstanceCount ||
        outSemanticIdentities.size() != outInstances.size() ||
        outKeys.size() != outInstances.size() ||
        outBatches.size() != plan.instancedBatchCount)
    {
        outInstances.clear();
        outSemanticIdentities.clear();
        outKeys.clear();
        outBatches.clear();
        return false;
    }
    return true;
}

bool CreateRasterInstanceStream(IRHIDevice& device,
                                std::span<const GPUInstanceData> instances,
                                const char* debugName,
                                RasterInstanceStream& outStream)
{
    std::vector<RasterInstanceStreamKey> positionalKeys;
    try
    {
        positionalKeys.reserve(instances.size());
        for (uint64 index = 0; index < instances.size(); ++index)
        {
            positionalKeys.push_back({index + 1, 0});
        }
    }
    catch (...)
    {
        return false;
    }
    return CreateRasterInstanceStream(device,
                                      instances,
                                      positionalKeys,
                                      debugName,
                                      outStream);
}

bool BuildRasterInstanceData(
    const RenderInstanceBatchPlan& plan,
    const DirectDrawPacketBatch& directBatch,
    const RenderScene& scene,
    std::vector<GPUInstanceData>& outInstances,
    std::vector<RasterInstanceStreamKey>& outKeys,
    std::vector<RasterInstanceStreamBatch>& outBatches,
    const RenderResourceRegistry* resourceRegistry)
{
    std::vector<uint64> ignoredSemanticIdentities;
    return BuildRasterInstanceData(plan,
                                   directBatch,
                                   scene,
                                   outInstances,
                                   ignoredSemanticIdentities,
                                   outKeys,
                                   outBatches,
                                   resourceRegistry);
}

bool BuildRasterInstanceData(
    const RenderInstanceBatchPlan& plan,
    const DirectDrawPacketBatch& directBatch,
    const RenderScene& scene,
    std::vector<GPUInstanceData>& outInstances,
    std::vector<RasterInstanceStreamKey>& outKeys)
{
    std::vector<RasterInstanceStreamBatch> ignoredBatches;
    return BuildRasterInstanceData(plan,
                                   directBatch,
                                   scene,
                                   outInstances,
                                   outKeys,
                                   ignoredBatches);
}

bool BuildRasterInstanceData(
    const RenderInstanceBatchPlan& plan,
    const DirectDrawPacketBatch& directBatch,
    const RenderScene& scene,
    std::vector<GPUInstanceData>& outInstances)
{
    std::vector<RasterInstanceStreamKey> ignoredKeys;
    std::vector<RasterInstanceStreamBatch> ignoredBatches;
    return BuildRasterInstanceData(plan,
                                   directBatch,
                                   scene,
                                   outInstances,
                                   ignoredKeys,
                                   ignoredBatches);
}
} // namespace RVX
