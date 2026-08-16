#pragma once

/**
 * @file RasterInstanceStream.h
 * @brief Shared raster-instance ABI and immutable buffer materialization.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/MeshPassProcessor.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderUploadWorkDiagnostics.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHI.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace RVX
{
    struct RenderInstanceBatchPlan;
    class RenderScene;
    class RenderResourceRegistry;
    /** @brief Raster transform/identity data shared by Direct and GPU-driven paths. */
    struct alignas(16) GPUInstanceData
    {
        Mat4 worldMatrix;
        Mat4 normalMatrix;
        Vec4 boundingSphere;
        Vec4 aabbMin;
        Vec4 aabbMax;
        uint32 meshId;
        uint32 materialId;
        uint32 indexCount;
        uint32 firstIndex;
        int32 vertexOffset;
        uint32 sourceIndex = RVX_INVALID_INDEX;
        uint32 drawGroupIndex;
        uint32 drawGroupVisibleOffset;
        uint32 candidateIndex = RVX_INVALID_INDEX;
        uint32 forceVisible = 0;
        uint32 padding[2] = {};
    };

    static_assert(sizeof(GPUInstanceData) == 224,
                  "GPUInstanceData must match GPUInstanceData.hlsli");
    static_assert(alignof(GPUInstanceData) == 16);
    static_assert(offsetof(GPUInstanceData, worldMatrix) == 0);
    static_assert(offsetof(GPUInstanceData, normalMatrix) == 64);
    static_assert(offsetof(GPUInstanceData, boundingSphere) == 128);
    static_assert(offsetof(GPUInstanceData, aabbMin) == 144);
    static_assert(offsetof(GPUInstanceData, aabbMax) == 160);
    static_assert(offsetof(GPUInstanceData, meshId) == 176);
    static_assert(offsetof(GPUInstanceData, materialId) == 180);
    static_assert(offsetof(GPUInstanceData, indexCount) == 184);
    static_assert(offsetof(GPUInstanceData, firstIndex) == 188);
    static_assert(offsetof(GPUInstanceData, vertexOffset) == 192);
    static_assert(offsetof(GPUInstanceData, sourceIndex) == 196);
    static_assert(offsetof(GPUInstanceData, drawGroupIndex) == 200);
    static_assert(offsetof(GPUInstanceData, drawGroupVisibleOffset) == 204);
    static_assert(offsetof(GPUInstanceData, candidateIndex) == 208);
    static_assert(offsetof(GPUInstanceData, forceVisible) == 212);
    static_assert(offsetof(GPUInstanceData, padding) == 216);

    /** @brief Generation-safe logical identity for one persistent instance row. */
    struct RasterInstanceStreamKey
    {
        uint64 objectId = 0;
        uint32 submeshIndex = 0;

        bool operator==(const RasterInstanceStreamKey&) const = default;
    };

    /** @brief One recorder-local input range belonging to a complete batch key. */
    struct RasterInstanceStreamBatch
    {
        RenderDrawGroupKey key{};
        uint32 inputFirstInstance = 0;
        uint32 instanceCount = 0;
        /** @brief Compatibility-only positional draw ordering. */
        bool preserveInputOrder = false;

        bool operator==(const RasterInstanceStreamBatch&) const = default;
    };

    /** @brief Physical index-segment binding published for the current recorder. */
    struct RasterInstanceStreamBatchBinding
    {
        RenderDrawGroupKey key{};
        uint32 firstInstance = 0;
        uint32 instanceCount = 0;

        bool operator==(const RasterInstanceStreamBatchBinding&) const = default;
    };

    /** @brief Per-entry values used by the order-sensitive transcript digest. */
    struct RasterTranscriptEntryDigest
    {
        bool available = false;
        uint64 identityHash = 0;
        uint64 consumedPayloadHash = 0;
    };

    /**
     * @brief Value-only Direct submission evidence in exact DrawIndexed order.
     * Fixed material keys come from the same resolved binding that will draw;
     * table keys are supplied separately by slot for singleton table draws.
     */
    struct DirectRasterTranscriptPlannedDraw
    {
        DirectDrawPacket packet{};
        uint64 fixedRasterMaterialKey = 0;
        bool usesMaterialParameterTable = false;
        uint32 representedPacketCount = 1;
        bool instanced = false;
    };

    /**
     * @brief Stable group fingerprint without resource-handle generations.
     *
     * This names the raster ABI and draw arguments, while per-entry mesh slots
     * remain part of the consumed payload evidence.
     */
    [[nodiscard]] uint64 GetRasterTranscriptGroupIdentity(
        const RenderDrawGroupKey& key) noexcept;

    [[nodiscard]] RasterTranscriptEntryDigest MakeRasterTranscriptEntryDigest(
        uint64 groupIdentity,
        const RasterInstanceStreamKey& key,
        const GPUInstanceData& instance,
        uint64 semanticIdentity,
        bool materialParameterSlotConsumed,
        uint32 indexCount,
        uint32 firstIndex,
        int32 vertexOffset) noexcept;

    /** @brief Initialize a digest before appending raster entries in draw order. */
    void BeginRasterTranscript(RasterTranscriptDigest& outDigest) noexcept;

    /** @brief Append one actual raster dereference to an initialized digest. */
    void AppendRasterTranscriptEntry(
        RasterTranscriptDigest& digest,
        const RasterTranscriptEntryDigest& entry) noexcept;

    /** @brief Persistent, completion-safe index residency for one full batch key. */
    struct RasterInstanceStreamBatchResidency
    {
        RenderDrawGroupKey key{};
        std::vector<RasterInstanceStreamKey> activeKeys;
        uint32 indexSegmentBase = 0;
        uint32 indexSegmentCapacity = 0;
        uint32 activeCount = 0;

        [[nodiscard]] bool IsValid(uint32 indexCapacity) const noexcept
        {
            return indexSegmentCapacity != 0 &&
                   indexSegmentBase <= indexCapacity &&
                   indexSegmentCapacity <= indexCapacity - indexSegmentBase &&
                   activeCount == activeKeys.size() &&
                   activeCount <= indexSegmentCapacity;
        }
    };

    /** @brief Completion-safe resident data for one physical RHI frame slot. */
    struct RasterInstanceStreamFrameSlot
    {
        RHIBufferRef instances;
        RHIBufferRef instanceIndices;
        std::vector<GPUInstanceData> residentInstances;
        /** CPU-only, resident-row-aligned semantic transcript sidecar. */
        std::vector<uint64> residentRasterSemanticIdentities;
        std::vector<RasterInstanceStreamKey> residentKeys;
        /** @brief Physical index rows, including inactive persistent segments. */
        std::vector<uint32> residentDrawOrder;
        std::vector<RasterInstanceStreamBatchResidency> residentBatches;
        uint32 instanceCount = 0;
        uint32 instanceCapacity = 0;
        uint32 indexCapacity = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return instances && instanceIndices && instanceCount != 0 &&
                   instanceCapacity >= instanceCount &&
                   residentInstances.size() == instanceCapacity &&
                   residentRasterSemanticIdentities.size() == instanceCapacity &&
                   residentKeys.size() == instanceCapacity &&
                   residentDrawOrder.size() == indexCapacity &&
                   instances->GetSize() >=
                       static_cast<uint64>(instanceCapacity) *
                           sizeof(GPUInstanceData) &&
                   instanceIndices->GetSize() >= static_cast<uint64>(indexCapacity) *
                       sizeof(uint32) &&
                   !residentBatches.empty() &&
                   std::all_of(residentBatches.begin(),
                               residentBatches.end(),
                               [this](const RasterInstanceStreamBatchResidency& batch)
                               {
                                   return batch.IsValid(indexCapacity);
                               });
        }
    };

    /**
     * @brief Persistent completion-safe residency for one registered raster pass.
     *
     * A pass owns this cache for its registration lifetime while each graph
     * recorder owns a separate RasterInstanceStream attempt snapshot.  This
     * keeps physical-slot residency stable across fresh graph recorders
     * without allowing one recorder's active aliases or diagnostics to leak
     * into another recording.
     */
    struct RasterInstanceStreamCache
    {
        std::array<RasterInstanceStreamFrameSlot, RVX_MAX_FRAME_COUNT>
            frameSlots{};
        /** @brief Monotonic work committed by this persistent cache. */
        DirectRasterMutationTotals mutationTotals{};
        bool mutationTotalsSaturated = false;
    };

    /**
     * @brief RHI-backed Direct stream with completion-safe physical-slot caches.
     *
     * Active fields alias the slot materialized for the current graph record;
     * upload counters and incremental-work diagnostics describe only that
     * materialization attempt.  They are published only after every mapped
     * write for the attempt has committed successfully.  The frameSlots member
     * is retained solely for the legacy overload below; graph recorders use a
     * separately owned RasterInstanceStreamCache.
     */
    struct RasterInstanceStream
    {
        RHIBufferRef instances;
        RHIBufferRef instanceIndices;
        uint32 instanceCount = 0;
        uint64 instanceUploadBytes = 0;
        uint64 indexUploadBytes = 0;
        /** @brief Successfully committed instance rows for this attempt. */
        uint32 instancePatchedRowCount = 0;
        /** @brief Successfully committed draw-order index rows for this attempt. */
        uint32 indexPatchedRowCount = 0;
        /** @brief Resident logical rows in the active completion-safe slot. */
        uint32 activeInstanceCount = 0;
        /** @brief Allocated logical-row capacity in the active slot. */
        uint32 activeInstanceCapacity = 0;
        /** @brief Allocated physical draw-index capacity in the active slot. */
        uint32 activeIndexCapacity = 0;
        /** @brief This attempt committed a complete instance-row materialization. */
        bool instanceFullMaterialization = false;
        /** @brief This attempt committed a complete draw-order materialization. */
        bool indexFullMaterialization = false;
        RenderUploadWorkDiagnostics instanceUploadWork{};
        RenderUploadWorkDiagnostics indexUploadWork{};
        /** @brief Recorder-local physical segment bindings for instanced batches. */
        std::vector<RasterInstanceStreamBatchBinding> batchBindings;
        uint32 activeFrameSlot = RVX_INVALID_INDEX;
        std::array<RasterInstanceStreamFrameSlot, RVX_MAX_FRAME_COUNT>
            frameSlots{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return activeFrameSlot < RVX_MAX_FRAME_COUNT && instances &&
                   instanceIndices && instanceCount != 0 &&
                   activeInstanceCapacity >= instanceCount &&
                   activeIndexCapacity != 0 && !batchBindings.empty() &&
                   instances->GetSize() >=
                       static_cast<uint64>(activeInstanceCapacity) *
                           sizeof(GPUInstanceData) &&
                   instanceIndices->GetSize() >=
                       static_cast<uint64>(activeIndexCapacity) * sizeof(uint32) &&
                   std::all_of(batchBindings.begin(),
                               batchBindings.end(),
                               [this](const RasterInstanceStreamBatchBinding& binding)
                               {
                                   return binding.instanceCount != 0 &&
                                       binding.firstInstance <= activeIndexCapacity &&
                                       binding.instanceCount <=
                                           activeIndexCapacity - binding.firstInstance;
                               });
        }
    };

    /**
     * @brief Build Direct's complete ordered raster transcript without RHI work.
     * Missing residency or semantic evidence makes @p outDigest unavailable and
     * leaves all render state unchanged.
     */
    [[nodiscard]] bool BuildCompleteDirectRasterTranscript(
        std::span<const DirectRasterTranscriptPlannedDraw> plannedDraws,
        const RenderScene& scene,
        const RenderResourceRegistry& resourceRegistry,
        const RasterInstanceStream* stream,
        const RasterInstanceStreamCache* cache,
        std::span<const uint64> rasterMaterialSemanticKeysBySlot,
        RasterTranscriptDigest& outDigest) noexcept;

    /**
     * @brief Reconstruct Direct's submitted instance-index dereference order.
     *
     * The cache is required because an external cache owns the resident frame
     * slots used by graph recorders.  No source input ordering is consulted.
     */
    [[nodiscard]] bool BuildRasterInstanceStreamTranscript(
        const RasterInstanceStream& stream,
        const RasterInstanceStreamCache& cache,
        RasterTranscriptDigest& outDigest) noexcept;

    /**
     * @brief Transactionally publish CPU-only post-binding transcript evidence.
     *
     * The values are resident-row-aligned and never map, upload, or mutate
     * the GPU stream. On validation failure the current sidecar is retained.
     */
    [[nodiscard]] bool FinalizeRasterInstanceStreamSemanticSidecar(
        const RasterInstanceStream& stream,
        RasterInstanceStreamCache& cache,
        std::vector<uint64>&& semanticIdentitiesByResidentRow) noexcept;

    /**
     * @brief Materialize one completion-safe, identity-indexed raster stream.
     *
     * Static content in a warmed physical slot performs no Map/copy. Row data
     * is keyed by object/submesh identity, so transform changes update only
     * dirty rows and draw churn reuses free logical rows while rewriting only
     * changed draw-order indices. A failed Map or write commit never advances
     * CPU metadata; a partial multi-buffer commit fail-closes that slot rather
     * than exposing incompatible old aliases as a current generation.
     */
    /**
     * @brief Materialize an attempt snapshot against externally owned residency.
     *
     * The cache must outlive all graph recorders that may reference its
     * imported buffers.  The output stream never owns or mutates active state
     * from a previous recorder.
     */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        std::span<const RasterInstanceStreamKey> keys,
        std::span<const RasterInstanceStreamBatch> batches,
        const char* debugName,
        RasterInstanceStream& outStream,
        std::span<const uint64> semanticIdentities = {});

    /** @brief Materialize keyed input with externally owned group residency. */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        std::span<const RasterInstanceStreamKey> keys,
        std::span<const RasterInstanceStreamBatch> batches,
        const char* debugName,
        RasterInstanceStreamCache& cache,
        RasterInstanceStream& outStream,
        std::span<const uint64> semanticIdentities = {});

    /** @brief Materialize one implicit compatibility batch into external residency. */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        std::span<const RasterInstanceStreamKey> keys,
        const char* debugName,
        RasterInstanceStreamCache& cache,
        RasterInstanceStream& outStream,
        std::span<const uint64> semanticIdentities = {});

    /** @brief Materialize one implicit compatibility batch for keyed callers. */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        std::span<const RasterInstanceStreamKey> keys,
        const char* debugName,
        RasterInstanceStream& outStream,
        std::span<const uint64> semanticIdentities = {});

    /** @brief Legacy positional overload for callers without canonical keys. */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        const char* debugName,
        RasterInstanceStream& outStream);

    /** @brief Resolve one keyed CPU instance array from a sealed batch plan. */
    [[nodiscard]] bool BuildRasterInstanceData(
        const RenderInstanceBatchPlan& plan,
        const DirectDrawPacketBatch& directBatch,
        const RenderScene& scene,
        std::vector<GPUInstanceData>& outInstances,
        std::vector<uint64>& outSemanticIdentities,
        std::vector<RasterInstanceStreamKey>& outKeys,
        std::vector<RasterInstanceStreamBatch>& outBatches,
        const RenderResourceRegistry* resourceRegistry = nullptr);

    /** @brief Compatibility overload that discards diagnostic semantic identities. */
    [[nodiscard]] bool BuildRasterInstanceData(
        const RenderInstanceBatchPlan& plan,
        const DirectDrawPacketBatch& directBatch,
        const RenderScene& scene,
        std::vector<GPUInstanceData>& outInstances,
        std::vector<RasterInstanceStreamKey>& outKeys,
        std::vector<RasterInstanceStreamBatch>& outBatches,
        const RenderResourceRegistry* resourceRegistry = nullptr);

    /** @brief Compatibility overload that discards persistent batch ranges. */
    [[nodiscard]] bool BuildRasterInstanceData(
        const RenderInstanceBatchPlan& plan,
        const DirectDrawPacketBatch& directBatch,
        const RenderScene& scene,
        std::vector<GPUInstanceData>& outInstances,
        std::vector<RasterInstanceStreamKey>& outKeys);

    /** @brief Compatibility overload that discards canonical row keys. */
    [[nodiscard]] bool BuildRasterInstanceData(
        const RenderInstanceBatchPlan& plan,
        const DirectDrawPacketBatch& directBatch,
        const RenderScene& scene,
        std::vector<GPUInstanceData>& outInstances);

    /** @brief Shared identity stream creator used by GPUCulling frame owners. */
    [[nodiscard]] RHIBufferRef CreateRasterInstanceIndexBuffer(
        IRHIDevice& device,
        uint32 instanceCapacity,
        const char* debugName);
} // namespace RVX
