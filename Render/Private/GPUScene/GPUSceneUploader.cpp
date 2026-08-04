/**
 * @file GPUSceneUploader.cpp
 * @brief Persistent RenderGraph-copy uploader for the non-executing GPU scene.
 */

#include "GPUScene/GPUSceneUploader.h"

#include "Render/Graph/RenderGraph.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    enum class GPUSceneUploadTable : uint8
    {
        Primitives = 0,
        Bounds,
        Transforms,
        Materials,
        Geometries,
        Draws,
        Count,
    };

    constexpr uint32 GPU_SCENE_UPLOAD_TABLE_COUNT =
        static_cast<uint32>(GPUSceneUploadTable::Count);

    static_assert(GPU_SCENE_UPLOAD_TABLE_COUNT ==
                  GPU_SCENE_RESIDENT_TABLE_COUNT);

    struct TableSource
    {
        const void* rows = nullptr;
        uint32 rowCount = 0;
        uint32 stride = 0;
        const char* debugName = nullptr;
        bool rowCountValid = false;
    };

    template <typename T>
    TableSource MakeTableSource(const std::vector<T>& rows, const char* debugName)
    {
        if (rows.size() > std::numeric_limits<uint32>::max())
        {
            return {nullptr, 0, static_cast<uint32>(sizeof(T)), debugName, false};
        }
        return {rows.empty() ? nullptr : rows.data(),
                static_cast<uint32>(rows.size()),
                static_cast<uint32>(sizeof(T)),
                debugName,
                true};
    }

    TableSource GetTableSource(
        const GPUSceneCommittedMirror& mirror,
        GPUSceneUploadTable table)
    {
        switch (table)
        {
            case GPUSceneUploadTable::Primitives:
                return MakeTableSource(mirror.primitives, "GPUScene.Primitives");
            case GPUSceneUploadTable::Bounds:
                return MakeTableSource(mirror.bounds, "GPUScene.Bounds");
            case GPUSceneUploadTable::Transforms:
                return MakeTableSource(mirror.transforms, "GPUScene.Transforms");
            case GPUSceneUploadTable::Materials:
                return MakeTableSource(mirror.materials, "GPUScene.Materials");
            case GPUSceneUploadTable::Geometries:
                return MakeTableSource(mirror.geometries, "GPUScene.Geometries");
            case GPUSceneUploadTable::Draws:
                return MakeTableSource(mirror.draws, "GPUScene.Draws");
            case GPUSceneUploadTable::Count:
            default:
                return {};
        }
    }

    bool HasValidTableSources(const GPUSceneCommittedMirror& mirror) noexcept
    {
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            if (!GetTableSource(
                    mirror,
                    static_cast<GPUSceneUploadTable>(tableIndex)).rowCountValid)
            {
                return false;
            }
        }
        return true;
    }

    bool IsTrackerIssuedCompletionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token) noexcept
    {
        const RHIQueueTopology& topology = tracker.GetTopology();
        for (uint8 pointIndex = 0; pointIndex < token.count; ++pointIndex)
        {
            const GPUCompletionPoint point = token.points[pointIndex];
            const bool activeDomain = std::find(
                topology.logicalQueueDomains.begin(),
                topology.logicalQueueDomains.end(),
                point.domain) != topology.logicalQueueDomains.end();
            if (!activeDomain || point.value == 0 ||
                point.value > tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
        }
        return true;
    }

    const GPUSceneTableChangeSet& GetTableChangeSet(
        const GPUSceneChangeSet& changes,
        GPUSceneUploadTable table)
    {
        switch (table)
        {
            case GPUSceneUploadTable::Primitives: return changes.primitives;
            case GPUSceneUploadTable::Bounds: return changes.bounds;
            case GPUSceneUploadTable::Transforms: return changes.transforms;
            case GPUSceneUploadTable::Materials: return changes.materials;
            case GPUSceneUploadTable::Geometries: return changes.geometries;
            case GPUSceneUploadTable::Draws: return changes.draws;
            case GPUSceneUploadTable::Count:
            default: return changes.primitives;
        }
    }

    bool HasDirtyWork(const GPUSceneTableChangeSet& changes) noexcept
    {
        return changes.fullTableDirty || !changes.dirtyRanges.empty();
    }

    void CoalesceRanges(std::vector<GPUSceneDirtyRowRange>& ranges) noexcept
    {
        if (ranges.size() < 2)
        {
            return;
        }

        std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs)
        {
            return lhs.firstRow < rhs.firstRow;
        });
        size_t output = 0;
        for (const GPUSceneDirtyRowRange range : ranges)
        {
            if (range.rowCount == 0)
            {
                continue;
            }
            if (output == 0)
            {
                ranges[output++] = range;
                continue;
            }

            GPUSceneDirtyRowRange& previous = ranges[output - 1U];
            const uint64 previousEnd =
                static_cast<uint64>(previous.firstRow) + previous.rowCount;
            const uint64 rangeEnd =
                static_cast<uint64>(range.firstRow) + range.rowCount;
            if (range.firstRow <= previousEnd)
            {
                previous.rowCount = static_cast<uint32>(
                    std::min<uint64>(std::numeric_limits<uint32>::max(),
                                     std::max(previousEnd, rangeEnd) - previous.firstRow));
            }
            else
            {
                ranges[output++] = range;
            }
        }
        ranges.resize(output);
    }

    uint32 GrowCapacity(uint32 required) noexcept
    {
        uint32 capacity = 1;
        while (capacity < required)
        {
            if (capacity > std::numeric_limits<uint32>::max() / 2U)
            {
                return required;
            }
            capacity *= 2U;
        }
        return capacity;
    }

} // namespace

class GPUSceneUploader::Impl
{
public:
    struct TableState
    {
        RHIBufferRef buffer;
        RHIBufferAccessSnapshot access = MakeRHIBufferAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);
        uint32 capacity = 0;
        uint32 stride = 0;
        bool fullDirty = true;
        std::vector<GPUSceneDirtyRowRange> dirtyRanges;
    };

    struct BufferSet
    {
        std::array<TableState, GPU_SCENE_UPLOAD_TABLE_COUNT> tables;
        /** Last committed CPU delta generation represented by dirty state. */
        uint64 coveredVersion = 0;
        /** Latest CPU mirror version this set is intended to reach. */
        uint64 desiredVersion = 0;
        /** Actual version whose bytes were submitted to the GPU. */
        uint64 residentVersion = 0;
        GPUCompletionToken lastUse;
        bool hasLastUse = false;
        bool frameReadUse = false;
        bool frameReadAccessCommitted = false;
        std::array<RGBufferHandle, GPU_SCENE_UPLOAD_TABLE_COUNT> frameReadHandles;
        std::array<RHIBufferAccessSnapshot, GPU_SCENE_UPLOAD_TABLE_COUNT>
            frameReadPreviousAccess;
        bool unusable = false;
    };

    struct CopyRange
    {
        uint64 sourceOffset = 0;
        uint64 destinationOffset = 0;
        uint64 size = 0;
    };

    struct TableUploadPlan
    {
        uint32 tableIndex = 0;
        RHIBufferRef target;
        RHIBufferRef staging;
        RGBufferHandle targetHandle;
        RHIBufferAccessSnapshot previousAccess;
        std::vector<GPUSceneDirtyRowRange> consumedRanges;
        std::vector<CopyRange> copies;
        bool consumedFull = false;
    };

    struct PendingUpload
    {
        uint32 setIndex = RVX_INVALID_INDEX;
        uint64 version = 0;
        std::vector<TableUploadPlan> tables;
        bool graphExecuted = false;
        bool fullUpload = false;
    };

    IRHIDevice* device = nullptr;
    RenderSubmissionTracker* tracker = nullptr;
    const GPUSceneCommittedMirror* observedMirror = nullptr;
    uint64 observedVersion = 0;
    uint64 reclaimedThrough = 0;
    std::vector<BufferSet> sets;
    std::optional<PendingUpload> pending;
    bool initialized = false;
    bool deviceLost = false;
};

GPUSceneUploader::GPUSceneUploader()
    : m_impl(std::make_unique<Impl>())
{
}

GPUSceneUploader::~GPUSceneUploader()
{
    Shutdown();
}

bool GPUSceneUploader::Initialize(
    IRHIDevice* device,
    RenderSubmissionTracker* submissionTracker) noexcept
{
    if (!m_impl || !device || !submissionTracker)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::NotInitialized;
        return false;
    }

    m_impl->device = device;
    m_impl->tracker = submissionTracker;
    m_impl->initialized = true;
    m_diagnostics = {};
    return true;
}

void GPUSceneUploader::Shutdown() noexcept
{
    if (!m_impl)
    {
        return;
    }

    m_impl->pending.reset();
    m_impl->sets.clear();
    m_impl->observedMirror = nullptr;
    m_impl->device = nullptr;
    m_impl->tracker = nullptr;
    m_impl->initialized = false;
    m_diagnostics = {};
}

void GPUSceneUploader::Observe(
    const GPUSceneCommittedMirror& mirror,
    const GPUSceneChangeSet& changes) noexcept
{
    if (!m_impl || !m_impl->initialized || m_impl->deviceLost)
    {
        return;
    }

    m_impl->observedMirror = &mirror;
    m_diagnostics.observedVersion = mirror.version;
    if (mirror.version == m_impl->observedVersion)
    {
        return;
    }

    const bool continuous = changes.committedVersion == mirror.version &&
                            changes.baseVersion == m_impl->observedVersion;
    if (!continuous)
    {
        m_diagnostics.continuityLost = true;
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::ContinuityLost;
        for (Impl::BufferSet& set : m_impl->sets)
        {
            set.coveredVersion = mirror.version;
            set.desiredVersion = mirror.version;
            for (Impl::TableState& table : set.tables)
            {
                table.fullDirty = true;
                table.dirtyRanges.clear();
            }
        }
    }
    else
    {
        try
        {
            for (Impl::BufferSet& set : m_impl->sets)
            {
                const bool setContinuous =
                    set.coveredVersion == changes.baseVersion;
                set.desiredVersion = mirror.version;
                for (uint32 tableIndex = 0;
                     tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
                     ++tableIndex)
                {
                    Impl::TableState& table = set.tables[tableIndex];
                    const GPUSceneTableChangeSet& delta = GetTableChangeSet(
                        changes,
                        static_cast<GPUSceneUploadTable>(tableIndex));
                    if (!setContinuous)
                    {
                        table.fullDirty = true;
                        table.dirtyRanges.clear();
                        continue;
                    }
                    if (table.fullDirty || !HasDirtyWork(delta))
                    {
                        continue;
                    }
                    if (delta.fullTableDirty)
                    {
                        table.fullDirty = true;
                        table.dirtyRanges.clear();
                        continue;
                    }
                    table.dirtyRanges.insert(table.dirtyRanges.end(),
                                             delta.dirtyRanges.begin(),
                                             delta.dirtyRanges.end());
                    CoalesceRanges(table.dirtyRanges);
                }
                set.coveredVersion = mirror.version;
            }
        }
        catch (const std::bad_alloc&)
        {
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
            for (Impl::BufferSet& set : m_impl->sets)
            {
                set.coveredVersion = mirror.version;
                set.desiredVersion = mirror.version;
                for (Impl::TableState& table : set.tables)
                {
                    table.fullDirty = true;
                    table.dirtyRanges.clear();
                }
            }
        }
        catch (...)
        {
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
            return;
        }
    }

    m_impl->observedVersion = mirror.version;
}

void GPUSceneUploader::BuildRenderGraph(
    RenderGraph& graph,
    RenderSubmissionResourceBatch* submissionBatch) noexcept
{
    if (!m_impl)
    {
        return;
    }

    // Frame counters describe only this recording attempt, including early
    // returns caused by a still-pending upload or a clean resident set.
    m_diagnostics.frameUploadBytes = 0;
    m_diagnostics.frameUploadRangeCount = 0;
    m_diagnostics.fullUpload = false;
    m_diagnostics.rollbackPending = false;
    if (!m_impl->initialized || !m_impl->observedMirror)
    {
        return;
    }

    // A warm static set is only a no-op while the device itself remains
    // usable. Check this before the clean fast path so device loss is never
    // misreported as a successful zero-copy frame.
    if (m_impl->deviceLost || !m_impl->device ||
        m_impl->device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
    {
        m_impl->deviceLost = true;
        m_diagnostics.deviceLost = true;
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::DeviceLost;
        return;
    }
    if (m_impl->pending)
    {
        return;
    }

    const GPUSceneCommittedMirror& mirror = *m_impl->observedMirror;
    if (!HasValidTableSources(mirror))
    {
        // The public GPU-scene row index is uint32. Refuse a mirror that
        // cannot be represented by the upload ABI instead of truncating it.
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return;
    }
    const auto satisfiesCapacity = [&mirror](const Impl::BufferSet& set)
    {
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            if (set.tables[tableIndex].capacity <
                GetTableSource(mirror, static_cast<GPUSceneUploadTable>(tableIndex)).rowCount)
            {
                return false;
            }
        }
        return true;
    };

    // A static resident set is a read-only no-op. Do this before querying
    // completion, so an in-flight read cannot turn a warm frame into a copy.
    for (const Impl::BufferSet& set : m_impl->sets)
    {
        if (!satisfiesCapacity(set) ||
            set.residentVersion != mirror.version ||
            set.coveredVersion != mirror.version ||
            set.desiredVersion != mirror.version)
        {
            continue;
        }
        bool cleanCurrent = true;
        for (const Impl::TableState& table : set.tables)
        {
            cleanCurrent &= !table.fullDirty && table.dirtyRanges.empty();
        }
        if (cleanCurrent)
        {
            m_diagnostics.residentVersion = set.residentVersion;
            return;
        }
    }
    static_cast<void>(PollSafeReclaimVersion());
    if (m_impl->deviceLost)
    {
        return;
    }

    const auto safeForWrite = [this](Impl::BufferSet& set)
    {
        if (set.unusable)
        {
            return false;
        }
        if (!set.hasLastUse)
        {
            return true;
        }
        const GPUCompletionStatus status = m_impl->tracker->Query(set.lastUse);
        if (status == GPUCompletionStatus::Lost)
        {
            set.unusable = true;
            m_impl->deviceLost = true;
            m_diagnostics.deviceLost = true;
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::DeviceLost;
            return false;
        }
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    };

    uint32 selectedSet = RVX_INVALID_INDEX;
    for (uint32 index = 0; index < m_impl->sets.size(); ++index)
    {
        Impl::BufferSet& set = m_impl->sets[index];
        if (!satisfiesCapacity(set) || !safeForWrite(set))
        {
            continue;
        }
        if (selectedSet == RVX_INVALID_INDEX)
        {
            selectedSet = index;
        }
    }
    if (m_impl->deviceLost)
    {
        return;
    }

    if (selectedSet == RVX_INVALID_INDEX)
    {
        try
        {
            Impl::BufferSet set;
            set.coveredVersion = mirror.version;
            set.desiredVersion = mirror.version;
            for (uint32 tableIndex = 0;
                 tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
                 ++tableIndex)
            {
                Impl::TableState& table = set.tables[tableIndex];
                const TableSource source = GetTableSource(
                    mirror, static_cast<GPUSceneUploadTable>(tableIndex));
                table.capacity = GrowCapacity(std::max(1U, source.rowCount));
                table.stride = source.stride;
                RHIBufferDesc desc;
                desc.size = static_cast<uint64>(table.capacity) * source.stride;
                desc.usage = RHIBufferUsage::Structured |
                             RHIBufferUsage::ShaderResource |
                             RHIBufferUsage::CopyDst;
                desc.memoryType = RHIMemoryType::Default;
                desc.stride = source.stride;
                desc.debugName = source.debugName;
                table.buffer = m_impl->device->CreateBuffer(desc);
                if (!table.buffer)
                {
                    m_diagnostics.failureReason =
                        GPUSceneUploadFailureReason::BufferCreationFailed;
                    return;
                }
                // Every newly allocated set is fully initialized; dirty-only
                // writes into an uninitialized set are never permitted.
                table.fullDirty = true;
            }
            m_impl->sets.push_back(std::move(set));
            selectedSet = static_cast<uint32>(m_impl->sets.size() - 1U);
        }
        catch (const std::bad_alloc&)
        {
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::BufferCreationFailed;
            return;
        }
        catch (...)
        {
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
            return;
        }
    }

    Impl::BufferSet& set = m_impl->sets[selectedSet];
    Impl::PendingUpload pending;
    pending.setIndex = selectedSet;
    pending.version = mirror.version;
    try
    {
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            Impl::TableState& table = set.tables[tableIndex];
            const TableSource source = GetTableSource(
                mirror, static_cast<GPUSceneUploadTable>(tableIndex));
            std::vector<GPUSceneDirtyRowRange> ranges;
            const bool fullUpload = table.fullDirty;
            if (fullUpload)
            {
                // Initialize the whole allocated capacity, not merely the live
                // rows. This makes the exported ShaderResource contents valid
                // while retaining the schema sentinel at row zero.
                ranges.push_back({0, table.capacity});
            }
            else
            {
                ranges = table.dirtyRanges;
                ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                            [source](const GPUSceneDirtyRowRange& range)
                {
                    return range.rowCount == 0 || range.firstRow >= source.rowCount;
                }), ranges.end());
                for (GPUSceneDirtyRowRange& range : ranges)
                {
                    range.rowCount = std::min(range.rowCount, source.rowCount - range.firstRow);
                }
            }
            ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                        [](const GPUSceneDirtyRowRange& range)
            {
                return range.rowCount == 0;
            }), ranges.end());
            if (ranges.empty())
            {
                continue;
            }

            uint64 stagingSize = 0;
            for (const GPUSceneDirtyRowRange range : ranges)
            {
                stagingSize += static_cast<uint64>(range.rowCount) * source.stride;
            }
            RHIBufferDesc stagingDesc;
            stagingDesc.size = stagingSize;
            stagingDesc.usage = RHIBufferUsage::CopySrc;
            stagingDesc.memoryType = RHIMemoryType::Upload;
            stagingDesc.debugName = "GPUScene.UploadStaging";
            RHIBufferRef staging = m_impl->device->CreateBuffer(stagingDesc);
            if (!staging)
            {
                m_diagnostics.failureReason = GPUSceneUploadFailureReason::StagingCreationFailed;
                return;
            }
            void* mapped = staging->Map();
            if (!mapped)
            {
                m_diagnostics.failureReason = GPUSceneUploadFailureReason::StagingMapFailed;
                return;
            }
            std::memset(mapped, 0, static_cast<size_t>(stagingSize));

            Impl::TableUploadPlan plan;
            plan.tableIndex = tableIndex;
            plan.target = table.buffer;
            plan.staging = staging;
            plan.previousAccess = table.access;
            plan.consumedFull = fullUpload;
            plan.consumedRanges = ranges;
            uint64 stagingOffset = 0;
            for (const GPUSceneDirtyRowRange range : ranges)
            {
                const uint64 bytes = static_cast<uint64>(range.rowCount) * source.stride;
                if (source.rows != nullptr && range.firstRow < source.rowCount)
                {
                    const uint32 sourceRows = std::min(
                        range.rowCount, source.rowCount - range.firstRow);
                    std::memcpy(static_cast<uint8*>(mapped) + stagingOffset,
                                static_cast<const uint8*>(source.rows) +
                                    static_cast<uint64>(range.firstRow) * source.stride,
                                static_cast<size_t>(sourceRows) * source.stride);
                }
                plan.copies.push_back({stagingOffset,
                                       static_cast<uint64>(range.firstRow) * source.stride,
                                       bytes});
                stagingOffset += bytes;
                m_diagnostics.frameUploadBytes += bytes;
                ++m_diagnostics.frameUploadRangeCount;
            }
            staging->Unmap();

            const uint64 persistentBufferBytes =
                static_cast<uint64>(table.capacity) * table.stride;
            if (!RetainRenderSubmissionResource(submissionBatch, table.buffer,
                                                persistentBufferBytes) ||
                !RetainRenderSubmissionResource(submissionBatch, staging,
                                                stagingSize))
            {
                m_diagnostics.failureReason =
                    GPUSceneUploadFailureReason::SubmissionRetentionFailed;
                return;
            }
            pending.fullUpload |= fullUpload;
            pending.tables.push_back(std::move(plan));
        }
    }
    catch (const std::bad_alloc&)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::StagingCreationFailed;
        return;
    }
    catch (...)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return;
    }

    if (pending.tables.empty())
    {
        m_diagnostics.residentVersion = set.residentVersion;
        return;
    }

    try
    {
        struct UploadPassData
        {
            std::vector<Impl::TableUploadPlan> tables;
            std::vector<RGBufferHandle> stagingHandles;
        };

        std::vector<RGBufferHandle> stagingHandles;
        stagingHandles.reserve(pending.tables.size());
        for (Impl::TableUploadPlan& plan : pending.tables)
        {
            Impl::TableState& table = set.tables[plan.tableIndex];
            plan.targetHandle = graph.ImportBuffer(table.buffer.Get(), table.access);
            stagingHandles.push_back(graph.ImportBuffer(
                plan.staging.Get(),
                MakeRHIBufferAccessSnapshot(
                    RHIResourceState::CopySource,
                    RHIShaderStage::None,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Valid)));
        }

        graph.AddPass<UploadPassData>(
            "GPUScene.Upload",
            RenderGraphPassType::Copy,
            [plans = pending.tables, stagingHandles](
                RenderGraphBuilder& builder,
                UploadPassData& data)
            {
                data.tables = plans;
                data.stagingHandles = stagingHandles;
                for (uint32 index = 0; index < data.tables.size(); ++index)
                {
                    const Impl::TableUploadPlan& plan = data.tables[index];
                    for (const Impl::CopyRange& copy : plan.copies)
                    {
                        static_cast<void>(builder.Read(
                            data.stagingHandles[index].Range(copy.sourceOffset, copy.size),
                            RHIResourceState::CopySource,
                            RHIShaderStage::None));
                        static_cast<void>(builder.Write(
                            plan.targetHandle.Range(copy.destinationOffset, copy.size),
                            RHIResourceState::CopyDest));
                    }
                }
            },
            [](const UploadPassData& data, RHICommandContext& context)
            {
                for (const Impl::TableUploadPlan& plan : data.tables)
                {
                    for (const Impl::CopyRange& copy : plan.copies)
                    {
                        context.CopyBuffer(plan.staging.Get(), plan.target.Get(),
                                           copy.sourceOffset,
                                           copy.destinationOffset,
                                           copy.size);
                    }
                }
            });
        for (const Impl::TableUploadPlan& plan : pending.tables)
        {
            graph.SetExportAccess(
                plan.targetHandle,
                MakeRHIAccessSnapshot(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::All,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Valid));
        }
    }
    catch (const std::bad_alloc&)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return;
    }
    catch (...)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return;
    }

    m_diagnostics.fullUpload = pending.fullUpload;
    // The plan now owns an immutable dirty snapshot. Later Observe calls may
    // append newer deltas directly to the set while this upload is pending.
    for (const Impl::TableUploadPlan& plan : pending.tables)
    {
        Impl::TableState& table = set.tables[plan.tableIndex];
        table.fullDirty = false;
        table.dirtyRanges.clear();
    }
    m_impl->pending = std::move(pending);
}

std::optional<GPUSceneResidentGraphLease>
GPUSceneUploader::AcquireCurrentGraphLease(
    RenderGraph& graph,
    RenderSubmissionResourceBatch* submissionBatch) noexcept
{
    if (!m_impl || !m_impl->initialized || m_impl->deviceLost ||
        m_impl->pending || !m_impl->observedMirror ||
        m_impl->observedVersion == 0 ||
        m_impl->observedMirror->version != m_impl->observedVersion)
    {
        return std::nullopt;
    }
    if (!m_impl->device ||
        m_impl->device->QueryRuntimeStatus() != RHIDeviceRuntimeStatus::Ready)
    {
        m_impl->deviceLost = true;
        m_diagnostics.deviceLost = true;
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::DeviceLost;
        return std::nullopt;
    }

    const GPUSceneCommittedMirror& mirror = *m_impl->observedMirror;
    if (!HasValidTableSources(mirror))
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return std::nullopt;
    }

    for (const Impl::BufferSet& set : m_impl->sets)
    {
        // A graph recording may consume one concrete uploader set exactly
        // once. Do not satisfy a second request from a different same-version
        // set while the first submission has not resolved its access snapshot.
        if (set.frameReadUse)
        {
            return std::nullopt;
        }
    }

    uint32 selectedSetIndex = RVX_INVALID_INDEX;
    for (uint32 setIndex = 0; setIndex < m_impl->sets.size(); ++setIndex)
    {
        const Impl::BufferSet& set = m_impl->sets[setIndex];
        if (set.unusable || set.frameReadUse ||
            set.residentVersion != m_impl->observedVersion ||
            set.coveredVersion != m_impl->observedVersion ||
            set.desiredVersion != m_impl->observedVersion)
        {
            continue;
        }

        bool complete = true;
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            const Impl::TableState& table = set.tables[tableIndex];
            const TableSource source = GetTableSource(
                mirror, static_cast<GPUSceneUploadTable>(tableIndex));
            complete &= table.buffer && table.capacity >= source.rowCount &&
                        table.stride == source.stride && !table.fullDirty &&
                        table.dirtyRanges.empty();
        }
        if (complete)
        {
            selectedSetIndex = setIndex;
            break;
        }
    }

    if (selectedSetIndex == RVX_INVALID_INDEX)
    {
        return std::nullopt;
    }

    Impl::BufferSet& set = m_impl->sets[selectedSetIndex];
    GPUSceneResidentGraphLease lease;
    lease.version = m_impl->observedVersion;
    lease.m_bufferSetIndex = selectedSetIndex;
    try
    {
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            const Impl::TableState& table = set.tables[tableIndex];
            const uint64 bytes = static_cast<uint64>(table.capacity) * table.stride;
            if (!RetainRenderSubmissionResource(
                    submissionBatch, table.buffer, bytes))
            {
                m_diagnostics.failureReason =
                    GPUSceneUploadFailureReason::SubmissionRetentionFailed;
                return std::nullopt;
            }
            lease.buffers[tableIndex] = table.buffer;
            lease.capacities[tableIndex] = table.capacity;
        }

        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            lease.handles[tableIndex] = graph.ImportBuffer(
                lease.buffers[tableIndex].Get(), set.tables[tableIndex].access);
        }
    }
    catch (const std::bad_alloc&)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return std::nullopt;
    }
    catch (...)
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return std::nullopt;
    }

    if (!lease.IsValid())
    {
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::UnexpectedFailure;
        return std::nullopt;
    }

    // One lease maps to one concrete set. Store the imported handles and the
    // pre-recording snapshots so Commit/Release can be symmetric even when the
    // graph executes but the enclosing submission is abandoned.
    set.frameReadPreviousAccess = {};
    for (uint32 tableIndex = 0;
         tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
         ++tableIndex)
    {
        set.frameReadHandles[tableIndex] = lease.handles[tableIndex];
        set.frameReadPreviousAccess[tableIndex] = set.tables[tableIndex].access;
    }
    set.frameReadAccessCommitted = false;
    set.frameReadUse = true;
    return lease;
}

bool GPUSceneUploader::CancelCurrentGraphLease() noexcept
{
    if (!m_impl || !m_impl->initialized || m_impl->deviceLost || m_impl->pending)
    {
        return false;
    }

    Impl::BufferSet* leasedSet = nullptr;
    for (Impl::BufferSet& set : m_impl->sets)
    {
        if (!set.frameReadUse)
        {
            continue;
        }
        if (leasedSet != nullptr || set.frameReadAccessCommitted)
        {
            return false;
        }
        leasedSet = &set;
    }

    if (leasedSet == nullptr)
    {
        return false;
    }

    for (uint32 tableIndex = 0;
         tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
         ++tableIndex)
    {
        leasedSet->tables[tableIndex].access =
            leasedSet->frameReadPreviousAccess[tableIndex];
    }
    leasedSet->frameReadUse = false;
    leasedSet->frameReadAccessCommitted = false;
    leasedSet->frameReadHandles = {};
    leasedSet->frameReadPreviousAccess = {};
    return true;
}

void GPUSceneUploader::CommitRealizedAccess(const RenderGraph& graph) noexcept
{
    if (!m_impl)
    {
        return;
    }

    if (m_impl->pending && m_impl->pending->setIndex < m_impl->sets.size())
    {
        Impl::PendingUpload& pending = *m_impl->pending;
        Impl::BufferSet& set = m_impl->sets[pending.setIndex];
        for (const Impl::TableUploadPlan& plan : pending.tables)
        {
            set.tables[plan.tableIndex].access = graph.GetRealizedAccess(plan.targetHandle);
        }
        pending.graphExecuted = true;
    }

    for (Impl::BufferSet& set : m_impl->sets)
    {
        if (!set.frameReadUse)
        {
            continue;
        }
        for (uint32 tableIndex = 0;
             tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
             ++tableIndex)
        {
            if (!set.frameReadHandles[tableIndex].IsValid())
            {
                continue;
            }
            set.tables[tableIndex].access = graph.GetRealizedAccess(
                set.frameReadHandles[tableIndex]);
        }
        set.frameReadAccessCommitted = true;
    }
}

void GPUSceneUploader::NotifySubmission(const GPUCompletionToken& completion) noexcept
{
    if (!m_impl)
    {
        return;
    }

    bool hasFrameReadUse = false;
    for (const Impl::BufferSet& set : m_impl->sets)
    {
        hasFrameReadUse |= set.frameReadUse;
    }
    if (!m_impl->pending && !hasFrameReadUse)
    {
        return;
    }

    GPUCompletionToken normalized;
    const bool validToken = m_impl->tracker &&
                            MergeGPUCompletionToken(normalized, completion) &&
                            normalized.count != 0 &&
                            IsTrackerIssuedCompletionToken(*m_impl->tracker, normalized);
    const bool validPending = !m_impl->pending ||
        (m_impl->pending->graphExecuted &&
         m_impl->pending->setIndex < m_impl->sets.size());
    bool validReadAccess = true;
    for (const Impl::BufferSet& set : m_impl->sets)
    {
        validReadAccess &= !set.frameReadUse || set.frameReadAccessCommitted;
    }
    if (!validToken || !validPending || !validReadAccess)
    {
        if (m_impl->pending &&
            m_impl->pending->setIndex < m_impl->sets.size())
        {
            Impl::BufferSet& set = m_impl->sets[m_impl->pending->setIndex];
            for (const Impl::TableUploadPlan& plan : m_impl->pending->tables)
            {
                set.tables[plan.tableIndex].access = plan.previousAccess;
                if (plan.consumedFull)
                {
                    set.tables[plan.tableIndex].fullDirty = true;
                    set.tables[plan.tableIndex].dirtyRanges.clear();
                }
                else if (!set.tables[plan.tableIndex].fullDirty)
                {
                    std::vector<GPUSceneDirtyRowRange>& ranges =
                        set.tables[plan.tableIndex].dirtyRanges;
                    ranges.insert(ranges.end(), plan.consumedRanges.begin(),
                                  plan.consumedRanges.end());
                    CoalesceRanges(ranges);
                }
            }
            set.unusable = true;
        }
        for (Impl::BufferSet& set : m_impl->sets)
        {
            if (set.frameReadUse)
            {
                if (set.frameReadAccessCommitted)
                {
                    for (uint32 tableIndex = 0;
                         tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
                         ++tableIndex)
                    {
                        set.tables[tableIndex].access =
                            set.frameReadPreviousAccess[tableIndex];
                    }
                }
                set.frameReadUse = false;
                set.frameReadAccessCommitted = false;
                set.frameReadHandles = {};
                set.unusable = true;
            }
        }
        m_diagnostics.failureReason = GPUSceneUploadFailureReason::InvalidCompletionToken;
        m_diagnostics.rollbackPending = true;
        m_impl->pending.reset();
        return;
    }

    uint32 pendingSetIndex = RVX_INVALID_INDEX;
    if (m_impl->pending)
    {
        pendingSetIndex = m_impl->pending->setIndex;
        Impl::BufferSet& set = m_impl->sets[pendingSetIndex];
        set.residentVersion = m_impl->pending->version;
        m_diagnostics.residentVersion = set.residentVersion;
    }

    for (uint32 index = 0; index < m_impl->sets.size(); ++index)
    {
        Impl::BufferSet& set = m_impl->sets[index];
        if (!set.frameReadUse && index != pendingSetIndex)
        {
            continue;
        }
        GPUCompletionToken merged = set.hasLastUse ? set.lastUse : GPUCompletionToken{};
        if (!MergeGPUCompletionToken(merged, normalized))
        {
            set.unusable = true;
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::InvalidCompletionToken;
            continue;
        }
        set.lastUse = merged;
        set.hasLastUse = true;
        set.frameReadUse = false;
        set.frameReadAccessCommitted = false;
        set.frameReadHandles = {};
    }

    m_diagnostics.failureReason = GPUSceneUploadFailureReason::None;
    m_impl->pending.reset();
    static_cast<void>(PollSafeReclaimVersion());
}

void GPUSceneUploader::ReleaseUnsubmittedFrame() noexcept
{
    if (!m_impl)
    {
        return;
    }

    // RenderGraph has recorded a new access snapshot, but no GPU submission
    // owns that transition. Restore the last snapshot and merge the immutable
    // consumed delta back with any later Observe delta.
    if (m_impl->pending &&
        m_impl->pending->setIndex < m_impl->sets.size())
    {
        Impl::BufferSet& set = m_impl->sets[m_impl->pending->setIndex];
        for (const Impl::TableUploadPlan& plan : m_impl->pending->tables)
        {
            Impl::TableState& table = set.tables[plan.tableIndex];
            table.access = plan.previousAccess;
            if (plan.consumedFull)
            {
                table.fullDirty = true;
                table.dirtyRanges.clear();
            }
            else if (!table.fullDirty)
            {
                table.dirtyRanges.insert(table.dirtyRanges.end(),
                                         plan.consumedRanges.begin(),
                                         plan.consumedRanges.end());
                CoalesceRanges(table.dirtyRanges);
            }
        }
    }
    for (Impl::BufferSet& set : m_impl->sets)
    {
        if (set.frameReadUse && set.frameReadAccessCommitted)
        {
            for (uint32 tableIndex = 0;
                 tableIndex < GPU_SCENE_UPLOAD_TABLE_COUNT;
                 ++tableIndex)
            {
                set.tables[tableIndex].access =
                    set.frameReadPreviousAccess[tableIndex];
            }
        }
        set.frameReadUse = false;
        set.frameReadAccessCommitted = false;
        set.frameReadHandles = {};
    }
    m_diagnostics.rollbackPending = true;
    m_impl->pending.reset();
}

uint64 GPUSceneUploader::PollSafeReclaimVersion() noexcept
{
    if (!m_impl || !m_impl->initialized || !m_impl->tracker ||
        m_impl->deviceLost || m_impl->pending)
    {
        return 0;
    }

    uint64 safeVersion = m_impl->observedVersion;
    uint32 pendingSetCount = 0;
    for (Impl::BufferSet& set : m_impl->sets)
    {
        if (set.unusable)
        {
            m_impl->deviceLost = true;
            m_diagnostics.deviceLost = true;
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::DeviceLost;
            return 0;
        }
        if (set.frameReadUse)
        {
            ++pendingSetCount;
            m_diagnostics.pendingSetCount = pendingSetCount;
            return 0;
        }
        if (!set.hasLastUse)
        {
            continue;
        }

        const GPUCompletionStatus status = m_impl->tracker->Query(set.lastUse);
        if (status == GPUCompletionStatus::Lost)
        {
            set.unusable = true;
            m_impl->deviceLost = true;
            m_diagnostics.deviceLost = true;
            m_diagnostics.failureReason = GPUSceneUploadFailureReason::DeviceLost;
            return 0;
        }
        if (status == GPUCompletionStatus::Pending)
        {
            ++pendingSetCount;
            // A submitted version can still read identities retired by that
            // same commit. Reclaim only through the immediately preceding
            // committed version until every domain of the token completes.
            const uint64 beforeResident = set.residentVersion > 0
                ? set.residentVersion - 1U
                : 0;
            safeVersion = std::min(safeVersion, beforeResident);
        }
    }

    m_diagnostics.pendingSetCount = pendingSetCount;
    m_diagnostics.safeReclaimVersion = safeVersion;
    m_diagnostics.bufferSetCount = static_cast<uint32>(m_impl->sets.size());
    uint64 persistentBytes = 0;
    for (const Impl::BufferSet& set : m_impl->sets)
    {
        for (const Impl::TableState& table : set.tables)
        {
            persistentBytes += static_cast<uint64>(table.capacity) * table.stride;
        }
    }
    m_diagnostics.persistentBytes = persistentBytes;
    return safeVersion > m_impl->reclaimedThrough ? safeVersion : 0;
}

void GPUSceneUploader::ConfirmReclaimedThrough(uint64 version) noexcept
{
    if (!m_impl || version <= m_impl->reclaimedThrough ||
        version > m_diagnostics.safeReclaimVersion)
    {
        return;
    }
    m_impl->reclaimedThrough = version;
}

} // namespace RVX
