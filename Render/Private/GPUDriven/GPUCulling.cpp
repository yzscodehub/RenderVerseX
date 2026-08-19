/**
 * @file GPUCulling.cpp
 * @brief GPU-driven culling implementation
 */

#include "Render/GPUDriven/GPUCulling.h"
#include "Core/Log.h"
#include "GPUScene/GPUSceneUploader.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Visibility/RenderVisibility.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"
#include "ShaderCompiler/ShaderManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <numeric>
#include <optional>
#include <unordered_map>

namespace RVX
{
namespace
{
    static_assert(RVX_GPU_SCENE_CULLING_TABLE_COUNT ==
                  GPU_SCENE_RESIDENT_TABLE_COUNT);

    // Direct rendering owns the canonical visibility decision. GPU culling
    // must be conservative relative to that decision, including when a
    // Vulkan shader fuses the dot products differently at a frustum edge.
    constexpr float32 FrustumRejectRelativeTolerance =
        16.0f * std::numeric_limits<float32>::epsilon();

    // One compaction workgroup owns one draw group. Keep X within the native
    // dispatch limit and use Counts.z as the packed row width so the shader
    // can cover more than one X dimension without a second metadata buffer.
    constexpr uint32 CompactDispatchMaxWidth = 65535u;

    struct CompactDispatchDimensions
    {
        uint32 width = 1;
        uint32 height = 0;
    };

    [[nodiscard]] CompactDispatchDimensions GetCompactDispatchDimensions(
        uint32 drawGroupCount) noexcept
    {
        CompactDispatchDimensions dimensions;
        dimensions.width = std::min(
            std::max(drawGroupCount, 1u), CompactDispatchMaxWidth);
        dimensions.height = drawGroupCount == 0
            ? 0u
            : (drawGroupCount + dimensions.width - 1u) / dimensions.width;
        return dimensions;
    }

    [[nodiscard]] float32 GetConservativeFrustumRejectTolerance(
        float32 signedDistance,
        float32 projectedRadius) noexcept
    {
        return FrustumRejectRelativeTolerance * std::max(
            1.0f, std::abs(signedDistance) + std::abs(projectedRadius));
    }

    [[nodiscard]] bool IsConservativelyOutsideFrustumPlane(
        float32 signedDistance,
        float32 projectedRadius) noexcept
    {
        return signedDistance <
            -projectedRadius - GetConservativeFrustumRejectTolerance(
                signedDistance, projectedRadius);
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

    [[nodiscard]] uint64 MixQualificationIdentity(
        uint64 seed, uint64 value) noexcept
    {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
        value ^= value >> 31u;
        seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) +
            (seed >> 2u);
        return seed;
    }

    [[nodiscard]] uint64 HashQualificationIdentityList(
        std::vector<uint64> identities)
    {
        std::sort(identities.begin(), identities.end());
        uint64 hash = 0xCBF29CE484222325ull;
        for (const uint64 identity : identities)
        {
            hash = MixQualificationIdentity(hash, identity);
        }
        return hash;
    }

    [[nodiscard]] uint64 HashQualificationBytes(const void* bytes,
                                                size_t byteCount) noexcept
    {
        constexpr uint64 OffsetBasis = 0xCBF29CE484222325ull;
        constexpr uint64 Prime = 0x100000001B3ull;
        const auto* const first = static_cast<const uint8*>(bytes);
        uint64 hash = OffsetBasis;
        for (size_t index = 0; index < byteCount; ++index)
        {
            hash ^= first[index];
            hash *= Prime;
        }
        return hash;
    }

    [[nodiscard]] bool IsQualificationCaptureTier(
        GPUDrivenTier tier) noexcept
    {
        return tier == GPUDrivenTier::IndirectGrouped ||
            tier == GPUDrivenTier::GPUResidentScene;
    }

    RHIBufferUsage MakeGpuWritableStructuredUsage(RHIBufferUsage baseUsage)
    {
        return baseUsage |
               RHIBufferUsage::Structured |
               RHIBufferUsage::ShaderResource |
               RHIBufferUsage::UnorderedAccess |
               RHIBufferUsage::CopyDst;
    }

    MaterialPipelineVariant GetGPUCullingPipelineVariant(MaterialRenderMode mode)
    {
        switch (mode)
        {
            case MaterialRenderMode::Masked:
                return MaterialPipelineVariant::Masked;
            case MaterialRenderMode::Transparent:
                return MaterialPipelineVariant::Transparent;
            case MaterialRenderMode::Opaque:
            default:
                return MaterialPipelineVariant::Opaque;
        }
    }

    std::filesystem::path FindGPUCullingShaderPath()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            std::filesystem::path candidate = path / "Render" / "Shaders" / "GPUDriven" / "GPUCulling.hlsl";
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    std::filesystem::path FindGPUSceneCullingShaderPath()
    {
        std::filesystem::path path = std::filesystem::current_path();
        for (uint32 i = 0; i < 8; ++i)
        {
            std::filesystem::path candidate = path / "Render" / "Shaders" /
                "GPUDriven" / "GPUSceneCulling.hlsl";
            if (std::filesystem::exists(candidate))
            {
                return candidate;
            }

            if (!path.has_parent_path())
            {
                break;
            }
            path = path.parent_path();
        }

        return {};
    }

    GPUCullingConstants MakeDefaultGPUCullingConstants()
    {
        GPUCullingConstants constants{};
        constants.viewProj = Mat4(1.0f);
        for (Vec4& plane : constants.frustumPlanes)
        {
            plane = Vec4(0.0f);
        }
        constants.cameraPosition = Vec4(0.0f);
        constants.params = Vec4(0.0f);
        std::fill_n(constants.counts, 4, 0u);
        std::fill_n(constants.gpuSceneTableCounts0, 4, 0u);
        std::fill_n(constants.gpuSceneTableCounts1, 4, 0u);
        return constants;
    }

    uint32 GetGPUSceneTableRowStride(uint32 tableIndex)
    {
        switch (static_cast<GPUSceneResidentTable>(tableIndex))
        {
            case GPUSceneResidentTable::Primitives:
                return sizeof(GPUScenePrimitiveRow);
            case GPUSceneResidentTable::Bounds:
                return sizeof(GPUSceneBoundsRow);
            case GPUSceneResidentTable::Transforms:
                return sizeof(GPUSceneTransformRow);
            case GPUSceneResidentTable::Materials:
                return sizeof(GPUSceneMaterialRow);
            case GPUSceneResidentTable::Geometries:
                return sizeof(GPUSceneGeometryRow);
            case GPUSceneResidentTable::Draws:
                return sizeof(GPUSceneDrawMetadataRow);
            case GPUSceneResidentTable::Count:
            default:
                return 0;
        }
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

    [[nodiscard]] bool IsLiveCanonicalKey(
        const GPUCullingStableInstanceKey& key) noexcept
    {
        return key.IsValid();
    }

    [[nodiscard]] bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token,
        GPUCompletionPoint& outPoint) noexcept
    {
        outPoint = {};
        if (token.count == 0 || token.count > token.points.size() ||
            tracker.Query(token) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        for (uint8 pointIndex = 0; pointIndex < token.count; ++pointIndex)
        {
            const GPUCompletionPoint point = token.points[pointIndex];
            if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                if (outPoint.value != 0)
                {
                    return false;
                }
                outPoint = point;
            }
        }
        return outPoint.value != 0;
    }

    [[nodiscard]] bool IsCompletionSatisfied(
        GPUCompletionStatus status) noexcept
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }
} // namespace

// ============================================================================
// GPUCulling
// ============================================================================

GPUCulling::~GPUCulling()
{
    Shutdown();
}

bool GPUCulling::ReserveCpuCapacity(uint32 capacity)
{
    try
    {
        m_instances.reserve(capacity);
        m_gpuSceneCandidates.reserve(capacity);
        m_collectedKeys.reserve(capacity);
        m_collectedRasterSemanticIdentities.reserve(capacity);
        m_collectedSourceRevisions.reserve(capacity);
        m_collectedCanonicalRows.reserve(capacity);
        m_collectedRequiresComparison.reserve(capacity);
        m_removedRowsScratch.reserve(capacity);
        m_newRowsScratch.reserve(capacity);
        m_poppedFreeRowsScratch.reserve(capacity);
        m_reusedRemovedRowsScratch.reserve(capacity);
        m_preparedNewObjectIdsScratch.reserve(capacity);
        m_retiredObjectIdsScratch.reserve(capacity);
        m_instanceDirtyRowsScratch.reserve(capacity);
        m_candidateDirtyRowsScratch.reserve(capacity);
        m_activeRowDirtyRowsScratch.reserve(capacity);
        m_dirtyRangeScratch.reserve(capacity);
        m_drawGroups.reserve(capacity);
        m_groupDrawCounts.reserve(capacity);
        m_visibleInstanceIndices.reserve(capacity);
        m_rasterVisibleInstanceIndices.reserve(capacity);
        m_visibleSourceIndices.reserve(capacity);
        m_indirectCommands.reserve(capacity);
        m_collectedInputByKey.reserve(capacity);
        m_changedObjectIds.reserve(capacity);
        m_removedObjectIds.reserve(capacity);
        m_canonicalRowByKey.reserve(capacity);
        m_rowsByObjectId.reserve(capacity);
    }
    catch (...)
    {
        return false;
    }
    return true;
}

void GPUCulling::Initialize(IRHIDevice* device,
                            const GPUCullingConfig& config,
                            uint32 frameSlotCount)
{
    m_device = device;
    m_config = config;
    m_occlusionRequested = config.enableOcclusionCulling;
    // HZB production inputs and validation are not implemented yet. Preserve
    // the request for diagnostics, but never silently run a partial path.
    m_config.enableOcclusionCulling = false;
    m_config.twoPhaseOcclusion = false;
    frameSlotCount = std::clamp(frameSlotCount, 1u, RVX_MAX_FRAME_COUNT);
    m_frameInputs.resize(frameSlotCount);
    m_activeFrameSlot = 0;
    if (!ReserveCpuCapacity(config.maxInstances))
    {
        RVX_RENDER_ERROR("GPUCulling: failed to reserve CPU collection capacity");
        Shutdown();
        return;
    }
    if (!ResetCanonicalRows(config.maxInstances))
    {
        RVX_RENDER_ERROR("GPUCulling: failed to allocate canonical row store");
        Shutdown();
        return;
    }
    CreateResources();
    CreatePipelineResources();
}

void GPUCulling::Shutdown()
{
    m_instanceIndexBuffer.Reset();
    m_visibilityBuffer.Reset();
    m_visibleInstanceBuffer.Reset();
    m_indirectBuffer.Reset();
    m_drawCountBuffer.Reset();
    m_frameInputs.clear();
    m_activeFrameSlot = 0;
    m_frustumCullShader.Reset();
    m_compactShader.Reset();
    m_finalizeShader.Reset();
    m_cullingDescriptorSetLayout.Reset();
    m_cullingPipelineLayout.Reset();
    m_frustumCullPipeline.Reset();
    m_occlusionCullPipeline.Reset();
    m_compactPipeline.Reset();
    m_finalizePipeline.Reset();
    m_gpuSceneFrustumCullShader.Reset();
    m_gpuSceneCompactShader.Reset();
    m_gpuSceneFinalizeShader.Reset();
    m_gpuSceneDescriptorSetLayout.Reset();
    m_gpuScenePipelineLayout.Reset();
    m_gpuSceneFrustumCullPipeline.Reset();
    m_gpuSceneCompactPipeline.Reset();
    m_gpuSceneFinalizePipeline.Reset();
    m_gpuSceneDescriptorSet.Reset();
    m_gpuSceneTableBuffers = {};
    m_gpuSceneTableCapacities = {};
    m_gpuSceneLeaseVersion = 0;
    m_instances.clear();
    m_gpuSceneCandidates.clear();
    m_canonicalInstances.clear();
    m_canonicalGPUSceneCandidates.clear();
    m_canonicalGPUSceneCandidateValid.clear();
    m_canonicalActiveRows.clear();
    m_canonicalKeys.clear();
    m_canonicalSourceRevisions.clear();
    m_canonicalLastSeenEpoch.clear();
    m_liveCanonicalRows.clear();
    m_liveRowPositions.clear();
    m_rowObjectListPositions.clear();
    m_canonicalRowByKey.clear();
    m_rowsByObjectId.clear();
    m_collectedInputByKey.clear();
    m_changedObjectIds.clear();
    m_removedObjectIds.clear();
    m_collectedKeys.clear();
    m_collectedRasterSemanticIdentities.clear();
    m_collectedSourceRevisions.clear();
    m_collectedCanonicalRows.clear();
    m_collectedRequiresComparison.clear();
    m_removedRowsScratch.clear();
    m_newRowsScratch.clear();
    m_poppedFreeRowsScratch.clear();
    m_reusedRemovedRowsScratch.clear();
    m_preparedNewObjectIdsScratch.clear();
    m_retiredObjectIdsScratch.clear();
    m_instanceDirtyRowsScratch.clear();
    m_candidateDirtyRowsScratch.clear();
    m_activeRowDirtyRowsScratch.clear();
    m_dirtyRangeScratch.clear();
    m_instanceDirtyJournals.clear();
    m_candidateDirtyJournals.clear();
    m_activeRowDirtyJournals.clear();
    m_gpuSceneCandidateVersion = 0;
    m_gpuSceneCandidateCount = 0;
    m_nextSnapshotVersion = 0;
    m_canonicalInstanceVersion = 0;
    m_canonicalGPUSceneCandidateVersion = 0;
    m_canonicalActiveRowVersion = 0;
    m_finalizedInstanceVersion = 0;
    m_finalizedGPUSceneCandidateVersion = 0;
    m_finalizedActiveRowVersion = 0;
    m_incrementalDiagnostics = {};
    m_gpuSceneEnabled = false;
    m_recordingGpuOnly = false;
    m_gpuSceneQualificationArmed = false;
    m_gpuSceneQualificationCapture = {};
    m_pendingGPUSceneQualificationCapture = {};
    m_gpuSceneQualificationDiagnostics = {};
    m_statsBuffer.Reset();
    m_transientUploadBuffers.clear();
    m_pendingOwnerRetirements.clear();
    m_device = nullptr;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_pipelineFallbackReason = GPUCullingFallbackReason::None;
    m_accessSnapshots = {};
}

void GPUCulling::RetireOwnerSnapshots(
    const GPUCompletionToken& completion,
    RenderRetirementQueue& retirement)
{
    FlushRenderOwnerRetirements(
        m_pendingOwnerRetirements, completion, retirement);
}

bool GPUCulling::RetainSubmissionResources(
    RenderSubmissionResourceBatch& batch)
{
    for (const RHIBufferRef& buffer : m_transientUploadBuffers)
    {
        if (!batch.Retain(buffer, buffer ? buffer->GetSize() : 0))
        {
            return false;
        }
    }
    m_transientUploadBuffers.clear();
    return true;
}

bool GPUCulling::RetainSealedSubmissionResources(
    RenderSubmissionResourceBatch& batch)
{
    // A sealed state may outlive both the graph callbacks that recorded it and
    // the source culler that created it.  Retain every RHI object that its
    // recorded cull or indirect commands can reference; RenderGraph only owns
    // graph-created resources and must not be relied upon for these objects.
    if (!RetainSubmissionResources(batch))
    {
        return false;
    }

    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (inputs == nullptr)
    {
        return false;
    }

    const auto retain = [&batch]<typename T>(const Ref<T>& object,
                                              uint64 estimatedBytes = 0)
    {
        return !object || batch.Retain(object, estimatedBytes);
    };
    const auto retainBuffer = [&retain](const RHIBufferRef& buffer)
    {
        return retain(buffer, buffer ? buffer->GetSize() : 0);
    };

    const bool retainedCoreResources = retainBuffer(inputs->instanceBuffer) &&
        retainBuffer(inputs->gpuSceneCandidateBuffer) &&
        retainBuffer(inputs->constantsBuffer) &&
        retain(inputs->descriptorSet) &&
        retainBuffer(m_instanceIndexBuffer) &&
        retainBuffer(m_visibilityBuffer) &&
        retainBuffer(m_visibleInstanceBuffer) &&
        retainBuffer(m_indirectBuffer) &&
        retainBuffer(m_drawCountBuffer) &&
        retainBuffer(m_statsBuffer) &&
        retain(m_frustumCullShader) &&
        retain(m_compactShader) &&
        retain(m_finalizeShader) &&
        retain(m_cullingDescriptorSetLayout) &&
        retain(m_cullingPipelineLayout) &&
        retain(m_frustumCullPipeline) &&
        retain(m_occlusionCullPipeline) &&
        retain(m_compactPipeline) &&
        retain(m_finalizePipeline) &&
        retain(m_gpuSceneFrustumCullShader) &&
        retain(m_gpuSceneCompactShader) &&
        retain(m_gpuSceneFinalizeShader) &&
        retain(m_gpuSceneDescriptorSetLayout) &&
        retain(m_gpuScenePipelineLayout) &&
        retain(m_gpuSceneFrustumCullPipeline) &&
        retain(m_gpuSceneCompactPipeline) &&
        retain(m_gpuSceneFinalizePipeline) &&
        retain(m_gpuSceneDescriptorSet);
    if (!retainedCoreResources)
    {
        return false;
    }

    const GPUSceneQualificationCapture& qualification =
        m_gpuSceneQualificationCapture;
    if (!retainBuffer(qualification.visibilityReadback) ||
        !retainBuffer(qualification.visibleRowsReadback) ||
        !retainBuffer(qualification.drawCountsReadback) ||
        !retainBuffer(qualification.indirectCommandsReadback))
    {
        return false;
    }

    for (const RHIBufferRef& tableBuffer : m_gpuSceneTableBuffers)
    {
        if (!retainBuffer(tableBuffer))
        {
            return false;
        }
    }
    return true;
}

void GPUCulling::SetConfig(const GPUCullingConfig& config)
{
    bool needsResize = config.maxInstances != m_config.maxInstances;
    // Complete all potentially allocating CPU capacity work before changing
    // the published configuration or replacing frame-slot resources. A
    // failed grow therefore leaves the previous culler usable and truthful.
    if (needsResize && !ReserveCpuCapacity(config.maxInstances))
    {
        RVX_RENDER_ERROR("GPUCulling: rejected resize because CPU reserve failed");
        return;
    }
    if (needsResize && !ResetCanonicalRows(config.maxInstances))
    {
        RVX_RENDER_ERROR("GPUCulling: rejected resize because canonical allocation failed");
        return;
    }

    m_config = config;
    m_occlusionRequested = config.enableOcclusionCulling;
    m_config.enableOcclusionCulling = false;
    m_config.twoPhaseOcclusion = false;

    if (needsResize)
    {
        ++m_incrementalDiagnostics.capacityFullMaterializationCount;
        SaturatingAdd(m_mutationTotals.capacityFullMaterializationCount,
                      1,
                      m_mutationTotalsSaturated);
        CreateResources();
        CreatePipelineResources();
    }
}

bool GPUCulling::SetFrameSlot(uint32 frameSlot)
{
    if (frameSlot >= m_frameInputs.size())
    {
        return false;
    }

    m_activeFrameSlot = frameSlot;
    RefreshActiveInputAccessSnapshots();
    return true;
}

RHIBuffer* GPUCulling::GetInstanceBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->instanceBuffer.Get() : nullptr;
}

RHIBuffer* GPUCulling::GetGPUSceneCandidateBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->gpuSceneCandidateBuffer.Get() : nullptr;
}

GPUSceneRasterResourceSnapshot
GPUCulling::GetGPUSceneRasterResourceSnapshot() const
{
    GPUSceneRasterResourceSnapshot snapshot;
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    constexpr uint32 primitiveTableIndex =
        static_cast<uint32>(GPUSceneResidentTable::Primitives);
    constexpr uint32 transformTableIndex =
        static_cast<uint32>(GPUSceneResidentTable::Transforms);
    if (!m_gpuSceneEnabled || !HasCompleteGPUSceneCandidates() ||
        m_gpuSceneLeaseVersion == 0 ||
        m_gpuSceneLeaseVersion != m_gpuSceneCandidateVersion ||
        inputs == nullptr || !inputs->gpuSceneCandidateBuffer ||
        !IsGPUSceneCandidateResident(*inputs) ||
        !m_gpuSceneTableBuffers[primitiveTableIndex] ||
        !m_gpuSceneTableBuffers[transformTableIndex] ||
        m_gpuSceneTableCapacities[primitiveTableIndex] == 0 ||
        m_gpuSceneTableCapacities[transformTableIndex] == 0)
    {
        return snapshot;
    }

    snapshot.m_candidates = inputs->gpuSceneCandidateBuffer;
    snapshot.m_primitives = m_gpuSceneTableBuffers[primitiveTableIndex];
    snapshot.m_transforms = m_gpuSceneTableBuffers[transformTableIndex];
    snapshot.m_candidateCount =
        m_incrementalDiagnostics.activeRowHighWatermark;
    snapshot.m_candidateCapacity =
        static_cast<uint32>(inputs->gpuSceneCandidateBuffer->GetSize() /
                            sizeof(GPUSceneCullingCandidate));
    snapshot.m_primitiveCapacity = m_gpuSceneTableCapacities[primitiveTableIndex];
    snapshot.m_transformCapacity = m_gpuSceneTableCapacities[transformTableIndex];
    snapshot.m_leaseVersion = m_gpuSceneLeaseVersion;
    snapshot.m_exactLeaseVersion = m_gpuSceneLeaseVersion;
    return snapshot;
}

RHIBuffer* GPUCulling::GetCullingConstantsBuffer() const
{
    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    return inputs != nullptr ? inputs->constantsBuffer.Get() : nullptr;
}

const GPUCullingAccessSnapshots& GPUCulling::GetAccessSnapshots() const
{
    return m_accessSnapshots;
}

void GPUCulling::CommitAccessSnapshots(
    const GPUCullingAccessSnapshots& snapshots)
{
    m_accessSnapshots = snapshots;
    if (GPUCullingFrameInputs* inputs = GetActiveFrameInputs())
    {
        inputs->instanceAccess = snapshots.instances;
        inputs->gpuSceneCandidateAccess = snapshots.gpuSceneCandidates;
        inputs->constantsAccess = snapshots.constants;
        inputs->accessSnapshots = snapshots;
    }
}

bool GPUCulling::CommitFrameSlotAccessSnapshots(
    uint32 frameSlot,
    const GPUCullingAccessSnapshots& snapshots)
{
    if (frameSlot >= m_frameInputs.size())
    {
        return false;
    }

    GPUCullingFrameInputs& inputs = m_frameInputs[frameSlot];
    inputs.instanceAccess = snapshots.instances;
    inputs.gpuSceneCandidateAccess = snapshots.gpuSceneCandidates;
    inputs.constantsAccess = snapshots.constants;
    inputs.accessSnapshots = snapshots;
    if (frameSlot == m_activeFrameSlot)
    {
        RefreshActiveInputAccessSnapshots();
    }
    return true;
}

GPUCulling::GPUCullingFrameInputs* GPUCulling::GetActiveFrameInputs()
{
    return m_activeFrameSlot < m_frameInputs.size()
        ? &m_frameInputs[m_activeFrameSlot]
        : nullptr;
}

const GPUCulling::GPUCullingFrameInputs*
    GPUCulling::GetActiveFrameInputs() const
{
    return m_activeFrameSlot < m_frameInputs.size()
        ? &m_frameInputs[m_activeFrameSlot]
        : nullptr;
}

void GPUCulling::RefreshActiveInputAccessSnapshots()
{
    if (const GPUCullingFrameInputs* inputs = GetActiveFrameInputs())
    {
        m_instanceIndexBuffer = inputs->instanceIndexBuffer;
        m_visibilityBuffer = inputs->visibilityBuffer;
        m_visibleInstanceBuffer = inputs->visibleInstanceBuffer;
        m_indirectBuffer = inputs->indirectBuffer;
        m_drawCountBuffer = inputs->drawCountBuffer;
        m_statsBuffer = inputs->statsBuffer;
        m_gpuSceneDescriptorSet = inputs->gpuSceneDescriptorSet;
        m_gpuSceneTableBuffers = inputs->gpuSceneTableBuffers;
        m_gpuSceneTableCapacities = inputs->gpuSceneTableCapacities;
        m_gpuSceneLeaseVersion = inputs->gpuSceneLeaseVersion;
        m_gpuSceneEnabled = m_gpuSceneDescriptorSet &&
            m_gpuSceneLeaseVersion != 0 &&
            IsGPUSceneCandidateResident(*inputs);
        m_accessSnapshots = inputs->accessSnapshots;
        m_accessSnapshots.instances = inputs->instanceAccess;
        m_accessSnapshots.gpuSceneCandidates =
            inputs->gpuSceneCandidateAccess;
        m_accessSnapshots.constants = inputs->constantsAccess;
    }
}

bool GPUCulling::IsInstanceResident(
    const GPUCullingFrameInputs& inputs) const noexcept
{
    return m_finalizedInstanceVersion != 0 &&
        inputs.instanceDesiredVersion == m_finalizedInstanceVersion &&
        inputs.instanceResidentVersion == inputs.instanceDesiredVersion &&
        inputs.instanceAccess.uniformAccess.contentValidity ==
            RHIContentValidity::Valid;
}

bool GPUCulling::IsActiveRowsResident(
    const GPUCullingFrameInputs& inputs) const noexcept
{
    return m_finalizedActiveRowVersion != 0 &&
        inputs.activeRowsDesiredVersion == m_finalizedActiveRowVersion &&
        inputs.activeRowsResidentVersion == inputs.activeRowsDesiredVersion &&
        inputs.accessSnapshots.instanceIndices.uniformAccess.contentValidity ==
            RHIContentValidity::Valid;
}

bool GPUCulling::IsGPUSceneCandidateResident(
    const GPUCullingFrameInputs& inputs) const noexcept
{
    return m_finalizedGPUSceneCandidateVersion != 0 &&
        HasCompleteGPUSceneCandidates() &&
        inputs.gpuSceneCandidateDesiredVersion ==
            m_finalizedGPUSceneCandidateVersion &&
        inputs.gpuSceneCandidateResidentVersion ==
            inputs.gpuSceneCandidateDesiredVersion &&
        inputs.gpuSceneCandidateAccess.uniformAccess.contentValidity ==
            RHIContentValidity::Valid;
}

bool GPUCulling::IsCurrentInstanceSnapshotWithinConfiguredCapacity() const noexcept
{
    return m_instanceCount <= m_config.maxInstances &&
        (m_recordingGpuOnly || m_instanceCount == m_instances.size());
}

void GPUCulling::InvalidateFrameSnapshot() noexcept
{
    m_finalizedInstanceVersion = 0;
    m_finalizedGPUSceneCandidateVersion = 0;
    m_finalizedActiveRowVersion = 0;
}

uint64 GPUCulling::AllocateSnapshotVersion() noexcept
{
    ++m_nextSnapshotVersion;
    if (m_nextSnapshotVersion == 0)
    {
        ++m_nextSnapshotVersion;
    }
    return m_nextSnapshotVersion;
}

bool GPUCulling::ResetCanonicalRows(uint32 capacity)
{
    // Build the complete replacement off to the side. Configuration growth
    // must not leave a half-resized canonical store if one allocation fails.
    try
    {
        std::vector<GPUInstanceData> instances(capacity);
        std::vector<GPUSceneCullingCandidate> candidates(capacity);
        std::vector<uint8> candidateValid(capacity, 0);
        std::vector<GPUCullingActiveRow> activeRows(capacity);
        for (GPUCullingActiveRow& activeRow : activeRows)
        {
            activeRow.residentRow = RVX_INVALID_INDEX;
        }
        std::vector<GPUCullingStableInstanceKey> keys(capacity);
        std::vector<uint64> sourceRevisions(capacity, 0);
        std::vector<uint64> lastSeenEpoch(capacity, 0);
        std::vector<uint32> liveRows;
        liveRows.reserve(capacity);
        std::vector<uint32> livePositions(capacity, RVX_INVALID_INDEX);
        std::vector<uint32> objectListPositions(capacity, RVX_INVALID_INDEX);
        std::unordered_map<GPUCullingStableInstanceKey,
                           uint32,
                           StableKeyHasher> rowByKey;
        rowByKey.reserve(capacity);
        std::unordered_map<uint64, std::vector<uint32>> rowsByObjectId;
        rowsByObjectId.reserve(capacity);
        std::vector<uint32> freeRows;
        freeRows.reserve(capacity);
        for (uint32 row = 0; row < capacity; ++row)
        {
            freeRows.push_back(row);
        }
        std::make_heap(freeRows.begin(), freeRows.end(), std::greater<uint32>{});
        std::priority_queue<uint32,
                            std::vector<uint32>,
                            std::greater<uint32>> freeRowHeap(
            std::greater<uint32>{}, std::move(freeRows));

        m_canonicalInstances.swap(instances);
        m_canonicalGPUSceneCandidates.swap(candidates);
        m_canonicalGPUSceneCandidateValid.swap(candidateValid);
        m_canonicalActiveRows.swap(activeRows);
        m_canonicalKeys.swap(keys);
        m_canonicalSourceRevisions.swap(sourceRevisions);
        m_canonicalLastSeenEpoch.swap(lastSeenEpoch);
        m_liveCanonicalRows.swap(liveRows);
        m_liveRowPositions.swap(livePositions);
        m_rowObjectListPositions.swap(objectListPositions);
        m_canonicalRowByKey.swap(rowByKey);
        m_rowsByObjectId.swap(rowsByObjectId);
        m_freeCanonicalRows.swap(freeRowHeap);
    }
    catch (...)
    {
        return false;
    }
    m_instanceDirtyJournals.clear();
    m_candidateDirtyJournals.clear();
    m_activeRowDirtyJournals.clear();
    m_canonicalInstanceVersion = 0;
    m_canonicalGPUSceneCandidateVersion = 0;
    m_canonicalActiveRowVersion = 0;
    InvalidateFrameSnapshot();
    return true;
}

GPUInstanceData GPUCulling::NormalizeCanonicalInstance(
    GPUInstanceData instance) noexcept
{
    // These four values belong to the current pass recording, not to an
    // object/submesh row. Keeping them in the canonical stream would make a
    // harmless packet reorder look like a transform/material mutation.
    instance.sourceIndex = RVX_INVALID_INDEX;
    instance.drawGroupIndex = RVX_INVALID_INDEX;
    instance.drawGroupVisibleOffset = 0;
    instance.candidateIndex = RVX_INVALID_INDEX;
    instance.padding[0] = 0;
    instance.padding[1] = 0;
    return instance;
}

GPUSceneCullingCandidate GPUCulling::NormalizeCanonicalCandidate(
    GPUSceneCullingCandidate candidate,
    uint32 residentRow) noexcept
{
    candidate.drawGroupIndex = RVX_INVALID_INDEX;
    candidate.drawGroupVisibleOffset = 0;
    candidate.rasterInstanceIndex = residentRow;
    candidate.padding0 = 0;
    return candidate;
}

void GPUCulling::MergeDirtyRows(std::vector<uint32>& rows,
                                std::vector<DirtyRange>& outRanges)
{
    outRanges.clear();
    if (rows.empty())
    {
        return;
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    uint32 first = rows.front();
    uint32 previous = first;
    for (size_t index = 1; index < rows.size(); ++index)
    {
        const uint32 row = rows[index];
        if (row == previous + 1u)
        {
            previous = row;
            continue;
        }
        outRanges.push_back({first, previous - first + 1u});
        first = previous = row;
    }
    outRanges.push_back({first, previous - first + 1u});
}

void GPUCulling::PruneDirtyJournals()
{
    const auto prune = [this](std::deque<DirtyJournal>& journals,
                              auto desiredVersion,
                              auto residentVersion)
    {
        if (journals.empty() || m_frameInputs.empty())
        {
            return;
        }
        uint64 minimumResident = std::numeric_limits<uint64>::max();
        for (const GPUCullingFrameInputs& inputs : m_frameInputs)
        {
            const uint64 desired = desiredVersion(inputs);
            const uint64 resident = residentVersion(inputs);
            if (desired != 0 && resident != desired)
            {
                return;
            }
            if (resident != 0)
            {
                minimumResident = std::min(minimumResident, resident);
            }
        }
        if (minimumResident == std::numeric_limits<uint64>::max())
        {
            return;
        }
        while (journals.size() > 1u &&
               journals.front().version < minimumResident)
        {
            journals.pop_front();
        }
    };
    prune(m_instanceDirtyJournals,
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.instanceDesiredVersion; },
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.instanceResidentVersion; });
    prune(m_candidateDirtyJournals,
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.gpuSceneCandidateDesiredVersion; },
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.gpuSceneCandidateResidentVersion; });
    prune(m_activeRowDirtyJournals,
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.activeRowsDesiredVersion; },
          [](const GPUCullingFrameInputs& inputs)
          { return inputs.activeRowsResidentVersion; });
}

bool GPUCulling::FinalizeCpuSnapshotVersions()
{
    if (!IsCurrentInstanceSnapshotWithinConfiguredCapacity() ||
        m_instances.size() != m_collectedKeys.size() ||
        m_instances.size() != m_collectedRasterSemanticIdentities.size() ||
        m_instances.size() != m_collectedSourceRevisions.size() ||
        m_instances.size() != m_collectedCanonicalRows.size() ||
        m_instances.size() != m_collectedRequiresComparison.size() ||
        m_canonicalGPUSceneCandidateValid.size() !=
            m_canonicalGPUSceneCandidates.size() ||
        (!m_gpuSceneCandidates.empty() &&
         m_gpuSceneCandidates.size() != m_instances.size()))
    {
        RVX_RENDER_ERROR(
            "GPUCulling: rejected malformed incremental CPU culling inputs");
        return false;
    }

    const bool hasCompleteCandidates = HasCompleteGPUSceneCandidates();
    // Tier 1 can establish stable canonical rows before the GPU-scene
    // companion stream becomes available. When the first complete companion
    // stream arrives, source revisions may be unchanged, so it must still
    // materialize a canonical baseline and publish a candidate journal.
    const bool candidateBaselineMissing =
        m_canonicalGPUSceneCandidateVersion == 0;
    m_removedRowsScratch.clear();
    const auto considerRemoval = [this](uint32 row)
    {
        if (row >= m_canonicalKeys.size() ||
            !IsLiveCanonicalKey(m_canonicalKeys[row]) ||
            m_collectedInputByKey.contains(m_canonicalKeys[row]))
        {
            return;
        }
        m_removedRowsScratch.push_back(row);
    };

    if (m_fullCanonicalMutation)
    {
        for (uint32 row : m_liveCanonicalRows)
        {
            considerRemoval(row);
        }
    }
    else
    {
        const auto considerObject = [this, &considerRemoval](uint64 objectId)
        {
            const auto found = m_rowsByObjectId.find(objectId);
            if (found == m_rowsByObjectId.end())
            {
                return;
            }
            for (uint32 row : found->second)
            {
                if (row < m_canonicalKeys.size() &&
                    m_canonicalKeys[row].objectId == objectId)
                {
                    considerRemoval(row);
                }
            }
        };
        for (uint64 objectId : m_changedObjectIds)
        {
            considerObject(objectId);
        }
        for (uint64 objectId : m_removedObjectIds)
        {
            considerObject(objectId);
        }
    }
    std::sort(m_removedRowsScratch.begin(), m_removedRowsScratch.end());
    m_removedRowsScratch.erase(
        std::unique(m_removedRowsScratch.begin(), m_removedRowsScratch.end()),
        m_removedRowsScratch.end());

    // Validate every published reverse index before the preparation phase
    // changes even the free-row heap. A bad index is a contract violation;
    // it must leave the previous snapshot retryable.
    for (uint32 row : m_removedRowsScratch)
    {
        const GPUCullingStableInstanceKey& oldKey = m_canonicalKeys[row];
        if (!oldKey.IsValid())
        {
            continue;
        }
        const auto objectRows = m_rowsByObjectId.find(oldKey.objectId);
        const uint32 objectPosition = m_rowObjectListPositions[row];
        if (objectRows == m_rowsByObjectId.end() ||
            objectPosition >= objectRows->second.size() ||
            objectRows->second[objectPosition] != row ||
            m_liveRowPositions[row] == RVX_INVALID_INDEX ||
            m_liveRowPositions[row] >= m_liveCanonicalRows.size() ||
            m_liveCanonicalRows[m_liveRowPositions[row]] != row)
        {
            RVX_RENDER_ERROR("GPUCulling: corrupted stable object-to-row index");
            return false;
        }
    }
    for (uint32 inputIndex = 0;
         inputIndex < static_cast<uint32>(m_collectedCanonicalRows.size());
         ++inputIndex)
    {
        const uint32 row = m_collectedCanonicalRows[inputIndex];
        if (row == RVX_INVALID_INDEX)
        {
            continue;
        }
        if (row >= m_canonicalKeys.size() ||
            !(m_canonicalKeys[row] == m_collectedKeys[inputIndex]))
        {
            RVX_RENDER_ERROR("GPUCulling: corrupted collected stable row index");
            return false;
        }
    }

    // Prepare every new stable key before touching the published row payload,
    // live membership, or object index. This establishes the fail-closed
    // boundary: an allocation failure leaves the prior canonical snapshot,
    // generation versions, and object index semantically unchanged.
    m_newRowsScratch.clear();
    m_poppedFreeRowsScratch.clear();
    m_reusedRemovedRowsScratch.clear();
    m_preparedNewObjectIdsScratch.clear();
    size_t removedCursor = 0;
    try
    {
        for (uint32 inputIndex = 0;
             inputIndex < static_cast<uint32>(m_instances.size());
             ++inputIndex)
        {
            if (m_collectedCanonicalRows[inputIndex] != RVX_INVALID_INDEX)
            {
                continue;
            }
            m_newRowsScratch.push_back(inputIndex);
        }
        std::sort(m_newRowsScratch.begin(), m_newRowsScratch.end(),
                  [this](uint32 lhs, uint32 rhs)
                  {
                      const GPUCullingStableInstanceKey& left =
                          m_collectedKeys[lhs];
                      const GPUCullingStableInstanceKey& right =
                          m_collectedKeys[rhs];
                      return left.objectId != right.objectId
                          ? left.objectId < right.objectId
                          : left.logicalSubmeshIndex < right.logicalSubmeshIndex;
                  });

        for (uint32 inputIndex : m_newRowsScratch)
        {
            const bool hasFree = !m_freeCanonicalRows.empty();
            const bool hasRemoved = removedCursor < m_removedRowsScratch.size();
            if (!hasFree && !hasRemoved)
            {
                RVX_RENDER_ERROR("GPUCulling: stable row capacity exhausted");
                throw std::bad_alloc();
            }
            uint32 row = RVX_INVALID_INDEX;
            if (hasFree && (!hasRemoved ||
                            m_freeCanonicalRows.top() <
                                m_removedRowsScratch[removedCursor]))
            {
                row = m_freeCanonicalRows.top();
                m_freeCanonicalRows.pop();
                m_poppedFreeRowsScratch.push_back(row);
            }
            else
            {
                row = m_removedRowsScratch[removedCursor++];
                m_reusedRemovedRowsScratch.push_back(row);
            }
            m_collectedCanonicalRows[inputIndex] = row;
        }

        // Reserving object row lists before publication makes the subsequent
        // O(1) membership append non-allocating. New map entries stay empty
        // until commit and are removed by the rollback below.
        if (!m_newRowsScratch.empty())
        {
            FailStableRowPreparationCheckpoint();
        }
        for (size_t first = 0; first < m_newRowsScratch.size(); )
        {
            const uint64 objectId =
                m_collectedKeys[m_newRowsScratch[first]].objectId;
            size_t last = first + 1u;
            while (last < m_newRowsScratch.size() &&
                   m_collectedKeys[m_newRowsScratch[last]].objectId == objectId)
            {
                ++last;
            }
            auto [objectRows, inserted] =
                m_rowsByObjectId.try_emplace(objectId);
            if (inserted)
            {
                m_preparedNewObjectIdsScratch.push_back(objectId);
            }
            objectRows->second.reserve(
                objectRows->second.size() + (last - first));
            first = last;
        }

        for (uint32 inputIndex : m_newRowsScratch)
        {
            const uint32 row = m_collectedCanonicalRows[inputIndex];
            FailStableRowPreparationCheckpoint();
            if (!m_canonicalRowByKey.emplace(
                    m_collectedKeys[inputIndex], row).second)
            {
                RVX_RENDER_ERROR("GPUCulling: duplicate stable row key");
                throw std::bad_alloc();
            }
        }
    }
    catch (...)
    {
        for (uint32 inputIndex : m_newRowsScratch)
        {
            const auto found =
                m_canonicalRowByKey.find(m_collectedKeys[inputIndex]);
            if (found != m_canonicalRowByKey.end() &&
                found->second == m_collectedCanonicalRows[inputIndex])
            {
                m_canonicalRowByKey.erase(found);
            }
            m_collectedCanonicalRows[inputIndex] = RVX_INVALID_INDEX;
        }
        for (uint32 row : m_poppedFreeRowsScratch)
        {
            m_freeCanonicalRows.push(row);
        }
        for (uint64 objectId : m_preparedNewObjectIdsScratch)
        {
            const auto found = m_rowsByObjectId.find(objectId);
            if (found != m_rowsByObjectId.end() && found->second.empty())
            {
                m_rowsByObjectId.erase(found);
            }
        }
        RVX_RENDER_ERROR("GPUCulling: failed to allocate incremental stable rows");
        return false;
    }

    m_instanceDirtyRowsScratch.clear();
    m_candidateDirtyRowsScratch.clear();
    m_activeRowDirtyRowsScratch.clear();

    // Retire absent rows now that all potential allocation points are proved.
    // Inactive rows need no instance/candidate scrub because every dispatch
    // first resolves and validates the active-row indirection.
    m_retiredObjectIdsScratch.clear();
    for (uint32 row : m_removedRowsScratch)
    {
        const bool reused = std::binary_search(
            m_reusedRemovedRowsScratch.begin(),
            m_reusedRemovedRowsScratch.end(), row);
        const GPUCullingStableInstanceKey oldKey = m_canonicalKeys[row];
        if (oldKey.IsValid())
        {
            m_canonicalRowByKey.erase(oldKey);
            const auto objectRows = m_rowsByObjectId.find(oldKey.objectId);
            const uint32 objectPosition = m_rowObjectListPositions[row];
            const uint32 movedObjectRow = objectRows->second.back();
            objectRows->second[objectPosition] = movedObjectRow;
            m_rowObjectListPositions[movedObjectRow] = objectPosition;
            objectRows->second.pop_back();
            m_rowObjectListPositions[row] = RVX_INVALID_INDEX;
            m_retiredObjectIdsScratch.push_back(oldKey.objectId);
        }
        const uint32 livePosition = m_liveRowPositions[row];
        if (livePosition != RVX_INVALID_INDEX &&
            livePosition < m_liveCanonicalRows.size())
        {
            const uint32 movedRow = m_liveCanonicalRows.back();
            m_liveCanonicalRows[livePosition] = movedRow;
            m_liveRowPositions[movedRow] = livePosition;
            m_liveCanonicalRows.pop_back();
            m_liveRowPositions[row] = RVX_INVALID_INDEX;
        }
        m_canonicalKeys[row] = {};
        m_canonicalSourceRevisions[row] = 0;
        m_canonicalLastSeenEpoch[row] = 0;
        m_canonicalGPUSceneCandidateValid[row] = 0;
        if (!reused)
        {
            m_freeCanonicalRows.push(row);
        }
    }

    for (uint32 inputIndex = 0;
         inputIndex < static_cast<uint32>(m_instances.size());
         ++inputIndex)
    {
        const uint32 row = m_collectedCanonicalRows[inputIndex];
        RVX_ASSERT(row < m_canonicalInstances.size());
        const bool newRow = !m_canonicalKeys[row].IsValid();
        const GPUInstanceData canonical =
            NormalizeCanonicalInstance(m_instances[inputIndex]);
        if (newRow || m_collectedRequiresComparison[inputIndex] != 0)
        {
            if (newRow || std::memcmp(&canonical, &m_canonicalInstances[row],
                                      sizeof(canonical)) != 0)
            {
                m_canonicalInstances[row] = canonical;
                m_instanceDirtyRowsScratch.push_back(row);
            }
        }
        if (newRow)
        {
            m_canonicalKeys[row] = m_collectedKeys[inputIndex];
            m_liveRowPositions[row] =
                static_cast<uint32>(m_liveCanonicalRows.size());
            m_liveCanonicalRows.push_back(row);
            const auto objectRows =
                m_rowsByObjectId.find(m_collectedKeys[inputIndex].objectId);
            RVX_ASSERT(objectRows != m_rowsByObjectId.end());
            RVX_ASSERT(objectRows->second.size() < objectRows->second.capacity());
            m_rowObjectListPositions[row] =
                static_cast<uint32>(objectRows->second.size());
            objectRows->second.push_back(row);
        }
        m_canonicalSourceRevisions[row] = m_collectedSourceRevisions[inputIndex];
        m_canonicalLastSeenEpoch[row] = m_collectionEpoch;

        // The active indirection is a dense current-frame dispatch stream,
        // not a second sparse resident table. Packet/draw-group collection is
        // deterministic and group-contiguous, while residentRow continues to
        // address the independent sparse instance/candidate payloads.
        GPUCullingActiveRow activeRow;
        activeRow.residentRow = row;
        activeRow.drawGroupIndex = m_instances[inputIndex].drawGroupIndex;
        activeRow.drawGroupVisibleOffset =
            m_instances[inputIndex].drawGroupVisibleOffset;
        if (!(m_canonicalActiveRows[inputIndex] == activeRow))
        {
            m_canonicalActiveRows[inputIndex] = activeRow;
            m_activeRowDirtyRowsScratch.push_back(inputIndex);
        }

        if (!hasCompleteCandidates)
        {
            // Tier 1 may advance a stable row while the companion stream is
            // absent. Retain the old candidate bytes for sparse future
            // comparison, but require the next complete stream to validate
            // any new, reused, or changed row before it can be consumed.
            if (newRow || m_collectedRequiresComparison[inputIndex] != 0)
            {
                m_canonicalGPUSceneCandidateValid[row] = 0;
            }
        }
        else
        {
            const GPUSceneCullingCandidate candidate =
                NormalizeCanonicalCandidate(m_gpuSceneCandidates[inputIndex], row);
            const bool mustCompare = candidateBaselineMissing || newRow ||
                m_collectedRequiresComparison[inputIndex] != 0 ||
                m_canonicalGPUSceneCandidateValid[row] == 0;
            if (mustCompare &&
                (candidateBaselineMissing ||
                 std::memcmp(&candidate,
                             &m_canonicalGPUSceneCandidates[row],
                             sizeof(candidate)) != 0))
            {
                m_canonicalGPUSceneCandidates[row] = candidate;
                m_candidateDirtyRowsScratch.push_back(row);
            }
            m_canonicalGPUSceneCandidateValid[row] = 1;
        }
    }

    // Delete truly empty object lists only after all rows for this frame have
    // joined their prepared lists. This avoids stale indices without making a
    // same-object remove/add churn allocate during publication.
    std::sort(m_retiredObjectIdsScratch.begin(), m_retiredObjectIdsScratch.end());
    m_retiredObjectIdsScratch.erase(
        std::unique(m_retiredObjectIdsScratch.begin(),
                    m_retiredObjectIdsScratch.end()),
        m_retiredObjectIdsScratch.end());
    for (uint64 objectId : m_retiredObjectIdsScratch)
    {
        const auto objectRows = m_rowsByObjectId.find(objectId);
        if (objectRows != m_rowsByObjectId.end() && objectRows->second.empty())
        {
            m_rowsByObjectId.erase(objectRows);
        }
    }

    bool journalDiscontinuity = false;
    bool instanceVersionAdvanced = false;
    bool candidateVersionAdvanced = false;
    bool activeRowVersionAdvanced = false;
    try
    {
        const auto appendJournal = [this](std::deque<DirtyJournal>& journals,
                                          uint64 version,
                                          bool fullMaterialization)
        {
            DirtyJournal journal;
            journal.version = version;
            journal.fullMaterialization = fullMaterialization;
            journal.ranges = m_dirtyRangeScratch;
            FailDirtyJournalCheckpoint();
            journals.push_back(std::move(journal));
        };

        m_dirtyRangeScratch.clear();
        MergeDirtyRows(m_instanceDirtyRowsScratch, m_dirtyRangeScratch);
        if (!m_dirtyRangeScratch.empty())
        {
            ++m_canonicalInstanceVersion;
            if (m_canonicalInstanceVersion == 0)
            {
                ++m_canonicalInstanceVersion;
            }
            instanceVersionAdvanced = true;
            appendJournal(
                m_instanceDirtyJournals, m_canonicalInstanceVersion, false);
        }

        m_dirtyRangeScratch.clear();
        MergeDirtyRows(m_candidateDirtyRowsScratch, m_dirtyRangeScratch);
        if (hasCompleteCandidates && !m_dirtyRangeScratch.empty())
        {
            ++m_canonicalGPUSceneCandidateVersion;
            if (m_canonicalGPUSceneCandidateVersion == 0)
            {
                ++m_canonicalGPUSceneCandidateVersion;
            }
            candidateVersionAdvanced = true;
            appendJournal(m_candidateDirtyJournals,
                          m_canonicalGPUSceneCandidateVersion,
                          false);
        }

        m_dirtyRangeScratch.clear();
        MergeDirtyRows(m_activeRowDirtyRowsScratch, m_dirtyRangeScratch);
        if (!m_dirtyRangeScratch.empty())
        {
            ++m_canonicalActiveRowVersion;
            if (m_canonicalActiveRowVersion == 0)
            {
                ++m_canonicalActiveRowVersion;
            }
            activeRowVersionAdvanced = true;
            appendJournal(m_activeRowDirtyJournals,
                          m_canonicalActiveRowVersion,
                          false);
        }
    }
    catch (...)
    {
        // Canonical data is already committed, but no slot may consume it
        // without a complete revision journal. Drop every coverage claim so
        // later seals explicitly re-materialize all three streams.
        ForceFullInputMaterialization();
        ++m_incrementalDiagnostics.continuityFullMaterializationCount;
        SaturatingAdd(m_mutationTotals.continuityFullMaterializationCount,
                      1,
                      m_mutationTotalsSaturated);
        journalDiscontinuity = true;
        const auto advanceVersion = [](uint64& version)
        {
            ++version;
            if (version == 0)
            {
                ++version;
            }
        };
        if (!m_instanceDirtyRowsScratch.empty() && !instanceVersionAdvanced)
        {
            advanceVersion(m_canonicalInstanceVersion);
        }
        if (hasCompleteCandidates && !m_candidateDirtyRowsScratch.empty() &&
            !candidateVersionAdvanced)
        {
            advanceVersion(m_canonicalGPUSceneCandidateVersion);
        }
        if (!m_activeRowDirtyRowsScratch.empty() && !activeRowVersionAdvanced)
        {
            advanceVersion(m_canonicalActiveRowVersion);
        }
    }
    m_incrementalDiagnostics.instancePatchedRowCount =
        static_cast<uint32>(m_instanceDirtyRowsScratch.size());
    m_incrementalDiagnostics.candidatePatchedRowCount =
        static_cast<uint32>(m_candidateDirtyRowsScratch.size());
    m_incrementalDiagnostics.activeRowPatchedRowCount =
        static_cast<uint32>(m_activeRowDirtyRowsScratch.size());
    m_incrementalDiagnostics.activeRowCount = m_instanceCount;
    m_incrementalDiagnostics.activeRowHighWatermark =
        m_liveCanonicalRows.empty()
            ? 0
            : (*std::max_element(m_liveCanonicalRows.begin(),
                                 m_liveCanonicalRows.end()) + 1u);

    m_finalizedInstanceVersion = m_canonicalInstanceVersion;
    m_finalizedGPUSceneCandidateVersion = hasCompleteCandidates
        ? m_canonicalGPUSceneCandidateVersion
        : 0;
    m_finalizedActiveRowVersion = m_canonicalActiveRowVersion;
    if (!journalDiscontinuity)
    {
        PruneDirtyJournals();
    }
    const bool finalized = m_finalizedInstanceVersion != 0 &&
        m_finalizedActiveRowVersion != 0;
    if (finalized)
    {
        SaturatingAdd(m_mutationTotals.instancePatchedRowCount,
                      static_cast<uint64>(
                          m_incrementalDiagnostics.instancePatchedRowCount),
                      m_mutationTotalsSaturated);
        SaturatingAdd(m_mutationTotals.candidatePatchedRowCount,
                      static_cast<uint64>(
                          m_incrementalDiagnostics.candidatePatchedRowCount),
                      m_mutationTotalsSaturated);
        SaturatingAdd(m_mutationTotals.activeRowPatchedRowCount,
                      static_cast<uint64>(
                          m_incrementalDiagnostics.activeRowPatchedRowCount),
                      m_mutationTotalsSaturated);
    }
    return finalized;
}

bool GPUCulling::EnsureInstanceResidency()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!IsCurrentInstanceSnapshotWithinConfiguredCapacity() ||
        m_finalizedInstanceVersion == 0 || m_instances.empty() ||
        inputs == nullptr || !inputs->instanceBuffer)
    {
        return false;
    }

    inputs->instanceDesiredVersion = m_finalizedInstanceVersion;
    const bool instanceResident = IsInstanceResident(*inputs);
    const bool activeRowsResident = IsActiveRowsResident(*inputs);
    if (instanceResident && activeRowsResident)
    {
        return true;
    }

    if (!instanceResident)
    {
        // A new desired version must never inherit the previous slot's
        // readable state. Keep the prior resident version for diagnostics and
        // sparse retry, but prevent any seal from treating its bytes as the
        // new CPU snapshot. An active-row-only update retains the matching
        // instance stream and must leave its valid access intact.
        inputs->instanceAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs->instanceAccess.rangeOverrides.clear();
        m_accessSnapshots.instances = inputs->instanceAccess;
        inputs->accessSnapshots.instances = inputs->instanceAccess;
        if (!UploadInstances())
        {
            return false;
        }
    }
    if (activeRowsResident)
    {
        return true;
    }
    if (EnsureActiveRowsResidency())
    {
        return true;
    }

    // Two stream commits form one Tier 1 publication. A failed active-row
    // commit after an instance commit must not leave that new instance version
    // advertisable through a stale indirection binding.
    if (!instanceResident)
    {
        m_lastInstanceUploadBytes = 0;
        inputs->instanceResidentVersion = 0;
        inputs->instanceAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs->instanceAccess.rangeOverrides.clear();
        m_accessSnapshots.instances = inputs->instanceAccess;
        inputs->accessSnapshots.instances = inputs->instanceAccess;
    }
    return false;
}

void GPUCulling::InvalidateActiveRowResidency(
    GPUCullingFrameInputs& inputs) noexcept
{
    inputs.activeRowsResidentVersion = 0;
    inputs.accessSnapshots.instanceIndices.uniformAccess.contentValidity =
        RHIContentValidity::Invalid;
    inputs.accessSnapshots.instanceIndices.rangeOverrides.clear();
    m_accessSnapshots.instanceIndices = inputs.accessSnapshots.instanceIndices;
}

void GPUCulling::ForceFullInputMaterialization() noexcept
{
    // A journal construction failure occurs after canonical data has changed.
    // Discard all slot coverage rather than letting any slot interpret an old
    // resident revision as the new payload. The next seal for every slot
    // must take the explicit full-materialization path from canonical rows.
    m_instanceDirtyJournals.clear();
    m_candidateDirtyJournals.clear();
    m_activeRowDirtyJournals.clear();
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.instanceResidentVersion = 0;
        inputs.activeRowsResidentVersion = 0;
        inputs.gpuSceneCandidateResidentVersion = 0;
        inputs.instanceAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs.instanceAccess.rangeOverrides.clear();
        inputs.gpuSceneCandidateAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs.gpuSceneCandidateAccess.rangeOverrides.clear();
        inputs.accessSnapshots.instanceIndices.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs.accessSnapshots.instanceIndices.rangeOverrides.clear();
    }
    RefreshActiveInputAccessSnapshots();
}

bool GPUCulling::EnsureActiveRowsResidency()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (m_finalizedActiveRowVersion == 0 || inputs == nullptr ||
        !inputs->instanceIndexBuffer)
    {
        return false;
    }
    inputs->activeRowsDesiredVersion = m_finalizedActiveRowVersion;
    if (IsActiveRowsResident(*inputs))
    {
        return true;
    }
    // Preserve the prior resident version while attempting this delta. The
    // active-row journal needs that version to resolve the sparse ranges that
    // bridge the slot's previously resident snapshot to the desired one. A
    // failed upload still calls InvalidateActiveRowResidency() and fails
    // closed, so stale bytes can never be sealed as current.
    inputs->accessSnapshots.instanceIndices.uniformAccess.contentValidity =
        RHIContentValidity::Invalid;
    inputs->accessSnapshots.instanceIndices.rangeOverrides.clear();
    m_accessSnapshots.instanceIndices = inputs->accessSnapshots.instanceIndices;
    return UploadActiveRows();
}

bool GPUCulling::EnsureGPUSceneCandidateResidency()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!IsCurrentInstanceSnapshotWithinConfiguredCapacity() ||
        m_finalizedGPUSceneCandidateVersion == 0 ||
        !HasCompleteGPUSceneCandidates() || inputs == nullptr ||
        !inputs->gpuSceneCandidateBuffer)
    {
        return false;
    }

    inputs->gpuSceneCandidateDesiredVersion =
        m_finalizedGPUSceneCandidateVersion;
    if (IsGPUSceneCandidateResident(*inputs))
    {
        return true;
    }

    // Do not publish previous candidate bytes under a newly selected scene
    // lease/snapshot while the upload is in flight or retrying.
    inputs->gpuSceneCandidateAccess.uniformAccess.contentValidity =
        RHIContentValidity::Invalid;
    inputs->gpuSceneCandidateAccess.rangeOverrides.clear();
    m_accessSnapshots.gpuSceneCandidates = inputs->gpuSceneCandidateAccess;
    inputs->accessSnapshots.gpuSceneCandidates =
        inputs->gpuSceneCandidateAccess;
    return UploadGPUSceneCandidates();
}

void GPUCulling::QueueFrameInputRetirements()
{
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        QueueRenderOwnerRetirement(
            inputs.instanceBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.gpuSceneCandidateBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.constantsBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.instanceIndexBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.visibilityBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.visibleInstanceBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.indirectBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.drawCountBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.statsBuffer, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.descriptorSet, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.gpuSceneDescriptorSet, m_pendingOwnerRetirements);
        for (RHIBufferRef& tableBuffer : inputs.gpuSceneTableBuffers)
        {
            tableBuffer.Reset();
        }
    }
    m_frameInputs.clear();
}

void GPUCulling::CreateResources()
{
    if (!m_device) return;

    const uint32 frameSlotCount = std::max(
        1u, static_cast<uint32>(m_frameInputs.size()));
    m_instanceIndexBuffer.Reset();
    m_visibilityBuffer.Reset();
    m_visibleInstanceBuffer.Reset();
    m_indirectBuffer.Reset();
    m_drawCountBuffer.Reset();
    m_statsBuffer.Reset();
    m_gpuSceneDescriptorSet.Reset();
    m_gpuSceneTableBuffers = {};
    m_gpuSceneTableCapacities = {};
    m_gpuSceneLeaseVersion = 0;
    m_gpuSceneEnabled = false;
    QueueFrameInputRetirements();
    m_frameInputs.resize(frameSlotCount);
    m_activeFrameSlot = std::min(m_activeFrameSlot, frameSlotCount - 1u);
    m_accessSnapshots = {};

    const bool gpuWritableOutputs = SupportsGpuExecution();
    const RHIMemoryType cpuOutputMemoryType = RHIMemoryType::Upload;

    RHIBufferDesc desc;

    // Per-flight CPU-written inputs. RenderContext waits the selected frame
    // slot before SceneRenderer selects it, so writing slot N never races the
    // GPU consuming slot N from an earlier frame.
    desc.size = m_config.maxInstances * sizeof(GPUInstanceData);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(GPUInstanceData);
    desc.debugName = "GPUCulling.InstanceBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.instanceBuffer = m_device->CreateBuffer(desc);
        if (inputs.instanceBuffer)
        {
            inputs.instanceAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ShaderResource,
                RHIShaderStage::Compute,
                GPUQueueDomain::Graphics,
                RHIContentValidity::Unknown);
        }
    }

    // Owner-local active-row indirection.  It replaces the old identity index
    // buffer: compute reads frame-local ordinals here, then resolves stable
    // canonical rows in gInstances/gCandidates. Raster still consumes the
    // compaction output directly, so this buffer is never rebound as a fake
    // vertex stream.
    desc.size = static_cast<uint64>(m_config.maxInstances) *
        sizeof(GPUCullingActiveRow);
    desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = sizeof(GPUCullingActiveRow);
    desc.debugName = "GPUCulling.ActiveRowsBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.instanceIndexBuffer = m_device->CreateBuffer(desc);
        if (inputs.instanceIndexBuffer)
        {
            inputs.accessSnapshots.instanceIndices =
                MakeRHIBufferAccessSnapshot(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Compute,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Unknown);
        }
    }

    // Visibility flag buffer (GPU cull pass output, compact pass input)
    desc.size = m_config.maxInstances * sizeof(uint32);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::Vertex)
        : RHIBufferUsage::Vertex;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.VisibilityBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.visibilityBuffer = m_device->CreateBuffer(desc);
    }

    // Visible instance buffer (output)
    desc.size = m_config.maxInstances * sizeof(uint32);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::Vertex)
        : RHIBufferUsage::Vertex;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.VisibleInstanceBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.visibleInstanceBuffer = m_device->CreateBuffer(desc);
        if (inputs.visibleInstanceBuffer)
        {
            inputs.accessSnapshots.visibleInstances =
                MakeRHIBufferAccessSnapshot(
                    gpuWritableOutputs
                        ? RHIResourceState::UnorderedAccess
                        : RHIResourceState::VertexBuffer,
                    gpuWritableOutputs
                        ? RHIShaderStage::Compute
                        : RHIShaderStage::Vertex,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Unknown);
        }
    }

    // Indirect draw buffer
    desc.size = m_config.maxInstances * sizeof(IndirectDrawIndexedCommand);
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::IndirectArgs)
        : (RHIBufferUsage::IndirectArgs | RHIBufferUsage::CopyDst);
    desc.memoryType = RHIMemoryType::Default;
    desc.stride = sizeof(IndirectDrawIndexedCommand);
    desc.debugName = "GPUCulling.IndirectDrawBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.indirectBuffer = m_device->CreateBuffer(desc);
    }

    // Draw count buffer. Element 0 is the legacy total draw count; grouped
    // draws use one counter per GPUCullingDrawGroup at the same index.
    desc.size = std::max<uint64>(sizeof(uint32) * 4,
                                 (static_cast<uint64>(m_config.maxInstances) + 1u) *
                                     sizeof(uint32));
    desc.usage = gpuWritableOutputs
        ? MakeGpuWritableStructuredUsage(RHIBufferUsage::IndirectArgs)
        : RHIBufferUsage::None;
    desc.memoryType = gpuWritableOutputs ? RHIMemoryType::Default : cpuOutputMemoryType;
    desc.stride = sizeof(uint32);
    desc.debugName = "GPUCulling.DrawCountBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.drawCountBuffer = m_device->CreateBuffer(desc);
    }

    // Per-flight culling constants must not be overwritten while a previous
    // compute dispatch is still consuming them.
    desc.size = 256;
    desc.usage = RHIBufferUsage::Constant;
    desc.memoryType = RHIMemoryType::Upload;
    desc.stride = 0;
    desc.debugName = "GPUCulling.ConstantsBuffer";
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        inputs.constantsBuffer = m_device->CreateBuffer(desc);
        if (inputs.constantsBuffer)
        {
            inputs.constantsAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Compute,
                GPUQueueDomain::Graphics,
                RHIContentValidity::Unknown);
        }
    }
    // Statistics buffer (optional)
    if (m_statsEnabled)
    {
        desc.size = sizeof(uint32) * 8;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Readback;
        desc.stride = sizeof(uint32);
        desc.debugName = "GPUCulling.StatsBuffer";
        for (GPUCullingFrameInputs& inputs : m_frameInputs)
        {
            inputs.statsBuffer = m_device->CreateBuffer(desc);
        }
    }

    RefreshActiveInputAccessSnapshots();
}

void GPUCulling::CreatePipelineResources()
{
    m_pipelineFallbackReason = GPUCullingFallbackReason::None;
    QueueRenderOwnerRetirement(m_frustumCullShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_compactShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_finalizeShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_cullingDescriptorSetLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_cullingPipelineLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_frustumCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_occlusionCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_compactPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_finalizePipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFrustumCullShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneCompactShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFinalizeShader, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneDescriptorSetLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuScenePipelineLayout, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFrustumCullPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneCompactPipeline, m_pendingOwnerRetirements);
    QueueRenderOwnerRetirement(m_gpuSceneFinalizePipeline, m_pendingOwnerRetirements);
    m_gpuSceneDescriptorSet.Reset();
    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        QueueRenderOwnerRetirement(
            inputs.descriptorSet, m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            inputs.gpuSceneDescriptorSet, m_pendingOwnerRetirements);
        inputs.gpuSceneTableBuffers = {};
        inputs.gpuSceneTableCapacities = {};
        inputs.gpuSceneLeaseVersion = 0;
    }
    m_gpuSceneTableBuffers = {};
    m_gpuSceneTableCapacities = {};
    m_gpuSceneLeaseVersion = 0;
    m_gpuSceneEnabled = false;

    if (!SupportsGpuExecution())
    {
        m_pipelineFallbackReason = EvaluateGpuExecution(false).fallbackReason;
        return;
    }

    const bool hasCompleteFrameInputs =
        !m_frameInputs.empty() &&
        std::all_of(m_frameInputs.begin(), m_frameInputs.end(),
                    [](const GPUCullingFrameInputs& inputs)
                    {
                        return inputs.instanceBuffer &&
                            inputs.constantsBuffer &&
                            inputs.instanceIndexBuffer &&
                            inputs.visibilityBuffer &&
                            inputs.visibleInstanceBuffer &&
                            inputs.indirectBuffer &&
                            inputs.drawCountBuffer;
                    });
    if (!hasCompleteFrameInputs)
    {
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    const std::filesystem::path shaderPath = FindGPUCullingShaderPath();
    if (shaderPath.empty())
    {
        RVX_RENDER_WARN("GPUCulling: GPU shader file not found; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderFileMissing;
        return;
    }

    RHIDescriptorSetLayoutDesc setLayoutDesc;
    setLayoutDesc.debugName = "GPUCulling.DescriptorSetLayout";
    setLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(3, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(4, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(5, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    setLayoutDesc.AddBinding(6, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    m_cullingDescriptorSetLayout = m_device->CreateDescriptorSetLayout(setLayoutDesc);
    if (!m_cullingDescriptorSetLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create descriptor set layout; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed;
        return;
    }

    RHIPipelineLayoutDesc pipelineLayoutDesc;
    pipelineLayoutDesc.debugName = "GPUCulling.PipelineLayout";
    pipelineLayoutDesc.setLayouts = {m_cullingDescriptorSetLayout.Get()};
    m_cullingPipelineLayout = m_device->CreatePipelineLayout(pipelineLayoutDesc);
    if (!m_cullingPipelineLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create pipeline layout; CPU fallback remains active");
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineLayoutCreationFailed;
        return;
    }

    ShaderManagerConfig shaderConfig;
    shaderConfig.enableDiskCache = false;
    shaderConfig.enableAsyncCompile = false;
    shaderConfig.enableHotReload = false;
    ShaderManager shaderManager(shaderConfig);

    ShaderLoadDesc shaderDesc;
    shaderDesc.path = shaderPath.string();
    shaderDesc.stage = RHIShaderStage::Compute;
    shaderDesc.backend = m_device->GetBackendType();
    shaderDesc.enableDebugInfo = false;
    shaderDesc.enableOptimization = true;
    // HLSL shader-model selection is source-language metadata, not a DX12
    // execution gate.  The compiler selects DXIL/SPIR-V from the backend.
    shaderDesc.targetProfile = "cs_6_0";

    shaderDesc.entryPoint = "CSFrustumCull";
    ShaderLoadResult frustumResult = shaderManager.LoadFromFile(m_device, shaderDesc);
    if (!frustumResult.compileResult.success || !frustumResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile frustum cull shader: {}",
                        frustumResult.compileResult.errorMessage);
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderCompilationFailed;
        return;
    }
    m_frustumCullShader = frustumResult.shader;

    shaderDesc.entryPoint = "CSCompactDraws";
    ShaderLoadResult compactResult = shaderManager.LoadFromFile(m_device, shaderDesc);
    if (!compactResult.compileResult.success || !compactResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile compact shader: {}",
                        compactResult.compileResult.errorMessage);
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderCompilationFailed;
        return;
    }
    m_compactShader = compactResult.shader;

    shaderDesc.entryPoint = "CSFinalizeDrawGroups";
    ShaderLoadResult finalizeResult = shaderManager.LoadFromFile(m_device, shaderDesc);
    if (!finalizeResult.compileResult.success || !finalizeResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile draw-group finalize shader: {}",
                        finalizeResult.compileResult.errorMessage);
        m_pipelineFallbackReason = GPUCullingFallbackReason::ShaderCompilationFailed;
        return;
    }
    m_finalizeShader = finalizeResult.shader;

    RHIComputePipelineDesc pipelineDesc;
    pipelineDesc.pipelineLayout = m_cullingPipelineLayout.Get();

    pipelineDesc.computeShader = m_frustumCullShader.Get();
    pipelineDesc.debugName = "GPUCulling.FrustumCullPipeline";
    m_frustumCullPipeline = m_device->CreateComputePipeline(pipelineDesc);

    pipelineDesc.computeShader = m_compactShader.Get();
    pipelineDesc.debugName = "GPUCulling.CompactPipeline";
    m_compactPipeline = m_device->CreateComputePipeline(pipelineDesc);

    pipelineDesc.computeShader = m_finalizeShader.Get();
    pipelineDesc.debugName = "GPUCulling.FinalizeDrawGroupsPipeline";
    m_finalizePipeline = m_device->CreateComputePipeline(pipelineDesc);

    if (!m_frustumCullPipeline || !m_compactPipeline || !m_finalizePipeline)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create compute pipelines; CPU fallback remains active");
        m_frustumCullPipeline.Reset();
        m_compactPipeline.Reset();
        m_finalizePipeline.Reset();
        m_pipelineFallbackReason = GPUCullingFallbackReason::PipelineCreationFailed;
        return;
    }

    for (GPUCullingFrameInputs& inputs : m_frameInputs)
    {
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "GPUCulling.DescriptorSet";
        descriptorDesc.SetLayout(m_cullingDescriptorSetLayout.Get())
            .BindBuffer(0, inputs.constantsBuffer.Get())
            .BindBuffer(1, inputs.instanceBuffer.Get())
            .BindBuffer(2, inputs.instanceIndexBuffer.Get())
            .BindBuffer(3, inputs.visibilityBuffer.Get())
            .BindBuffer(4, inputs.visibleInstanceBuffer.Get())
            .BindBuffer(5, inputs.indirectBuffer.Get())
            .BindBuffer(6, inputs.drawCountBuffer.Get());
        inputs.descriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
        if (!inputs.descriptorSet)
        {
            RVX_RENDER_WARN("GPUCulling: failed to create frame-slot descriptor set; CPU fallback remains active");
            m_frustumCullPipeline.Reset();
            m_compactPipeline.Reset();
            m_finalizePipeline.Reset();
            m_pipelineFallbackReason =
                GPUCullingFallbackReason::DescriptorSetCreationFailed;
            return;
        }
    }

    // The GPU-scene path deliberately has an independent descriptor layout:
    // binding 1 is a stable candidate stream, binding 2 is the active-row
    // indirection, while bindings 7..12 are the six rows retained by one
    // exact GPU-scene lease.  No normal culling
    // descriptor or execution path is changed here.
    const std::filesystem::path gpuSceneShaderPath = FindGPUSceneCullingShaderPath();
    if (gpuSceneShaderPath.empty())
    {
        RVX_RENDER_WARN("GPUCulling: GPU-scene shader file not found; GPU-scene recording remains disabled");
        return;
    }

    RHIDescriptorSetLayoutDesc gpuSceneSetLayoutDesc;
    gpuSceneSetLayoutDesc.debugName = "GPUCulling.GPUSceneDescriptorSetLayout";
    gpuSceneSetLayoutDesc.AddBinding(0, RHIBindingType::UniformBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(1, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(2, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(3, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(4, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(5, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    gpuSceneSetLayoutDesc.AddBinding(6, RHIBindingType::StorageBuffer, RHIShaderStage::Compute);
    for (uint32 binding = 7; binding < 13; ++binding)
    {
        gpuSceneSetLayoutDesc.AddBinding(
            binding, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::Compute);
    }
    m_gpuSceneDescriptorSetLayout = m_device->CreateDescriptorSetLayout(gpuSceneSetLayoutDesc);
    if (!m_gpuSceneDescriptorSetLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene descriptor layout; GPU-scene recording remains disabled");
        return;
    }

    RHIPipelineLayoutDesc gpuScenePipelineLayoutDesc;
    gpuScenePipelineLayoutDesc.debugName = "GPUCulling.GPUScenePipelineLayout";
    gpuScenePipelineLayoutDesc.setLayouts = {m_gpuSceneDescriptorSetLayout.Get()};
    m_gpuScenePipelineLayout = m_device->CreatePipelineLayout(gpuScenePipelineLayoutDesc);
    if (!m_gpuScenePipelineLayout)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene pipeline layout; GPU-scene recording remains disabled");
        m_gpuSceneDescriptorSetLayout.Reset();
        return;
    }

    ShaderLoadDesc gpuSceneShaderDesc = shaderDesc;
    gpuSceneShaderDesc.path = gpuSceneShaderPath.string();
    gpuSceneShaderDesc.entryPoint = "CSGPUSceneFrustumCull";
    ShaderLoadResult gpuSceneFrustumResult = shaderManager.LoadFromFile(
        m_device, gpuSceneShaderDesc);
    if (!gpuSceneFrustumResult.compileResult.success || !gpuSceneFrustumResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile GPU-scene frustum shader: {}",
                        gpuSceneFrustumResult.compileResult.errorMessage);
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
        return;
    }
    m_gpuSceneFrustumCullShader = gpuSceneFrustumResult.shader;

    gpuSceneShaderDesc.entryPoint = "CSGPUSceneCompactDraws";
    ShaderLoadResult gpuSceneCompactResult = shaderManager.LoadFromFile(
        m_device, gpuSceneShaderDesc);
    if (!gpuSceneCompactResult.compileResult.success || !gpuSceneCompactResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile GPU-scene compact shader: {}",
                        gpuSceneCompactResult.compileResult.errorMessage);
        m_gpuSceneFrustumCullShader.Reset();
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
        return;
    }
    m_gpuSceneCompactShader = gpuSceneCompactResult.shader;

    gpuSceneShaderDesc.entryPoint = "CSGPUSceneFinalizeDrawGroups";
    ShaderLoadResult gpuSceneFinalizeResult = shaderManager.LoadFromFile(
        m_device, gpuSceneShaderDesc);
    if (!gpuSceneFinalizeResult.compileResult.success ||
        !gpuSceneFinalizeResult.shader)
    {
        RVX_RENDER_WARN("GPUCulling: failed to compile GPU-scene draw-group finalize shader: {}",
                        gpuSceneFinalizeResult.compileResult.errorMessage);
        m_gpuSceneFrustumCullShader.Reset();
        m_gpuSceneCompactShader.Reset();
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
        return;
    }
    m_gpuSceneFinalizeShader = gpuSceneFinalizeResult.shader;

    RHIComputePipelineDesc gpuScenePipelineDesc;
    gpuScenePipelineDesc.pipelineLayout = m_gpuScenePipelineLayout.Get();
    gpuScenePipelineDesc.computeShader = m_gpuSceneFrustumCullShader.Get();
    gpuScenePipelineDesc.debugName = "GPUCulling.GPUSceneFrustumCullPipeline";
    m_gpuSceneFrustumCullPipeline = m_device->CreateComputePipeline(gpuScenePipelineDesc);
    gpuScenePipelineDesc.computeShader = m_gpuSceneCompactShader.Get();
    gpuScenePipelineDesc.debugName = "GPUCulling.GPUSceneCompactPipeline";
    m_gpuSceneCompactPipeline = m_device->CreateComputePipeline(gpuScenePipelineDesc);
    gpuScenePipelineDesc.computeShader = m_gpuSceneFinalizeShader.Get();
    gpuScenePipelineDesc.debugName = "GPUCulling.GPUSceneFinalizeDrawGroupsPipeline";
    m_gpuSceneFinalizePipeline = m_device->CreateComputePipeline(gpuScenePipelineDesc);
    if (!m_gpuSceneFrustumCullPipeline || !m_gpuSceneCompactPipeline ||
        !m_gpuSceneFinalizePipeline)
    {
        RVX_RENDER_WARN("GPUCulling: failed to create GPU-scene pipelines; GPU-scene recording remains disabled");
        m_gpuSceneFrustumCullPipeline.Reset();
        m_gpuSceneCompactPipeline.Reset();
        m_gpuSceneFinalizePipeline.Reset();
        m_gpuSceneFrustumCullShader.Reset();
        m_gpuSceneCompactShader.Reset();
        m_gpuSceneFinalizeShader.Reset();
        m_gpuSceneDescriptorSetLayout.Reset();
        m_gpuScenePipelineLayout.Reset();
    }
}

GPUCullingExecutionDecision GPUCulling::EvaluateGpuExecution(bool requirePipelineResources) const
{
    GPUCullingExecutionDecision decision;
    if (!m_device)
    {
        decision.fallbackReason = GPUCullingFallbackReason::DeviceMissing;
        return decision;
    }

    const RHICapabilities& capabilities = m_device->GetCapabilities();
    if (!capabilities.supportsComputePipeline)
    {
        decision.fallbackReason = GPUCullingFallbackReason::ComputePipelineUnsupported;
        return decision;
    }

    if (!capabilities.supportsDescriptorSets)
    {
        decision.fallbackReason = GPUCullingFallbackReason::DescriptorSetsUnsupported;
        return decision;
    }

    if (!capabilities.indexedIndirectExecution.supportsCountBuffer)
    {
        decision.fallbackReason = GPUCullingFallbackReason::IndirectDrawCountUnsupported;
        return decision;
    }

    // The culling paths encode the source instance in every indexed-indirect
    // command, so a backend without firstInstance cannot consume this output.
    if (!capabilities.indexedIndirectExecution.supportsFirstInstance)
    {
        decision.fallbackReason =
            GPUCullingFallbackReason::IndirectDrawFirstInstanceUnsupported;
        return decision;
    }

    decision.gpuCapable = true;
    if (!requirePipelineResources)
    {
        decision.mode = GPUCullingExecutionMode::GpuCompute;
        decision.fallbackReason = GPUCullingFallbackReason::None;
        return decision;
    }

    if (m_pipelineFallbackReason != GPUCullingFallbackReason::None)
    {
        decision.fallbackReason = m_pipelineFallbackReason;
        return decision;
    }

    const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!m_frustumCullPipeline || !m_compactPipeline || !m_finalizePipeline ||
        inputs == nullptr ||
        !inputs->descriptorSet)
    {
        decision.fallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return decision;
    }

    decision.mode = GPUCullingExecutionMode::GpuCompute;
    decision.fallbackReason = GPUCullingFallbackReason::None;
    decision.pipelineReady = true;
    return decision;
}

bool GPUCulling::SupportsGpuExecution() const
{
    return EvaluateGpuExecution(false).gpuCapable;
}

bool GPUCulling::IsGPUSceneExecutionReady() const
{
    return EvaluateGpuExecution(false).gpuCapable &&
        m_gpuSceneDescriptorSetLayout &&
        m_gpuScenePipelineLayout &&
        m_gpuSceneFrustumCullPipeline &&
        m_gpuSceneCompactPipeline &&
        m_gpuSceneFinalizePipeline;
}

bool GPUCulling::ArmGPUSceneQualificationCapture()
{
    if (m_device == nullptr || m_gpuSceneQualificationArmed ||
        m_pendingGPUSceneQualificationCapture.submissionAccepted)
    {
        return false;
    }

    m_gpuSceneQualificationArmed = true;
    m_gpuSceneQualificationInputPlan = {};
    m_gpuSceneQualificationObservedInputIdentityHashes.clear();
    m_gpuSceneQualificationDiagnostics = {};
    m_gpuSceneQualificationDiagnostics.requested = true;
    m_gpuSceneQualificationDiagnostics.mismatch =
        GPUSceneCullingQualificationMismatch::CopyNotRecorded;
    return true;
}

bool GPUCulling::BeginGPUSceneQualificationInputCoverage(
    const GPUCullingQualificationInputPlan& inputPlan)
{
    if (!m_gpuSceneQualificationArmed)
    {
        return true;
    }

    const uint64 partitionCount =
        static_cast<uint64>(inputPlan.expectedGPUInputPacketCount) +
        inputPlan.directPacketCount + inputPlan.skippedPacketCount;
    if (!inputPlan.exactlyOncePartitioned ||
        inputPlan.expectedPacketCount == 0 ||
        inputPlan.expectedGPUInputPacketCount == 0 ||
        partitionCount != inputPlan.expectedPacketCount ||
        inputPlan.expectedGPUInputIdentityHashes.size() !=
            inputPlan.expectedGPUInputPacketCount ||
        std::any_of(inputPlan.expectedGPUInputIdentityHashes.begin(),
                    inputPlan.expectedGPUInputIdentityHashes.end(),
                    [](uint64 identity) { return identity == 0; }) ||
        std::any_of(inputPlan.expectedDirectVisibleIdentityHashes.begin(),
                    inputPlan.expectedDirectVisibleIdentityHashes.end(),
                    [](uint64 identity) { return identity == 0; }))
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::InputCoverage);
        return false;
    }

    m_gpuSceneQualificationInputPlan = inputPlan;
    m_gpuSceneQualificationObservedInputIdentityHashes.clear();
    return true;
}

bool GPUCulling::ObserveGPUSceneQualificationInputIdentity(uint64 identityHash)
{
    if (!m_gpuSceneQualificationArmed)
    {
        return true;
    }
    if (identityHash == 0 ||
        m_gpuSceneQualificationInputPlan.expectedGPUInputPacketCount == 0)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::InputCoverage);
        return false;
    }

    // Keep the observed sequence verbatim. Completion compares an immutable
    // copy against the immutable plan list, which makes both duplicates and
    // packet-identity drift explicit evidence rather than collection-time
    // behavior changes.
    m_gpuSceneQualificationObservedInputIdentityHashes.push_back(identityHash);
    return true;
}

void GPUCulling::PublishGPUSceneQualificationRequiredLane(
    GPUDrivenTier capturedTier) noexcept
{
    if (!m_gpuSceneQualificationArmed || !IsQualificationCaptureTier(capturedTier))
    {
        return;
    }

    m_gpuSceneQualificationDiagnostics.requested = true;
    m_gpuSceneQualificationDiagnostics.required = true;
    m_gpuSceneQualificationDiagnostics.capturedTier = capturedTier;
}

void GPUCulling::SetGPUSceneQualificationFailure(
    GPUSceneCullingQualificationMismatch mismatch) noexcept
{
    m_gpuSceneQualificationDiagnostics.requested = true;
    m_gpuSceneQualificationDiagnostics.matched = false;
    m_gpuSceneQualificationDiagnostics.mismatch = mismatch;
}

bool GPUCulling::PrepareGPUSceneQualificationCapture(
    const GPUCullingRecordingIdentity& identity,
    GPUDrivenTier capturedTier,
    uint64 gpuSceneLeaseVersion,
    uint32 sourceFrameSlot,
    std::span<const GPUInstanceData> activeInstances,
    std::span<const GPUCullingActiveRow> activeRows,
    std::span<const uint32> activeResidentRows,
    std::span<const uint64> activeRasterSemanticIdentities)
{
    const bool required = m_gpuSceneQualificationDiagnostics.required;
    const bool priorInputCoverageFailure =
        m_gpuSceneQualificationDiagnostics.mismatch ==
        GPUSceneCullingQualificationMismatch::InputCoverage;
    m_gpuSceneQualificationCapture = {};
    m_gpuSceneQualificationDiagnostics = {};
    m_gpuSceneQualificationDiagnostics.requested = true;
    m_gpuSceneQualificationDiagnostics.required = required;
    m_gpuSceneQualificationDiagnostics.capturedTier = capturedTier;
    m_gpuSceneQualificationDiagnostics.frameSequence = identity.frameSequence;
    m_gpuSceneQualificationDiagnostics.recordEpoch = identity.recordEpoch;
    m_gpuSceneQualificationDiagnostics.gpuSceneLeaseVersion =
        gpuSceneLeaseVersion;
    m_gpuSceneQualificationDiagnostics.activeRowVersion =
        m_finalizedActiveRowVersion;
    m_gpuSceneQualificationDiagnostics.activeRowCount =
        static_cast<uint32>(activeInstances.size());
    m_gpuSceneQualificationDiagnostics.drawGroupCount =
        static_cast<uint32>(m_drawGroups.size());
    m_gpuSceneQualificationDiagnostics.mismatch = priorInputCoverageFailure
        ? GPUSceneCullingQualificationMismatch::InputCoverage
        : GPUSceneCullingQualificationMismatch::ReferenceUnavailable;

    const bool capturesGPUScene =
        capturedTier == GPUDrivenTier::GPUResidentScene;
    // Raster transcript semantics belong only to Tier 1.  Tier 2 uses this
    // capture plumbing for its existing qualification path, but must not
    // acquire a material transcript sidecar as a consequence.
    const bool capturesTierOneRaster =
        capturedTier == GPUDrivenTier::IndirectGrouped;
    m_gpuSceneQualificationDiagnostics.candidateVersion = capturesGPUScene
        ? m_finalizedGPUSceneCandidateVersion
        : 0;
    m_gpuSceneQualificationDiagnostics.expectedInputPacketCount =
        m_gpuSceneQualificationInputPlan.expectedPacketCount;
    m_gpuSceneQualificationDiagnostics.expectedGPUInputPacketCount =
        m_gpuSceneQualificationInputPlan.expectedGPUInputPacketCount;
    m_gpuSceneQualificationDiagnostics.observedGPUInputPacketCount =
        static_cast<uint32>(
            m_gpuSceneQualificationObservedInputIdentityHashes.size());
    m_gpuSceneQualificationDiagnostics.directInputPacketCount =
        m_gpuSceneQualificationInputPlan.directPacketCount;
    m_gpuSceneQualificationDiagnostics.skippedInputPacketCount =
        m_gpuSceneQualificationInputPlan.skippedPacketCount;
    m_gpuSceneQualificationDiagnostics.planPacketIdentityHash =
        m_gpuSceneQualificationInputPlan.planPacketIdentityHash;
    const bool inputCoverageUnavailable =
        !m_gpuSceneQualificationInputPlan.exactlyOncePartitioned ||
        m_gpuSceneQualificationInputPlan.expectedPacketCount == 0 ||
        m_gpuSceneQualificationInputPlan.expectedGPUInputPacketCount == 0 ||
        m_gpuSceneQualificationInputPlan.expectedGPUInputPacketCount !=
            activeInstances.size() ||
        m_gpuSceneQualificationInputPlan.expectedGPUInputIdentityHashes.size() !=
            activeInstances.size();
    if (inputCoverageUnavailable)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::InputCoverage);
        return false;
    }

    if (m_device == nullptr || !identity.IsValid() ||
        !IsQualificationCaptureTier(capturedTier) || !required ||
        activeInstances.empty() || activeInstances.size() != activeRows.size() ||
        activeInstances.size() != activeResidentRows.size() ||
        (capturesTierOneRaster &&
         activeInstances.size() != activeRasterSemanticIdentities.size()) ||
        activeInstances.size() != m_collectedKeys.size() ||
        activeInstances.size() != m_instanceCount ||
        activeInstances.size() != m_incrementalDiagnostics.activeRowCount ||
        m_drawGroups.empty() || m_finalizedActiveRowVersion == 0 ||
        (capturesGPUScene &&
         (gpuSceneLeaseVersion == 0 ||
          m_finalizedGPUSceneCandidateVersion == 0)))
    {
        return false;
    }

    uint64 coveredActiveRows = 0;
    for (uint32 groupIndex = 0;
         groupIndex < static_cast<uint32>(m_drawGroups.size());
         ++groupIndex)
    {
        const GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
        if (group.commandOffset >= m_drawGroups.size() ||
            group.visibleInstanceOffset != coveredActiveRows ||
            group.maxDrawCount == 0 ||
            group.maxDrawCount > activeInstances.size() - coveredActiveRows)
        {
            return false;
        }
        coveredActiveRows += group.maxDrawCount;
    }
    if (coveredActiveRows != activeInstances.size())
    {
        return false;
    }

    for (uint32 activeIndex = 0;
         activeIndex < static_cast<uint32>(activeRows.size());
         ++activeIndex)
    {
        const GPUCullingActiveRow& active = activeRows[activeIndex];
        if (active.residentRow == RVX_INVALID_INDEX ||
            active.residentRow != activeResidentRows[activeIndex] ||
            active.drawGroupIndex >= m_drawGroups.size() ||
            active.drawGroupIndex != activeInstances[activeIndex].drawGroupIndex ||
            active.drawGroupVisibleOffset !=
                activeInstances[activeIndex].drawGroupVisibleOffset)
        {
            return false;
        }
    }

    GPUSceneQualificationCapture capture;
    capture.identity = identity;
    capture.capturedTier = capturedTier;
    capture.required = required;
    capture.sourceFrameSlot = sourceFrameSlot;
    capture.gpuSceneLeaseVersion = gpuSceneLeaseVersion;
    capture.candidateVersion = capturesGPUScene
        ? m_finalizedGPUSceneCandidateVersion
        : 0;
    capture.activeRowVersion = m_finalizedActiveRowVersion;
    capture.activeRowCount = static_cast<uint32>(activeInstances.size());
    capture.drawGroupCount = static_cast<uint32>(m_drawGroups.size());
    try
    {
        capture.cpuActiveInstances.assign(activeInstances.begin(), activeInstances.end());
        capture.cpuActiveResidentRows.assign(
            activeResidentRows.begin(), activeResidentRows.end());
        if (capturesTierOneRaster)
        {
            capture.cpuActiveRasterSemanticIdentities.assign(
                activeRasterSemanticIdentities.begin(),
                activeRasterSemanticIdentities.end());
        }
        capture.cpuActiveKeys.reserve(m_collectedKeys.size());
        for (const GPUCullingStableInstanceKey& key : m_collectedKeys)
        {
            capture.cpuActiveKeys.push_back(
                {key.objectId, key.logicalSubmeshIndex});
        }
        if (capturedTier == GPUDrivenTier::IndirectGrouped)
        {
            capture.expectedRasterInstances.resize(capture.activeRowCount);
            for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount;
                 ++activeIndex)
            {
                // Compare the raster-resident row with the fresh frame's
                // active packet payload.  The sparse canonical table is the
                // upload source, and would therefore hide a stale canonical
                // row that the GPU faithfully retained.
                capture.expectedRasterInstances[activeIndex] =
                    NormalizeCanonicalInstance(activeInstances[activeIndex]);
            }
        }
        capture.expectedVisibility.resize(capture.activeRowCount);
        capture.expectedVisibleResidentRows.resize(
            capture.activeRowCount, RVX_INVALID_INDEX);
        capture.expectedInstanceCounts.resize(capture.drawGroupCount);
        capture.expectedDrawCounts.resize(capture.drawGroupCount);
        capture.expectedIndirectCommands.resize(capture.drawGroupCount);
        capture.drawGroups.resize(capture.drawGroupCount);
        for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount;
             ++groupIndex)
        {
            const GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
            capture.drawGroups[groupIndex] = {
                GetStableHash(group.batchKey),
                GetRasterTranscriptGroupIdentity(group.batchKey),
                group.mesh,
                group.commandOffset,
                group.visibleInstanceOffset,
                group.maxDrawCount,
                group.indexCount,
                group.firstIndex,
                group.vertexOffset,
                group.batchKey.usesMaterialParameterTable};
        }
        capture.inputPlan = m_gpuSceneQualificationInputPlan;
        capture.activeRowIdentityHashes =
            m_gpuSceneQualificationObservedInputIdentityHashes;
    }
    catch (...)
    {
        return false;
    }

    const auto createReadback = [this](uint64 size,
                                       uint32 stride,
                                       const char* debugName) -> RHIBufferRef
    {
        RHIBufferDesc desc;
        desc.size = size;
        desc.usage = RHIBufferUsage::CopyDst;
        desc.memoryType = RHIMemoryType::Readback;
        desc.stride = stride;
        desc.debugName = debugName;
        return m_device->CreateBuffer(desc);
    };
    capture.visibilityReadback = createReadback(
        static_cast<uint64>(capture.activeRowCount) * sizeof(uint32),
        sizeof(uint32),
        "GPUCulling.GPUSceneQualificationVisibilityReadback");
    capture.visibleRowsReadback = createReadback(
        static_cast<uint64>(capture.activeRowCount) * sizeof(uint32),
        sizeof(uint32),
        "GPUCulling.GPUSceneQualificationVisibleRowsReadback");
    capture.drawCountsReadback = createReadback(
        static_cast<uint64>(capture.drawGroupCount + 1u) * sizeof(uint32),
        sizeof(uint32),
        "GPUCulling.GPUSceneQualificationDrawCountsReadback");
    capture.indirectCommandsReadback = createReadback(
        static_cast<uint64>(capture.drawGroupCount) *
            sizeof(IndirectDrawIndexedCommand),
        sizeof(IndirectDrawIndexedCommand),
        "GPUCulling.GPUSceneQualificationIndirectReadback");
    if (capturedTier == GPUDrivenTier::IndirectGrouped)
    {
        const GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
        if (inputs == nullptr || !inputs->instanceBuffer ||
            inputs->instanceBuffer->GetSize() == 0)
        {
            return false;
        }
        capture.rasterInstanceReadback = createReadback(
            inputs->instanceBuffer->GetSize(), sizeof(GPUInstanceData),
            "GPUCulling.GPUSceneQualificationRasterInstanceReadback");
    }
    if (!capture.IsAllocated())
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReadbackAllocationFailed);
        return false;
    }

    capture.referencePrepared = true;
    bool payloadSaturated = false;
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.cpuActiveInstances.size()) *
                      sizeof(GPUInstanceData),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.cpuActiveResidentRows.size()) *
                      sizeof(uint32),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.cpuActiveKeys.size()) *
                      sizeof(RasterInstanceStreamKey),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(
                      capture.cpuActiveRasterSemanticIdentities.size()) *
                      sizeof(uint64),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.expectedRasterInstances.size()) *
                      sizeof(GPUInstanceData),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.expectedVisibility.size()) *
                      sizeof(uint32),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.expectedVisibleResidentRows.size()) *
                      sizeof(uint32),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.expectedInstanceCounts.size() +
                                       capture.expectedDrawCounts.size()) *
                      sizeof(uint32),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.expectedIndirectCommands.size()) *
                      sizeof(IndirectDrawIndexedCommand),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(capture.drawGroups.size()) *
                      sizeof(GPUSceneQualificationCapture::DrawGroupTopology),
                  payloadSaturated);
    SaturatingAdd(capture.cpuPayloadBytes,
                  static_cast<uint64>(
                      capture.inputPlan.expectedGPUInputIdentityHashes.size() +
                      capture.inputPlan.expectedDirectVisibleIdentityHashes.size() +
                      capture.activeRowIdentityHashes.size()) *
                      sizeof(uint64),
                  payloadSaturated);
    m_gpuSceneQualificationCapture = std::move(capture);
    m_gpuSceneQualificationDiagnostics.readbackAllocated = true;
    m_gpuSceneQualificationDiagnostics.cpuPayloadBytes =
        m_gpuSceneQualificationCapture.cpuPayloadBytes;
    m_gpuSceneQualificationDiagnostics.mismatch =
        GPUSceneCullingQualificationMismatch::CopyNotRecorded;
    return true;
}

bool GPUCulling::FinalizeTierOneRasterSemanticEvidence(
    std::span<const uint64> fixedRasterMaterialKeysByGroup,
    std::span<const uint64> parameterTableRasterMaterialKeysBySlot,
    const RenderResourceRegistry& resourceRegistry)
{
    GPUSceneQualificationCapture& capture = m_gpuSceneQualificationCapture;
    // Only an explicitly armed Tier1 capture retains the diagnostic sidecar.
    // Ordinary seals and Tier2 do not acquire any semantic payload.
    if (!capture.referencePrepared ||
        capture.capturedTier != GPUDrivenTier::IndirectGrouped)
    {
        return true;
    }
    if (capture.rasterSemanticEvidenceFinalized ||
        fixedRasterMaterialKeysByGroup.size() != capture.drawGroupCount ||
        capture.cpuActiveInstances.size() != capture.activeRowCount ||
        capture.cpuActiveRasterSemanticIdentities.size() !=
            capture.activeRowCount ||
        capture.drawGroups.size() != capture.drawGroupCount)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    std::vector<uint64> finalized;
    try
    {
        finalized.resize(capture.activeRowCount, 0);
        for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount;
             ++activeIndex)
        {
            const GPUInstanceData& instance =
                capture.cpuActiveInstances[activeIndex];
            if (instance.drawGroupIndex >= capture.drawGroups.size())
            {
                SetGPUSceneQualificationFailure(
                    GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
                return false;
            }
            const GPUSceneQualificationCapture::DrawGroupTopology& group =
                capture.drawGroups[instance.drawGroupIndex];
            uint64 rasterMaterialKey = 0;
            if (group.materialParameterSlotConsumed)
            {
                if (instance.materialId >=
                    parameterTableRasterMaterialKeysBySlot.size())
                {
                    SetGPUSceneQualificationFailure(
                        GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
                    return false;
                }
                rasterMaterialKey =
                    parameterTableRasterMaterialKeysBySlot[instance.materialId];
            }
            else
            {
                rasterMaterialKey =
                    fixedRasterMaterialKeysByGroup[instance.drawGroupIndex];
            }
            const std::optional<uint64> combined =
                resourceRegistry.CombineRasterMeshAndMaterialSemanticIdentity(
                    group.mesh, rasterMaterialKey);
            if (!combined)
            {
                SetGPUSceneQualificationFailure(
                    GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
                return false;
            }
            finalized[activeIndex] = *combined;
        }
    }
    catch (...)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    capture.cpuActiveRasterSemanticIdentities = std::move(finalized);
    capture.rasterSemanticEvidenceFinalized = true;
    return true;
}

bool GPUCulling::BuildGPUSceneQualificationReference(
    const Mat4& viewMatrix,
    const Mat4& projectionMatrix)
{
    GPUSceneQualificationCapture& capture = m_gpuSceneQualificationCapture;
    if (!capture.referencePrepared ||
        capture.cpuActiveInstances.size() != capture.activeRowCount ||
        capture.cpuActiveResidentRows.size() != capture.activeRowCount ||
        capture.cpuActiveKeys.size() != capture.activeRowCount ||
        (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
         capture.cpuActiveRasterSemanticIdentities.size() !=
             capture.activeRowCount) ||
        capture.expectedVisibility.size() != capture.activeRowCount ||
        capture.expectedVisibleResidentRows.size() != capture.activeRowCount ||
        capture.expectedInstanceCounts.size() != capture.drawGroupCount ||
        capture.expectedDrawCounts.size() != capture.drawGroupCount ||
        capture.expectedIndirectCommands.size() != capture.drawGroupCount ||
        (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
         capture.expectedRasterInstances.size() != capture.activeRowCount) ||
        capture.drawGroups.size() != capture.drawGroupCount)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    std::fill(capture.expectedVisibility.begin(),
              capture.expectedVisibility.end(),
              0u);
    std::fill(capture.expectedVisibleResidentRows.begin(),
              capture.expectedVisibleResidentRows.end(),
              RVX_INVALID_INDEX);
    std::fill(capture.expectedInstanceCounts.begin(),
              capture.expectedInstanceCounts.end(),
              0u);
    std::fill(capture.expectedDrawCounts.begin(),
              capture.expectedDrawCounts.end(),
              0u);
    std::fill(capture.expectedIndirectCommands.begin(),
              capture.expectedIndirectCommands.end(),
              IndirectDrawIndexedCommand{});

    Vec4 frustumPlanes[6];
    ExtractFrustumPlanes(projectionMatrix * viewMatrix, frustumPlanes);
    const Vec3 cameraPosition = Vec3(inverse(viewMatrix)[3]);
    uint32 expectedVisibleCount = 0;
    for (uint32 activeIndex = 0;
         activeIndex < capture.activeRowCount;
         ++activeIndex)
    {
        const GPUInstanceData& instance = capture.cpuActiveInstances[activeIndex];
        if (instance.drawGroupIndex >= capture.drawGroupCount ||
            instance.indexCount == 0 ||
            capture.cpuActiveResidentRows[activeIndex] == RVX_INVALID_INDEX)
        {
            continue;
        }

        bool visible = true;
        if (m_config.enableFrustumCulling && instance.forceVisible == 0)
        {
            const Vec3 center(instance.boundingSphere.x,
                              instance.boundingSphere.y,
                              instance.boundingSphere.z);
            const Vec3 extent = glm::max(
                Vec3(instance.aabbMax - instance.aabbMin) * 0.5f,
                Vec3(0.0f));
            for (const Vec4& plane : frustumPlanes)
            {
                const Vec3 normal(plane.x, plane.y, plane.z);
                if (dot(normal, normal) <= 1.0e-12f)
                {
                    continue;
                }
                const float32 signedDistance = dot(normal, center) + plane.w;
                const float32 projectedRadius = dot(glm::abs(normal), extent);
                if (IsConservativelyOutsideFrustumPlane(
                        signedDistance, projectedRadius))
                {
                    visible = false;
                    break;
                }
            }
        }
        if (visible && instance.forceVisible == 0 &&
            m_config.enableDistanceCulling && m_config.maxDrawDistance > 0.0f)
        {
            const Vec3 center(instance.boundingSphere.x,
                              instance.boundingSphere.y,
                              instance.boundingSphere.z);
            visible = length(center - cameraPosition) -
                    std::max(instance.boundingSphere.w, 0.0f) <=
                m_config.maxDrawDistance;
        }
        if (!visible)
        {
            continue;
        }

        const uint32 groupIndex = instance.drawGroupIndex;
        const GPUSceneQualificationCapture::DrawGroupTopology& group =
            capture.drawGroups[groupIndex];
        if (group.commandOffset >= capture.drawGroupCount ||
            group.visibleInstanceOffset > capture.activeRowCount ||
            group.maxDrawCount >
                capture.activeRowCount - group.visibleInstanceOffset ||
            capture.expectedInstanceCounts[groupIndex] >= group.maxDrawCount)
        {
            SetGPUSceneQualificationFailure(
                GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
            return false;
        }
        const uint32 visibleIndex = group.visibleInstanceOffset +
            capture.expectedInstanceCounts[groupIndex];
        if (visibleIndex >= capture.expectedVisibleResidentRows.size() ||
            visibleIndex >= group.visibleInstanceOffset + group.maxDrawCount)
        {
            SetGPUSceneQualificationFailure(
                GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
            return false;
        }
        capture.expectedVisibility[activeIndex] = 1u;
        capture.expectedVisibleResidentRows[visibleIndex] =
            capture.cpuActiveResidentRows[activeIndex];
        ++capture.expectedInstanceCounts[groupIndex];
        ++expectedVisibleCount;
    }

    uint32 expectedSubmittedDrawCount = 0;
    for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount; ++groupIndex)
    {
        const uint32 visibleCount = capture.expectedInstanceCounts[groupIndex];
        capture.expectedDrawCounts[groupIndex] = visibleCount > 0 ? 1u : 0u;
        const GPUSceneQualificationCapture::DrawGroupTopology& group =
            capture.drawGroups[groupIndex];
        IndirectDrawIndexedCommand& command =
            capture.expectedIndirectCommands[groupIndex];
        command.indexCount = group.indexCount;
        command.instanceCount = visibleCount;
        command.firstIndex = group.firstIndex;
        command.vertexOffset = group.vertexOffset;
        command.firstInstance = group.visibleInstanceOffset;
        expectedSubmittedDrawCount += capture.expectedDrawCounts[groupIndex];
    }
    m_gpuSceneQualificationDiagnostics.expectedVisibleInstanceCount =
        expectedVisibleCount;
    m_gpuSceneQualificationDiagnostics.expectedSubmittedDrawCount =
        expectedSubmittedDrawCount;
    return true;
}

bool GPUCulling::RecordGPUSceneQualificationReadback(RHICommandContext& ctx)
{
    GPUSceneQualificationCapture& capture = m_gpuSceneQualificationCapture;
    if (!capture.referencePrepared)
    {
        return true;
    }
    if (!capture.IsAllocated() || m_visibilityBuffer == nullptr ||
        m_visibleInstanceBuffer == nullptr || m_indirectBuffer == nullptr ||
        m_drawCountBuffer == nullptr)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReadbackAllocationFailed);
        return false;
    }
    const bool capturesTierOneRasterPayload =
        capture.capturedTier == GPUDrivenTier::IndirectGrouped;
    const GPUCullingFrameInputs* inputs = capturesTierOneRasterPayload
        ? GetActiveFrameInputs()
        : nullptr;
    if (capturesTierOneRasterPayload &&
        (inputs == nullptr || !inputs->instanceBuffer ||
         !capture.rasterInstanceReadback ||
         inputs->instanceBuffer->GetSize() !=
             capture.rasterInstanceReadback->GetSize()))
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReadbackAllocationFailed);
        return false;
    }

    const RHIAccessSnapshot computeUAVAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const RHIAccessSnapshot copySourceAccess = MakeRHIAccessSnapshot(
        RHIResourceState::CopySource,
        RHIShaderStage::All,
        GPUQueueDomain::Graphics);
    const uint64 activeBytes =
        static_cast<uint64>(capture.activeRowCount) * sizeof(uint32);
    const uint64 drawCountBytes =
        static_cast<uint64>(capture.drawGroupCount + 1u) * sizeof(uint32);
    const uint64 indirectBytes =
        static_cast<uint64>(capture.drawGroupCount) *
        sizeof(IndirectDrawIndexedCommand);

    ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, copySourceAccess);
    ctx.BufferBarrier(m_visibleInstanceBuffer.Get(), computeUAVAccess, copySourceAccess);
    ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, copySourceAccess);
    ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, copySourceAccess);
    ctx.BufferBarrier(capture.visibilityReadback.Get(),
                      RHIResourceState::Undefined,
                      RHIResourceState::CopyDest);
    ctx.BufferBarrier(capture.visibleRowsReadback.Get(),
                      RHIResourceState::Undefined,
                      RHIResourceState::CopyDest);
    ctx.BufferBarrier(capture.drawCountsReadback.Get(),
                      RHIResourceState::Undefined,
                      RHIResourceState::CopyDest);
    ctx.BufferBarrier(capture.indirectCommandsReadback.Get(),
                      RHIResourceState::Undefined,
                      RHIResourceState::CopyDest);
    RHIAccessSnapshot rasterInputAccess{};
    if (capturesTierOneRasterPayload)
    {
        rasterInputAccess = inputs->instanceAccess.uniformAccess;
        const RHIAccessSnapshot rasterCopySourceAccess = MakeRHIAccessSnapshot(
            RHIResourceState::CopySource, RHIShaderStage::All,
            GPUQueueDomain::Graphics);
        ctx.BufferBarrier(inputs->instanceBuffer.Get(), rasterInputAccess,
                          rasterCopySourceAccess);
        ctx.BufferBarrier(capture.rasterInstanceReadback.Get(),
                          RHIResourceState::Undefined,
                          RHIResourceState::CopyDest);
    }
    ctx.CopyBuffer(m_visibilityBuffer.Get(), capture.visibilityReadback.Get(),
                   0, 0, activeBytes);
    ctx.CopyBuffer(m_visibleInstanceBuffer.Get(), capture.visibleRowsReadback.Get(),
                   0, 0, activeBytes);
    ctx.CopyBuffer(m_drawCountBuffer.Get(), capture.drawCountsReadback.Get(),
                   0, 0, drawCountBytes);
    ctx.CopyBuffer(m_indirectBuffer.Get(),
                   capture.indirectCommandsReadback.Get(),
                   0, 0, indirectBytes);
    if (capturesTierOneRasterPayload)
    {
        ctx.CopyBuffer(inputs->instanceBuffer.Get(),
                       capture.rasterInstanceReadback.Get(),
                       0, 0, inputs->instanceBuffer->GetSize());
        const RHIAccessSnapshot rasterCopySourceAccess = MakeRHIAccessSnapshot(
            RHIResourceState::CopySource, RHIShaderStage::All,
            GPUQueueDomain::Graphics);
        ctx.BufferBarrier(inputs->instanceBuffer.Get(), rasterCopySourceAccess,
                          rasterInputAccess);
    }
    // Preserve the RenderGraph-declared output state.  The readback buffers
    // are private to this explicitly armed capture and have no later graph use.
    ctx.BufferBarrier(m_visibilityBuffer.Get(), copySourceAccess, computeUAVAccess);
    ctx.BufferBarrier(m_visibleInstanceBuffer.Get(), copySourceAccess, computeUAVAccess);
    ctx.BufferBarrier(m_indirectBuffer.Get(), copySourceAccess, computeUAVAccess);
    ctx.BufferBarrier(m_drawCountBuffer.Get(), copySourceAccess, computeUAVAccess);
    capture.copyRecorded = true;
    m_gpuSceneQualificationDiagnostics.copyRecorded = true;
    m_gpuSceneQualificationDiagnostics.mismatch =
        GPUSceneCullingQualificationMismatch::CompletionPending;
    return true;
}

bool GPUCulling::AcceptGPUSceneQualificationSubmission(
    const GPUCullingRecordingIdentity& identity,
    GPUSceneQualificationCapture&& capture,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker)
{
    if (!capture.referencePrepared)
    {
        return true;
    }

    if (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
        !capture.rasterSemanticEvidenceFinalized)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    if (m_pendingGPUSceneQualificationCapture.submissionAccepted ||
        !capture.copyRecorded || !capture.IsAllocated() ||
        !capture.identity.IsValid() || capture.identity != identity ||
        !capture.required || !IsQualificationCaptureTier(capture.capturedTier) ||
        capture.activeRowVersion == 0 ||
        (capture.capturedTier == GPUDrivenTier::GPUResidentScene &&
         (capture.gpuSceneLeaseVersion == 0 || capture.candidateVersion == 0)) ||
        (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
         (capture.gpuSceneLeaseVersion != 0 || capture.candidateVersion != 0)))
    {
        SetGPUSceneQualificationFailure(
            capture.copyRecorded
                ? GPUSceneCullingQualificationMismatch::CompletionRejected
                : GPUSceneCullingQualificationMismatch::CopyNotRecorded);
        return false;
    }

    GPUCompletionPoint graphicsPoint;
    if (!IsExactCurrentGraphicsSubmissionToken(
            tracker, completion, graphicsPoint))
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::CompletionRejected);
        return false;
    }

    capture.completionPoint = graphicsPoint;
    capture.submissionAccepted = true;
    m_pendingGPUSceneQualificationCapture = std::move(capture);
    m_gpuSceneQualificationDiagnostics = {};
    m_gpuSceneQualificationDiagnostics.requested = true;
    m_gpuSceneQualificationDiagnostics.required = capture.required;
    m_gpuSceneQualificationDiagnostics.readbackAllocated = true;
    m_gpuSceneQualificationDiagnostics.copyRecorded = true;
    m_gpuSceneQualificationDiagnostics.submissionAccepted = true;
    m_gpuSceneQualificationDiagnostics.mismatch =
        GPUSceneCullingQualificationMismatch::CompletionPending;
    m_gpuSceneQualificationDiagnostics.capturedTier = capture.capturedTier;
    m_gpuSceneQualificationDiagnostics.activeRowCount =
        m_pendingGPUSceneQualificationCapture.activeRowCount;
    m_gpuSceneQualificationDiagnostics.drawGroupCount =
        m_pendingGPUSceneQualificationCapture.drawGroupCount;
    m_gpuSceneQualificationDiagnostics.expectedInputPacketCount =
        m_pendingGPUSceneQualificationCapture.inputPlan.expectedPacketCount;
    m_gpuSceneQualificationDiagnostics.expectedGPUInputPacketCount =
        m_pendingGPUSceneQualificationCapture.inputPlan
            .expectedGPUInputPacketCount;
    m_gpuSceneQualificationDiagnostics.observedGPUInputPacketCount =
        static_cast<uint32>(m_pendingGPUSceneQualificationCapture
                                 .activeRowIdentityHashes.size());
    m_gpuSceneQualificationDiagnostics.directInputPacketCount =
        m_pendingGPUSceneQualificationCapture.inputPlan.directPacketCount;
    m_gpuSceneQualificationDiagnostics.skippedInputPacketCount =
        m_pendingGPUSceneQualificationCapture.inputPlan.skippedPacketCount;
    m_gpuSceneQualificationDiagnostics.planPacketIdentityHash =
        m_pendingGPUSceneQualificationCapture.inputPlan.planPacketIdentityHash;
    m_gpuSceneQualificationDiagnostics.cpuPayloadBytes =
        m_pendingGPUSceneQualificationCapture.cpuPayloadBytes;
    m_gpuSceneQualificationDiagnostics.expectedVisibleInstanceCount =
        static_cast<uint32>(std::count_if(
            m_pendingGPUSceneQualificationCapture.expectedVisibility.begin(),
            m_pendingGPUSceneQualificationCapture.expectedVisibility.end(),
            [](uint32 visible) { return visible != 0u; }));
    m_gpuSceneQualificationDiagnostics.expectedSubmittedDrawCount =
        std::accumulate(
            m_pendingGPUSceneQualificationCapture.expectedDrawCounts.begin(),
            m_pendingGPUSceneQualificationCapture.expectedDrawCounts.end(),
            0u);
    m_gpuSceneQualificationDiagnostics.frameSequence =
        m_pendingGPUSceneQualificationCapture.identity.frameSequence;
    m_gpuSceneQualificationDiagnostics.recordEpoch =
        m_pendingGPUSceneQualificationCapture.identity.recordEpoch;
    m_gpuSceneQualificationDiagnostics.gpuSceneLeaseVersion =
        m_pendingGPUSceneQualificationCapture.gpuSceneLeaseVersion;
    m_gpuSceneQualificationDiagnostics.candidateVersion =
        m_pendingGPUSceneQualificationCapture.candidateVersion;
    m_gpuSceneQualificationDiagnostics.activeRowVersion =
        m_pendingGPUSceneQualificationCapture.activeRowVersion;
    m_gpuSceneQualificationDiagnostics.completionValue = graphicsPoint.value;
    return true;
}

bool GPUCulling::CompareGPUSceneQualificationInputCoverage(
    GPUSceneQualificationCapture& capture)
{
    GPUSceneCullingQualificationDiagnostics& diagnostics =
        m_gpuSceneQualificationDiagnostics;
    diagnostics.inputCoverageCompared = true;
    diagnostics.expectedInputPacketCount =
        capture.inputPlan.expectedPacketCount;
    diagnostics.expectedGPUInputPacketCount =
        capture.inputPlan.expectedGPUInputPacketCount;
    diagnostics.observedGPUInputPacketCount = static_cast<uint32>(
        capture.activeRowIdentityHashes.size());
    diagnostics.directInputPacketCount = capture.inputPlan.directPacketCount;
    diagnostics.skippedInputPacketCount = capture.inputPlan.skippedPacketCount;
    diagnostics.planPacketIdentityHash = capture.inputPlan.planPacketIdentityHash;

    const uint64 partitionCount =
        static_cast<uint64>(capture.inputPlan.expectedGPUInputPacketCount) +
        capture.inputPlan.directPacketCount + capture.inputPlan.skippedPacketCount;
    std::vector<uint64> expected =
        capture.inputPlan.expectedGPUInputIdentityHashes;
    std::vector<uint64> observed = capture.activeRowIdentityHashes;
    std::sort(expected.begin(), expected.end());
    std::sort(observed.begin(), observed.end());
    diagnostics.expectedGPUInputIdentityHash =
        HashQualificationIdentityList(expected);
    diagnostics.observedGPUInputIdentityHash =
        HashQualificationIdentityList(observed);

    const bool expectedUnique = std::adjacent_find(
        expected.begin(), expected.end()) == expected.end();
    const bool observedUnique = std::adjacent_find(
        observed.begin(), observed.end()) == observed.end();
    const bool matches = capture.inputPlan.exactlyOncePartitioned &&
        capture.inputPlan.expectedPacketCount != 0 &&
        capture.inputPlan.planPacketIdentityHash != 0 &&
        partitionCount == capture.inputPlan.expectedPacketCount &&
        expected.size() == capture.inputPlan.expectedGPUInputPacketCount &&
        observed.size() == capture.inputPlan.expectedGPUInputPacketCount &&
        expectedUnique && observedUnique && expected == observed;
    diagnostics.inputCoverageMatched = matches;
    if (!matches)
    {
        diagnostics.matched = false;
        diagnostics.mismatch = GPUSceneCullingQualificationMismatch::InputCoverage;
        diagnostics.firstMismatchActiveRow = RVX_INVALID_INDEX;
        diagnostics.firstMismatchDrawGroup = RVX_INVALID_INDEX;
        diagnostics.expectedValue =
            static_cast<uint32>(diagnostics.expectedGPUInputIdentityHash);
        diagnostics.observedValue =
            static_cast<uint32>(diagnostics.observedGPUInputIdentityHash);
    }
    return matches;
}

bool GPUCulling::CompareGPUSceneQualificationDirectVisibilityCoverage(
    GPUSceneQualificationCapture& capture,
    const uint32* visibility)
{
    GPUSceneCullingQualificationDiagnostics& diagnostics =
        m_gpuSceneQualificationDiagnostics;
    diagnostics.directVisibilityCoverageCompared = true;
    diagnostics.expectedDirectVisiblePacketCount = static_cast<uint32>(
        capture.inputPlan.expectedDirectVisibleIdentityHashes.size());

    std::vector<uint64> expected =
        capture.inputPlan.expectedDirectVisibleIdentityHashes;
    std::vector<uint64> observed;
    if (visibility != nullptr &&
        capture.activeRowIdentityHashes.size() == capture.activeRowCount)
    {
        observed.reserve(capture.activeRowCount);
        for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount;
             ++activeIndex)
        {
            if (visibility[activeIndex] != 0)
            {
                observed.push_back(
                    capture.activeRowIdentityHashes[activeIndex]);
            }
        }
    }
    std::sort(expected.begin(), expected.end());
    std::sort(observed.begin(), observed.end());
    diagnostics.observedGPUVisiblePacketCount =
        static_cast<uint32>(observed.size());
    diagnostics.expectedDirectVisibleIdentityHash =
        HashQualificationIdentityList(expected);
    diagnostics.observedGPUVisibleIdentityHash =
        HashQualificationIdentityList(observed);

    const bool expectedUnique = std::adjacent_find(
        expected.begin(), expected.end()) == expected.end();
    uint32 missing = 0;
    uint32 extras = 0;
    for (const uint64 identity : expected)
    {
        if (!std::binary_search(observed.begin(), observed.end(), identity))
        {
            ++missing;
        }
    }
    for (const uint64 identity : observed)
    {
        if (!std::binary_search(expected.begin(), expected.end(), identity))
        {
            ++extras;
        }
    }
    diagnostics.missingDirectVisiblePacketCount = missing;
    diagnostics.gpuOnlyVisiblePacketCount = extras;
    const bool matches = visibility != nullptr && expectedUnique && missing == 0;
    diagnostics.directVisibilityCoverageMatched = matches;
    if (!matches)
    {
        diagnostics.matched = false;
        if (diagnostics.mismatch == GPUSceneCullingQualificationMismatch::None ||
            diagnostics.mismatch ==
                GPUSceneCullingQualificationMismatch::CompletionPending)
        {
            diagnostics.mismatch =
                GPUSceneCullingQualificationMismatch::DirectVisibilityCoverage;
        }
        diagnostics.expectedValue = missing;
        diagnostics.observedValue = extras;
    }
    return matches;
}

bool GPUCulling::CompareGPUSceneQualificationReadback()
{
    GPUSceneQualificationCapture& capture = m_pendingGPUSceneQualificationCapture;
    const uint64 activeBytes =
        static_cast<uint64>(capture.activeRowCount) * sizeof(uint32);
    const uint64 drawCountBytes =
        static_cast<uint64>(capture.drawGroupCount + 1u) * sizeof(uint32);
    const uint64 indirectBytes =
        static_cast<uint64>(capture.drawGroupCount) *
        sizeof(IndirectDrawIndexedCommand);
    if (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
        !capture.rasterSemanticEvidenceFinalized)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }
    if (!capture.submissionAccepted || !capture.IsAllocated() ||
        capture.cpuActiveKeys.size() != capture.activeRowCount ||
        capture.expectedVisibility.size() != capture.activeRowCount ||
        capture.expectedVisibleResidentRows.size() != capture.activeRowCount ||
        capture.expectedInstanceCounts.size() != capture.drawGroupCount ||
        capture.expectedDrawCounts.size() != capture.drawGroupCount ||
        capture.expectedIndirectCommands.size() != capture.drawGroupCount ||
        (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
         capture.expectedRasterInstances.size() != capture.activeRowCount) ||
        capture.drawGroups.size() != capture.drawGroupCount ||
        capture.visibilityReadback->GetSize() < activeBytes ||
        capture.visibleRowsReadback->GetSize() < activeBytes ||
        capture.drawCountsReadback->GetSize() < drawCountBytes ||
        capture.indirectCommandsReadback->GetSize() < indirectBytes ||
        (capture.capturedTier == GPUDrivenTier::IndirectGrouped &&
         capture.rasterInstanceReadback->GetSize() <
             sizeof(GPUInstanceData)))
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
        return false;
    }

    for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount;
         ++groupIndex)
    {
        const GPUSceneQualificationCapture::DrawGroupTopology& group =
            capture.drawGroups[groupIndex];
        const uint32 expectedInstanceCount =
            capture.expectedInstanceCounts[groupIndex];
        if (group.commandOffset >= capture.drawGroupCount ||
            group.commandOffset + 1u > capture.drawGroupCount ||
            group.visibleInstanceOffset > capture.activeRowCount ||
            group.maxDrawCount == 0 ||
            group.maxDrawCount > capture.activeRowCount -
                group.visibleInstanceOffset ||
            expectedInstanceCount > group.maxDrawCount ||
            expectedInstanceCount > capture.activeRowCount -
                group.visibleInstanceOffset)
        {
            SetGPUSceneQualificationFailure(
                GPUSceneCullingQualificationMismatch::ReferenceUnavailable);
            return false;
        }
    }

    // Input coverage and cull outputs are deliberately reported separately.
    // Do not let an output match hide a missing, duplicate, or drifted packet
    // between the immutable frame plan and the GPU owner.
    const bool inputCoverageMatched =
        CompareGPUSceneQualificationInputCoverage(capture);

    void* visibilityMapped = capture.visibilityReadback->Map();
    void* visibleRowsMapped = capture.visibleRowsReadback->Map();
    void* drawCountsMapped = capture.drawCountsReadback->Map();
    void* indirectMapped = capture.indirectCommandsReadback->Map();
    const bool rasterPayloadRequired = capture.capturedTier ==
        GPUDrivenTier::IndirectGrouped;
    void* rasterInstancesMapped = rasterPayloadRequired
        ? capture.rasterInstanceReadback->Map()
        : nullptr;
    if (visibilityMapped == nullptr || visibleRowsMapped == nullptr ||
        drawCountsMapped == nullptr || indirectMapped == nullptr ||
        (rasterPayloadRequired && rasterInstancesMapped == nullptr))
    {
        if (visibilityMapped != nullptr)
        {
            capture.visibilityReadback->Unmap();
        }
        if (visibleRowsMapped != nullptr)
        {
            capture.visibleRowsReadback->Unmap();
        }
        if (drawCountsMapped != nullptr)
        {
            capture.drawCountsReadback->Unmap();
        }
        if (indirectMapped != nullptr)
        {
            capture.indirectCommandsReadback->Unmap();
        }
        if (rasterPayloadRequired && rasterInstancesMapped != nullptr)
        {
            capture.rasterInstanceReadback->Unmap();
        }
        m_gpuSceneQualificationDiagnostics.completionObserved = true;
        m_gpuSceneQualificationDiagnostics.cullOutputsCompared = false;
        m_gpuSceneQualificationDiagnostics.cullOutputsMatched = false;
        m_gpuSceneQualificationDiagnostics.indirectArgumentsCompared = false;
        m_gpuSceneQualificationDiagnostics.indirectArgumentsMatched = false;
        m_gpuSceneQualificationDiagnostics.rasterPayloadCompared = false;
        m_gpuSceneQualificationDiagnostics.rasterPayloadMatched = false;
        m_gpuSceneQualificationDiagnostics.compared = false;
        m_gpuSceneQualificationDiagnostics.matched = false;
        if (inputCoverageMatched)
        {
            m_gpuSceneQualificationDiagnostics.mismatch =
                GPUSceneCullingQualificationMismatch::ReadbackMapFailed;
        }
        return false;
    }

    const auto* visibility = static_cast<const uint32*>(visibilityMapped);
    const auto* visibleRows = static_cast<const uint32*>(visibleRowsMapped);
    const auto* drawCounts = static_cast<const uint32*>(drawCountsMapped);
    const auto* indirectCommands =
        static_cast<const IndirectDrawIndexedCommand*>(indirectMapped);
    const auto* rasterInstances = rasterPayloadRequired
        ? static_cast<const GPUInstanceData*>(rasterInstancesMapped)
        : nullptr;
    m_gpuSceneQualificationDiagnostics.completionObserved = true;
    m_gpuSceneQualificationDiagnostics.observedVisibleInstanceCount = 0;
    m_gpuSceneQualificationDiagnostics.observedSubmittedDrawCount =
        drawCounts[0];

    const bool directVisibilityCoverageMatched =
        CompareGPUSceneQualificationDirectVisibilityCoverage(capture, visibility);

    bool cullOutputsMatched = true;
    const auto fail = [this, &cullOutputsMatched, inputCoverageMatched,
                       directVisibilityCoverageMatched](
                          GPUSceneCullingQualificationMismatch mismatch,
                          uint32 activeRow,
                          uint32 drawGroup,
                          uint32 expected,
                          uint32 observed)
    {
        if (cullOutputsMatched && inputCoverageMatched &&
            directVisibilityCoverageMatched)
        {
            m_gpuSceneQualificationDiagnostics.mismatch = mismatch;
            m_gpuSceneQualificationDiagnostics.firstMismatchActiveRow = activeRow;
            m_gpuSceneQualificationDiagnostics.firstMismatchDrawGroup = drawGroup;
            m_gpuSceneQualificationDiagnostics.expectedValue = expected;
            m_gpuSceneQualificationDiagnostics.observedValue = observed;
        }
        cullOutputsMatched = false;
    };

    bool indirectArgumentsMatched = true;
    const auto failIndirectArguments =
        [this, &indirectArgumentsMatched, inputCoverageMatched,
         directVisibilityCoverageMatched, &cullOutputsMatched](
            uint32 drawGroup, uint32 expected, uint32 observed)
    {
        if (indirectArgumentsMatched && inputCoverageMatched &&
            directVisibilityCoverageMatched && cullOutputsMatched)
        {
            m_gpuSceneQualificationDiagnostics.mismatch =
                GPUSceneCullingQualificationMismatch::IndirectArguments;
            m_gpuSceneQualificationDiagnostics.firstMismatchActiveRow =
                RVX_INVALID_INDEX;
            m_gpuSceneQualificationDiagnostics.firstMismatchDrawGroup =
                drawGroup;
            m_gpuSceneQualificationDiagnostics.expectedValue = expected;
            m_gpuSceneQualificationDiagnostics.observedValue = observed;
        }
        indirectArgumentsMatched = false;
    };

    for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount; ++activeIndex)
    {
        const uint32 expected = capture.expectedVisibility[activeIndex];
        const uint32 observed = visibility[activeIndex];
        if (observed != 0)
        {
            ++m_gpuSceneQualificationDiagnostics.observedVisibleInstanceCount;
        }
        if (observed != expected)
        {
            fail(GPUSceneCullingQualificationMismatch::Visibility,
                 activeIndex,
                 RVX_INVALID_INDEX,
                 expected,
                 observed);
        }
    }

    for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount; ++groupIndex)
    {
        const GPUSceneQualificationCapture::DrawGroupTopology& group =
            capture.drawGroups[groupIndex];
        const uint32 expectedInstanceCount = capture.expectedInstanceCounts[groupIndex];
        const IndirectDrawIndexedCommand& expectedCommand =
            capture.expectedIndirectCommands[groupIndex];
        const IndirectDrawIndexedCommand& observedCommand =
            indirectCommands[group.commandOffset];
        if (observedCommand.indexCount != expectedCommand.indexCount)
        {
            failIndirectArguments(groupIndex, expectedCommand.indexCount,
                                  observedCommand.indexCount);
        }
        if (observedCommand.instanceCount != expectedCommand.instanceCount)
        {
            failIndirectArguments(groupIndex, expectedCommand.instanceCount,
                                  observedCommand.instanceCount);
        }
        if (observedCommand.firstIndex != expectedCommand.firstIndex)
        {
            failIndirectArguments(groupIndex, expectedCommand.firstIndex,
                                  observedCommand.firstIndex);
        }
        if (observedCommand.vertexOffset != expectedCommand.vertexOffset)
        {
            failIndirectArguments(
                groupIndex, static_cast<uint32>(expectedCommand.vertexOffset),
                static_cast<uint32>(observedCommand.vertexOffset));
        }
        if (observedCommand.firstInstance != expectedCommand.firstInstance)
        {
            failIndirectArguments(groupIndex, expectedCommand.firstInstance,
                                  observedCommand.firstInstance);
        }
        for (uint32 rowOffset = 0; rowOffset < expectedInstanceCount; ++rowOffset)
        {
            const uint32 visibleIndex = group.visibleInstanceOffset + rowOffset;
            const uint32 expected = capture.expectedVisibleResidentRows[visibleIndex];
            const uint32 observed = visibleRows[visibleIndex];
            if (observed != expected)
            {
                fail(GPUSceneCullingQualificationMismatch::CompactedResidentRows,
                     RVX_INVALID_INDEX,
                     groupIndex,
                     expected,
                     observed);
            }
        }
        const uint32 expectedDrawCount = capture.expectedDrawCounts[groupIndex];
        const uint32 observedDrawCount = drawCounts[group.commandOffset + 1u];
        if (observedDrawCount != expectedDrawCount)
        {
            fail(GPUSceneCullingQualificationMismatch::DrawCounts,
                 RVX_INVALID_INDEX,
                 groupIndex,
                 expectedDrawCount,
                 observedDrawCount);
        }
    }

    if (drawCounts[0] !=
        m_gpuSceneQualificationDiagnostics.expectedSubmittedDrawCount)
    {
        fail(GPUSceneCullingQualificationMismatch::DrawCounts,
             RVX_INVALID_INDEX,
             RVX_INVALID_INDEX,
             m_gpuSceneQualificationDiagnostics.expectedSubmittedDrawCount,
             drawCounts[0]);
    }

    m_gpuSceneQualificationDiagnostics.indirectArgumentsCompared = true;
    m_gpuSceneQualificationDiagnostics.indirectArgumentsMatched =
        indirectArgumentsMatched;
    uint64 expectedIndirectHash = 0xCBF29CE484222325ull;
    uint64 observedIndirectHash = 0xCBF29CE484222325ull;
    for (uint32 groupIndex = 0; groupIndex < capture.drawGroupCount;
         ++groupIndex)
    {
        expectedIndirectHash = MixQualificationIdentity(
            expectedIndirectHash,
            HashQualificationBytes(&capture.expectedIndirectCommands[groupIndex],
                                   sizeof(IndirectDrawIndexedCommand)));
        const IndirectDrawIndexedCommand& observed = indirectCommands[
            capture.drawGroups[groupIndex].commandOffset];
        observedIndirectHash = MixQualificationIdentity(
            observedIndirectHash,
            HashQualificationBytes(&observed,
                                   sizeof(IndirectDrawIndexedCommand)));
    }
    m_gpuSceneQualificationDiagnostics.expectedIndirectArgumentsHash =
        expectedIndirectHash;
    m_gpuSceneQualificationDiagnostics.observedIndirectArgumentsHash =
        observedIndirectHash;

    bool rasterPayloadMatched = true;
    if (rasterPayloadRequired)
    {
        m_gpuSceneQualificationDiagnostics.rasterPayloadCompared = true;
        uint64 expectedHash = 0xCBF29CE484222325ull;
        uint64 observedHash = 0xCBF29CE484222325ull;
        for (uint32 activeIndex = 0; activeIndex < capture.activeRowCount;
             ++activeIndex)
        {
            const uint32 residentRow = capture.cpuActiveResidentRows[activeIndex];
            if (residentRow >= capture.rasterInstanceReadback->GetSize() /
                                   sizeof(GPUInstanceData))
            {
                rasterPayloadMatched = false;
                if (inputCoverageMatched && directVisibilityCoverageMatched &&
                    cullOutputsMatched && indirectArgumentsMatched)
                {
                    m_gpuSceneQualificationDiagnostics.mismatch =
                        GPUSceneCullingQualificationMismatch::RasterPayload;
                    m_gpuSceneQualificationDiagnostics.firstMismatchResidentRow =
                        residentRow;
                }
                continue;
            }
            const GPUInstanceData& expected =
                capture.expectedRasterInstances[activeIndex];
            const GPUInstanceData& observed = rasterInstances[residentRow];
            expectedHash = MixQualificationIdentity(
                expectedHash, HashQualificationBytes(&expected, sizeof(expected)));
            observedHash = MixQualificationIdentity(
                observedHash, HashQualificationBytes(&observed, sizeof(observed)));
            if (rasterPayloadMatched &&
                std::memcmp(&expected, &observed, sizeof(GPUInstanceData)) != 0)
            {
                if (inputCoverageMatched && directVisibilityCoverageMatched &&
                    cullOutputsMatched && indirectArgumentsMatched)
                {
                    m_gpuSceneQualificationDiagnostics.mismatch =
                        GPUSceneCullingQualificationMismatch::RasterPayload;
                    m_gpuSceneQualificationDiagnostics.firstMismatchResidentRow =
                        residentRow;
                }
                rasterPayloadMatched = false;
            }
        }
        m_gpuSceneQualificationDiagnostics.expectedRasterPayloadHash =
            expectedHash;
        m_gpuSceneQualificationDiagnostics.observedRasterPayloadHash =
            observedHash;
        m_gpuSceneQualificationDiagnostics.rasterPayloadMatched =
            rasterPayloadMatched;
    }

    if (rasterPayloadRequired)
    {
        // Reconstruct the submitted raster order from the *observed* final
        // indirect commands and visible-row buffer.  Unlike the earlier
        // integrity check, this is deliberately group/firstInstance ordered
        // and resolves every row through the frozen object/submesh key.
        RasterTranscriptDigest expectedTranscript;
        RasterTranscriptDigest observedTranscript;
        BeginRasterTranscript(expectedTranscript);
        BeginRasterTranscript(observedTranscript);
        bool transcriptValid = true;
        bool transcriptMatched = true;
        uint32 transcriptEntry = 0;
        std::unordered_map<uint32, uint32> activeIndexByResidentRow;
        try
        {
            activeIndexByResidentRow.reserve(capture.activeRowCount);
            for (uint32 activeIndex = 0;
                 activeIndex < capture.activeRowCount;
                 ++activeIndex)
            {
                if (!activeIndexByResidentRow.emplace(
                        capture.cpuActiveResidentRows[activeIndex],
                        activeIndex).second)
                {
                    transcriptValid = false;
                    break;
                }
            }
        }
        catch (const std::bad_alloc&)
        {
            transcriptValid = false;
        }
        const auto appendEntry = [&capture, rasterInstances,
                                  &activeIndexByResidentRow](
                                     RasterTranscriptDigest& transcript,
                                     const GPUSceneQualificationCapture::DrawGroupTopology& group,
                                     uint32 residentRow,
                                     bool observedPayload,
                                     uint32 indexCount,
                                     uint32 firstIndex,
                                     int32 vertexOffset,
                                     RasterTranscriptEntryDigest& outEntry) -> bool
        {
            const auto activeIndex = activeIndexByResidentRow.find(residentRow);
            if (activeIndex == activeIndexByResidentRow.end())
            {
                return false;
            }
            const GPUInstanceData& payload = observedPayload
                ? rasterInstances[residentRow]
                : capture.expectedRasterInstances[activeIndex->second];
            outEntry = MakeRasterTranscriptEntryDigest(
                group.rasterTranscriptGroupIdentity,
                capture.cpuActiveKeys[activeIndex->second],
                payload,
                capture.cpuActiveRasterSemanticIdentities[activeIndex->second],
                group.materialParameterSlotConsumed,
                indexCount,
                firstIndex,
                vertexOffset);
            AppendRasterTranscriptEntry(transcript, outEntry);
            return transcript.available;
        };
        const auto noteTranscriptMismatch = [this, &transcriptMatched](
                                           uint32 entry,
                                           const RasterTranscriptEntryDigest* expected,
                                           const RasterTranscriptEntryDigest* observed)
        {
            if (transcriptMatched)
            {
                m_gpuSceneQualificationDiagnostics
                    .firstRasterTranscriptMismatchEntry = entry;
                m_gpuSceneQualificationDiagnostics
                    .expectedRasterTranscriptIdentityHash =
                    expected != nullptr ? expected->identityHash : 0;
                m_gpuSceneQualificationDiagnostics
                    .observedRasterTranscriptIdentityHash =
                    observed != nullptr ? observed->identityHash : 0;
                m_gpuSceneQualificationDiagnostics
                    .expectedRasterTranscriptPayloadHash =
                    expected != nullptr ? expected->consumedPayloadHash : 0;
                m_gpuSceneQualificationDiagnostics
                    .observedRasterTranscriptPayloadHash =
                    observed != nullptr ? observed->consumedPayloadHash : 0;
            }
            transcriptMatched = false;
        };

        for (uint32 groupIndex = 0;
             groupIndex < capture.drawGroupCount && transcriptValid;
             ++groupIndex)
        {
            const GPUSceneQualificationCapture::DrawGroupTopology& group =
                capture.drawGroups[groupIndex];
            const IndirectDrawIndexedCommand& expectedCommand =
                capture.expectedIndirectCommands[groupIndex];
            const IndirectDrawIndexedCommand& observedCommand =
                indirectCommands[group.commandOffset];
            const auto commandRangeValid = [activeCount = capture.activeRowCount](
                                               const IndirectDrawIndexedCommand& command)
            {
                return command.firstInstance <= activeCount &&
                    command.instanceCount <= activeCount - command.firstInstance;
            };
            if (!commandRangeValid(expectedCommand) ||
                !commandRangeValid(observedCommand))
            {
                transcriptValid = false;
                noteTranscriptMismatch(transcriptEntry, nullptr, nullptr);
                break;
            }

            const uint32 entryCount = std::max(expectedCommand.instanceCount,
                                                observedCommand.instanceCount);
            for (uint32 instanceOffset = 0;
                 instanceOffset < entryCount;
                 ++instanceOffset, ++transcriptEntry)
            {
                RasterTranscriptEntryDigest expectedEntry{};
                RasterTranscriptEntryDigest observedEntry{};
                const bool hasExpected =
                    instanceOffset < expectedCommand.instanceCount;
                const bool hasObserved =
                    instanceOffset < observedCommand.instanceCount;
                bool expectedAppended = false;
                bool observedAppended = false;
                if (hasExpected)
                {
                    const uint32 visibleIndex = expectedCommand.firstInstance +
                        instanceOffset;
                    const uint32 residentRow =
                        capture.expectedVisibleResidentRows[visibleIndex];
                    expectedAppended = residentRow != RVX_INVALID_INDEX &&
                        appendEntry(expectedTranscript, group, residentRow, false,
                                    expectedCommand.indexCount,
                                    expectedCommand.firstIndex,
                                    expectedCommand.vertexOffset,
                                    expectedEntry);
                }
                if (hasObserved)
                {
                    const uint32 visibleIndex = observedCommand.firstInstance +
                        instanceOffset;
                    const uint32 residentRow = visibleRows[visibleIndex];
                    observedAppended = residentRow != RVX_INVALID_INDEX &&
                        residentRow < capture.rasterInstanceReadback->GetSize() /
                            sizeof(GPUInstanceData) &&
                        appendEntry(observedTranscript, group, residentRow, true,
                                    observedCommand.indexCount,
                                    observedCommand.firstIndex,
                                    observedCommand.vertexOffset,
                                    observedEntry);
                }
                if ((hasExpected && !expectedAppended) ||
                    (hasObserved && !observedAppended))
                {
                    transcriptValid = false;
                }
                if (!hasExpected || !hasObserved || !expectedAppended ||
                    !observedAppended ||
                    expectedEntry.identityHash != observedEntry.identityHash ||
                    expectedEntry.consumedPayloadHash !=
                        observedEntry.consumedPayloadHash)
                {
                    noteTranscriptMismatch(
                        transcriptEntry,
                        expectedAppended ? &expectedEntry : nullptr,
                        observedAppended ? &observedEntry : nullptr);
                }
                if (!transcriptValid)
                {
                    break;
                }
            }
        }
        m_gpuSceneQualificationDiagnostics.tierOneRasterTranscriptReference =
            expectedTranscript;
        m_gpuSceneQualificationDiagnostics.tierOneRasterTranscript =
            transcriptValid ? observedTranscript : RasterTranscriptDigest{};
        m_gpuSceneQualificationDiagnostics.tierOneRasterTranscriptCompared = true;
        m_gpuSceneQualificationDiagnostics.tierOneRasterTranscriptMatched =
            transcriptValid && transcriptMatched &&
            expectedTranscript.entryCount == observedTranscript.entryCount &&
            expectedTranscript.orderedIdentityHash ==
                observedTranscript.orderedIdentityHash &&
            expectedTranscript.consumedPayloadHash ==
                observedTranscript.consumedPayloadHash &&
            expectedTranscript.unorderedIdentityHash ==
                observedTranscript.unorderedIdentityHash &&
            expectedTranscript.unorderedIdentityHashSecondary ==
                observedTranscript.unorderedIdentityHashSecondary &&
            expectedTranscript.unorderedConsumedPayloadHash ==
                observedTranscript.unorderedConsumedPayloadHash &&
            expectedTranscript.unorderedConsumedPayloadHashSecondary ==
                observedTranscript.unorderedConsumedPayloadHashSecondary;
    }

    capture.visibilityReadback->Unmap();
    capture.visibleRowsReadback->Unmap();
    capture.drawCountsReadback->Unmap();
    capture.indirectCommandsReadback->Unmap();
    if (rasterPayloadRequired)
    {
        capture.rasterInstanceReadback->Unmap();
    }
    m_gpuSceneQualificationDiagnostics.cullOutputsCompared = true;
    m_gpuSceneQualificationDiagnostics.cullOutputsMatched = cullOutputsMatched;
    m_gpuSceneQualificationDiagnostics.compared =
        m_gpuSceneQualificationDiagnostics.inputCoverageCompared &&
        m_gpuSceneQualificationDiagnostics.directVisibilityCoverageCompared &&
        m_gpuSceneQualificationDiagnostics.cullOutputsCompared &&
        m_gpuSceneQualificationDiagnostics.indirectArgumentsCompared &&
        (!rasterPayloadRequired ||
         m_gpuSceneQualificationDiagnostics.rasterPayloadCompared);
    m_gpuSceneQualificationDiagnostics.matched = inputCoverageMatched &&
        directVisibilityCoverageMatched && cullOutputsMatched &&
        indirectArgumentsMatched &&
        (!rasterPayloadRequired || rasterPayloadMatched);
    if (m_gpuSceneQualificationDiagnostics.matched)
    {
        m_gpuSceneQualificationDiagnostics.mismatch =
            GPUSceneCullingQualificationMismatch::None;
    }
    return m_gpuSceneQualificationDiagnostics.matched;
}

bool GPUCulling::CompleteGPUSceneQualificationCapture(
    uint32 frameSlot,
    const RenderSubmissionTracker& tracker)
{
    GPUSceneQualificationCapture& capture = m_pendingGPUSceneQualificationCapture;
    if (!capture.submissionAccepted || capture.sourceFrameSlot != frameSlot)
    {
        return true;
    }

    const GPUCompletionStatus status = tracker.Query(capture.completionPoint);
    if (status == GPUCompletionStatus::Pending)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::CompletionPending);
        return false;
    }
    if (status == GPUCompletionStatus::Lost)
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::CompletionLost);
        m_pendingGPUSceneQualificationCapture = {};
        return false;
    }
    if (!IsCompletionSatisfied(status))
    {
        SetGPUSceneQualificationFailure(
            GPUSceneCullingQualificationMismatch::CompletionPending);
        return false;
    }

    const bool matched = CompareGPUSceneQualificationReadback();
    m_pendingGPUSceneQualificationCapture = {};
    return matched;
}

bool GPUCulling::PollGPUSceneQualificationCapture(
    const RenderSubmissionTracker& tracker)
{
    const GPUSceneQualificationCapture& capture =
        m_pendingGPUSceneQualificationCapture;
    if (!capture.submissionAccepted)
    {
        return true;
    }
    // Provenance remains bound to the source slot stored in the accepted
    // capture.  The Render owner need not reuse that slot or publish another
    // frame merely to observe the already-submitted fence.
    return CompleteGPUSceneQualificationCapture(capture.sourceFrameSlot,
                                                tracker);
}

bool GPUCulling::HasPendingGPUSceneQualificationCapture() const noexcept
{
    return m_pendingGPUSceneQualificationCapture.submissionAccepted;
}

GPUCullingExecutionDecision GPUCulling::GetExecutionDecision() const
{
    return EvaluateGpuExecution(true);
}

std::shared_ptr<GPUCullingRecordedState> GPUCulling::CreateRecordedState(
    const GPUCullingRecordingIdentity& identity,
    bool includeTierOneInstances,
    bool includeGPUSceneCandidates) const
{
    const GPUCullingFrameInputs* sourceInputs = GetActiveFrameInputs();
    if (!identity.IsValid() || m_device == nullptr ||
        sourceInputs == nullptr ||
        !sourceInputs->constantsBuffer ||
        !sourceInputs->instanceIndexBuffer ||
        !sourceInputs->visibilityBuffer ||
        !sourceInputs->visibleInstanceBuffer ||
        !sourceInputs->indirectBuffer ||
        !sourceInputs->drawCountBuffer ||
        !IsActiveRowsResident(*sourceInputs) ||
        (includeTierOneInstances &&
         (!sourceInputs->instanceBuffer || !IsInstanceResident(*sourceInputs))) ||
        (includeGPUSceneCandidates &&
         (!sourceInputs->gpuSceneCandidateBuffer ||
          !sourceInputs->gpuSceneDescriptorSet ||
          !IsGPUSceneCandidateResident(*sourceInputs))))
    {
        RVX_RENDER_WARN("GPUCulling: rejected an invalid graph-recording seal");
        return nullptr;
    }

    auto recordedState = std::make_shared<GPUCullingRecordedState>();
    GPUCulling& sealed = recordedState->m_culling;
    sealed.m_device = m_device;
    sealed.m_config = m_config;
    sealed.m_occlusionRequested = m_occlusionRequested;
    sealed.m_statsEnabled = m_statsEnabled;
    // A graph recording is deliberately GPU-only: copying m_instances,
    // m_gpuSceneCandidates, canonical rows, or CPU cull outputs here turns a
    // static 100K scene into an O(N) per-pass CPU operation. The seal keeps
    // only the RHI snapshot plus the small draw topology needed to finalize
    // indirect commands and bind raster work.
    sealed.m_recordingGpuOnly = true;
    GPUCullingFrameInputs sealedInputs = *sourceInputs;
    if (!includeTierOneInstances)
    {
        sealedInputs.instanceBuffer.Reset();
        // The normal descriptor set strongly owns its Tier 1 instance
        // binding.  A Tier 2 recording has no legal use for that binding and
        // must not retain it through the sealed snapshot.
        sealedInputs.descriptorSet.Reset();
        sealedInputs.instanceAccess = {};
        sealedInputs.instanceDesiredVersion = 0;
        sealedInputs.instanceResidentVersion = 0;
        sealedInputs.accessSnapshots.instances = {};
    }
    if (!includeGPUSceneCandidates)
    {
        sealedInputs.gpuSceneCandidateBuffer.Reset();
        sealedInputs.gpuSceneDescriptorSet.Reset();
        sealedInputs.gpuSceneCandidateAccess = {};
        sealedInputs.gpuSceneCandidateDesiredVersion = 0;
        sealedInputs.gpuSceneCandidateResidentVersion = 0;
        sealedInputs.accessSnapshots.gpuSceneCandidates = {};
        sealedInputs.gpuSceneTableBuffers = {};
        sealedInputs.gpuSceneTableCapacities = {};
        sealedInputs.gpuSceneLeaseVersion = 0;
    }
    sealed.m_frameInputs.push_back(std::move(sealedInputs));
    sealed.m_activeFrameSlot = 0;

    // Pipelines/layouts and the waited frame-slot resources are immutable for
    // the lifetime of this recording.  Strong references prevent teardown;
    // the RenderContext slot fence prevents reuse before GPU completion.
    sealed.m_frustumCullShader = m_frustumCullShader;
    sealed.m_compactShader = m_compactShader;
    sealed.m_finalizeShader = m_finalizeShader;
    sealed.m_cullingDescriptorSetLayout = m_cullingDescriptorSetLayout;
    sealed.m_cullingPipelineLayout = m_cullingPipelineLayout;
    sealed.m_frustumCullPipeline = m_frustumCullPipeline;
    sealed.m_occlusionCullPipeline = m_occlusionCullPipeline;
    sealed.m_compactPipeline = m_compactPipeline;
    sealed.m_finalizePipeline = m_finalizePipeline;
    sealed.m_pipelineFallbackReason = m_pipelineFallbackReason;

    sealed.m_gpuSceneCandidateVersion = m_gpuSceneCandidateVersion;
    sealed.m_gpuSceneCandidateCount = m_gpuSceneCandidateCount;
    sealed.m_finalizedInstanceVersion = m_finalizedInstanceVersion;
    sealed.m_finalizedGPUSceneCandidateVersion =
        m_finalizedGPUSceneCandidateVersion;
    sealed.m_finalizedActiveRowVersion = m_finalizedActiveRowVersion;
    // This explicit capture-only payload is copied into the immutable graph
    // recording. Normal seals retain neither plan identities nor dense CPU
    // collections.
    sealed.m_gpuSceneQualificationInputPlan =
        m_gpuSceneQualificationInputPlan;
    sealed.m_gpuSceneQualificationObservedInputIdentityHashes =
        m_gpuSceneQualificationObservedInputIdentityHashes;
    sealed.m_gpuSceneQualificationDiagnostics =
        m_gpuSceneQualificationDiagnostics;
    sealed.m_drawGroups = m_drawGroups;
    // The actual raster transcript needs the frozen object/submesh identity
    // for each active resident row.  Retain that dense CPU payload only for
    // the explicit capture that is about to be prepared on this sealed
    // recording; normal graph seals remain GPU-only.
    if (m_gpuSceneQualificationArmed)
    {
        sealed.m_collectedKeys = m_collectedKeys;
        if (includeTierOneInstances)
        {
            sealed.m_collectedRasterSemanticIdentities =
                m_collectedRasterSemanticIdentities;
        }
    }
    sealed.m_instanceCount = m_instanceCount;
    // Indirect output bytes already live in the sealed GPU buffers. Preserve
    // only their O(1)/O(group) publication metadata so a caller that chose a
    // CPU cull before graph sealing can submit those exact bytes. This is not
    // a CPU fallback after sealing and does not retain any dense collection,
    // canonical, visibility, or command payload.
    sealed.m_drawCount = m_drawCount;
    sealed.m_usedCpuFallbackLastCull = m_usedCpuFallbackLastCull;
    sealed.m_usedGpuExecutionLastCull = m_usedGpuExecutionLastCull;
    sealed.m_lastFallbackReason = m_lastFallbackReason;
    sealed.m_activeDrawGroupIndex = m_activeDrawGroupIndex;
    sealed.m_incrementalDiagnostics = m_incrementalDiagnostics;
    sealed.RefreshActiveInputAccessSnapshots();

    recordedState->m_identity = identity;
    recordedState->m_sourceFrameSlot = m_activeFrameSlot;
    return recordedState;
}

std::shared_ptr<GPUCullingRecordedState> GPUCulling::SealForGraph(
    const GPUCullingRecordingIdentity& identity)
{
    if (!EnsureInstanceResidency())
    {
        RVX_RENDER_WARN(
            "GPUCulling: rejected a Tier 1 seal without resident instance data");
        return nullptr;
    }
    std::shared_ptr<GPUCullingRecordedState> recordedState =
        CreateRecordedState(identity, true, false);
    if (!recordedState)
    {
        return nullptr;
    }

    if (m_gpuSceneQualificationArmed)
    {
        m_gpuSceneQualificationArmed = false;
        GPUCulling& sealed = recordedState->m_culling;
        const bool prepared = sealed.PrepareGPUSceneQualificationCapture(
            identity,
            GPUDrivenTier::IndirectGrouped,
            0,
            m_activeFrameSlot,
            std::span<const GPUInstanceData>(m_instances),
            std::span<const GPUCullingActiveRow>(
                m_canonicalActiveRows.data(), m_instances.size()),
            std::span<const uint32>(m_collectedCanonicalRows),
            std::span<const uint64>(m_collectedRasterSemanticIdentities));
        m_gpuSceneQualificationDiagnostics =
            sealed.m_gpuSceneQualificationDiagnostics;
        if (!prepared)
        {
            sealed.m_gpuSceneQualificationCapture = {};
        }
    }
    return recordedState;
}

std::shared_ptr<GPUCullingRecordedState> GPUCulling::SealForGPUSceneGraph(
    const GPUCullingRecordingIdentity& identity,
    const GPUSceneResidentGraphLease& lease)
{
    if (!ConfigureGPUSceneRecording(lease))
    {
        return nullptr;
    }

    // Tier 2 consumes only the candidate/table snapshot.  In particular, it
    // must not make Tier 1 residency a prerequisite or upload its unrelated
    // GPUInstanceData stream.
    std::shared_ptr<GPUCullingRecordedState> recordedState =
        CreateRecordedState(identity, false, true);
    if (!recordedState)
    {
        return nullptr;
    }

    GPUCulling& sealed = recordedState->m_culling;
    sealed.m_gpuSceneFrustumCullShader = m_gpuSceneFrustumCullShader;
    sealed.m_gpuSceneCompactShader = m_gpuSceneCompactShader;
    sealed.m_gpuSceneFinalizeShader = m_gpuSceneFinalizeShader;
    sealed.m_gpuSceneDescriptorSetLayout = m_gpuSceneDescriptorSetLayout;
    sealed.m_gpuScenePipelineLayout = m_gpuScenePipelineLayout;
    sealed.m_gpuSceneFrustumCullPipeline = m_gpuSceneFrustumCullPipeline;
    sealed.m_gpuSceneCompactPipeline = m_gpuSceneCompactPipeline;
    sealed.m_gpuSceneFinalizePipeline = m_gpuSceneFinalizePipeline;
    if (m_gpuSceneQualificationArmed)
    {
        // The explicit request is consumed by this exact immutable seal.  It
        // cannot leak into a retry/new frame and allocate readback resources
        // on an unrelated GPU-scene lease.
        m_gpuSceneQualificationArmed = false;
        const bool prepared = sealed.PrepareGPUSceneQualificationCapture(
            identity,
            GPUDrivenTier::GPUResidentScene,
            lease.version,
            m_activeFrameSlot,
            std::span<const GPUInstanceData>(m_instances),
            std::span<const GPUCullingActiveRow>(
                m_canonicalActiveRows.data(), m_instances.size()),
            std::span<const uint32>(m_collectedCanonicalRows),
            {});
        m_gpuSceneQualificationDiagnostics =
            sealed.m_gpuSceneQualificationDiagnostics;
        if (!prepared)
        {
            // Qualification is fail-closed in diagnostics, but an explicitly
            // requested debug readback must never alter the submitted scene.
            // The caller sees ReferenceUnavailable or allocation failure and
            // receives no false success state.
            sealed.m_gpuSceneQualificationCapture = {};
        }
    }
    return recordedState;
}

void GPUCulling::BeginFrame()
{
    BeginFrame(0, {}, {}, true);
}

void GPUCulling::BeginFrame(uint64 sceneRevision,
                            std::span<const uint64> changedObjectIds,
                            std::span<const uint64> removedObjectIds,
                            bool fullMutation)
{
    InvalidateFrameSnapshot();
    m_instances.clear();
    m_gpuSceneCandidates.clear();
    m_collectedKeys.clear();
    m_collectedRasterSemanticIdentities.clear();
    m_collectedSourceRevisions.clear();
    m_collectedCanonicalRows.clear();
    m_collectedRequiresComparison.clear();
    m_collectedInputByKey.clear();
    m_changedObjectIds.clear();
    m_removedObjectIds.clear();
    try
    {
        m_changedObjectIds.insert(changedObjectIds.begin(),
                                  changedObjectIds.end());
        m_removedObjectIds.insert(removedObjectIds.begin(),
                                  removedObjectIds.end());
    }
    catch (...)
    {
        // A partially copied mutation journal must never be interpreted as a
        // sparse guarantee. The full path is slower but remains correct.
        m_changedObjectIds.clear();
        m_removedObjectIds.clear();
        fullMutation = true;
    }
    ++m_collectionEpoch;
    if (m_collectionEpoch == 0)
    {
        // Epoch wrap is intentionally a full discontinuity, not a stale-row
        // alias. Clear seen marks before using epoch one again.
        std::fill(m_canonicalLastSeenEpoch.begin(),
                  m_canonicalLastSeenEpoch.end(), 0);
        ++m_collectionEpoch;
        fullMutation = true;
    }
    m_collectedSceneRevision = sceneRevision;
    m_fullCanonicalMutation = fullMutation;
    m_gpuSceneCandidateVersion = 0;
    m_gpuSceneCandidateCount = 0;
    m_visibleInstanceIndices.clear();
    m_rasterVisibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    m_groupDrawCounts.clear();
    m_drawGroups.clear();
    m_instanceCount = 0;
    m_drawCount = 0;
    ResetFrameUploadDiagnostics();
    m_activeDrawGroupIndex = RVX_INVALID_INDEX;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats = {};
    m_incrementalDiagnostics.activeRowCount = 0;
    m_incrementalDiagnostics.activeRowHighWatermark = 0;
    m_incrementalDiagnostics.instancePatchedRowCount = 0;
    m_incrementalDiagnostics.candidatePatchedRowCount = 0;
    m_incrementalDiagnostics.activeRowPatchedRowCount = 0;
    m_incrementalDiagnostics.activeRowUploadBytes = 0;
}

void GPUCulling::ResetFrameUploadDiagnostics() noexcept
{
    m_lastInstanceUploadBytes = 0;
    m_lastGPUSceneCandidateUploadBytes = 0;
    m_incrementalDiagnostics.activeRowUploadBytes = 0;
    m_incrementalDiagnostics.instanceUploadWork = {};
    m_incrementalDiagnostics.candidateUploadWork = {};
    m_incrementalDiagnostics.activeRowUploadWork = {};
}

void GPUCulling::SetStableRowPreparationFailureCountdownForTesting(
    int32 countdown) noexcept
{
    m_stableRowPreparationFailureCountdown = countdown;
}

void GPUCulling::SetDirtyJournalFailureCountdownForTesting(
    int32 countdown) noexcept
{
    m_dirtyJournalFailureCountdown = countdown;
}

void GPUCulling::FailStableRowPreparationCheckpoint()
{
    if (m_stableRowPreparationFailureCountdown == 0)
    {
        throw std::bad_alloc();
    }
    if (m_stableRowPreparationFailureCountdown > 0)
    {
        --m_stableRowPreparationFailureCountdown;
    }
}

void GPUCulling::FailDirtyJournalCheckpoint()
{
    if (m_dirtyJournalFailureCountdown == 0)
    {
        throw std::bad_alloc();
    }
    if (m_dirtyJournalFailureCountdown > 0)
    {
        --m_dirtyJournalFailureCountdown;
    }
}

uint32 GPUCulling::BeginDrawGroup(uint64 meshId,
                                  uint64 materialId,
                                  MaterialPipelineVariant pipelineVariant,
                                  RenderResourceHandle mesh,
                                  RenderResourceHandle material,
                                  RenderDrawGroupKey batchKey)
{
    InvalidateFrameSnapshot();
    if (m_drawGroups.size() >= m_config.maxInstances)
    {
        return RVX_INVALID_INDEX;
    }

    GPUCullingDrawGroup group;
    group.mesh = mesh;
    group.material = material;
    group.meshId = meshId;
    group.materialId = materialId;
    group.pipelineVariant = pipelineVariant;
    group.batchKey = std::move(batchKey);
    const uint32 groupIndex = static_cast<uint32>(m_drawGroups.size());
    group.commandOffset = groupIndex;
    group.visibleInstanceOffset = m_instanceCount;
    group.countBufferOffset = static_cast<uint32>((groupIndex + 1) * sizeof(uint32));
    m_drawGroups.push_back(std::move(group));
    m_groupDrawCounts.push_back(0);
    m_activeDrawGroupIndex = groupIndex;
    return groupIndex;
}

void GPUCulling::EndDrawGroup()
{
    InvalidateFrameSnapshot();
    m_activeDrawGroupIndex = RVX_INVALID_INDEX;
}

uint32 GPUCulling::EnsureDefaultDrawGroup()
{
    if (m_activeDrawGroupIndex != RVX_INVALID_INDEX)
    {
        return m_activeDrawGroupIndex;
    }

    if (m_drawGroups.empty())
    {
        const uint32 groupIndex = BeginDrawGroup(0);
        m_activeDrawGroupIndex = RVX_INVALID_INDEX;
        return groupIndex;
    }

    return static_cast<uint32>(m_drawGroups.size() - 1u);
}

uint32 GPUCulling::AddInstance(const GPUInstanceData& instance)
{
    // Generic callers do not expose an object/submesh identity. Preserve the
    // legacy positional contract for them; renderer-owned callers use the
    // stable overload below and therefore retain rows across packet reorder.
    return AddInstanceWithStableKey(
        instance,
        {std::numeric_limits<uint64>::max(), m_instanceCount},
        0);
}

uint32 GPUCulling::AddInstanceWithStableKey(
    const GPUInstanceData& instance,
    GPUCullingStableInstanceKey key,
    uint64 sourceRevision,
    uint64 rasterSemanticIdentity)
{
    InvalidateFrameSnapshot();
    if (m_instanceCount >= m_config.maxInstances || !key.IsValid())
    {
        return RVX_INVALID_INDEX;
    }

    const bool explicitGroup = m_activeDrawGroupIndex != RVX_INVALID_INDEX;
    const uint32 previousActiveGroup = m_activeDrawGroupIndex;
    const size_t previousDrawGroupCount = m_drawGroups.size();
    const size_t previousDrawCountCount = m_groupDrawCounts.size();
    const auto rollbackNewImplicitGroup = [this,
                                           previousActiveGroup,
                                           previousDrawGroupCount,
                                           previousDrawCountCount]()
    {
        m_drawGroups.resize(previousDrawGroupCount);
        m_groupDrawCounts.resize(previousDrawCountCount);
        m_activeDrawGroupIndex = previousActiveGroup;
    };
    uint32 groupIndex = RVX_INVALID_INDEX;
    try
    {
        groupIndex = EnsureDefaultDrawGroup();
    }
    catch (...)
    {
        rollbackNewImplicitGroup();
        RVX_RENDER_ERROR("GPUCulling: failed to create implicit draw group");
        return RVX_INVALID_INDEX;
    }
    if (groupIndex == RVX_INVALID_INDEX || groupIndex >= m_drawGroups.size())
    {
        rollbackNewImplicitGroup();
        return RVX_INVALID_INDEX;
    }

    const uint32 index = m_instanceCount;
    GPUInstanceData groupedInstance = instance;
    groupedInstance.drawGroupIndex = groupIndex;
    groupedInstance.drawGroupVisibleOffset =
        m_drawGroups[groupIndex].visibleInstanceOffset;
    GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
    const bool initializeGroup = group.maxDrawCount == 0;
    if (!initializeGroup &&
        (group.indexCount != instance.indexCount ||
             group.firstIndex != instance.firstIndex ||
             group.vertexOffset != instance.vertexOffset))
    {
        if (explicitGroup)
        {
            return RVX_INVALID_INDEX;
        }
        try
        {
            groupIndex = BeginDrawGroup(0);
            m_activeDrawGroupIndex = RVX_INVALID_INDEX;
        }
        catch (...)
        {
            rollbackNewImplicitGroup();
            RVX_RENDER_ERROR("GPUCulling: failed to split an implicit draw group");
            return RVX_INVALID_INDEX;
        }
        if (groupIndex == RVX_INVALID_INDEX)
        {
            rollbackNewImplicitGroup();
            return RVX_INVALID_INDEX;
        }
        const uint32 admitted =
            AddInstanceWithStableKey(instance,
                                     key,
                                     sourceRevision,
                                     rasterSemanticIdentity);
        if (admitted == RVX_INVALID_INDEX)
        {
            rollbackNewImplicitGroup();
        }
        return admitted;
    }

    try
    {
        if (!m_collectedInputByKey.emplace(key, index).second)
        {
            rollbackNewImplicitGroup();
            return RVX_INVALID_INDEX;
        }
    }
    catch (...)
    {
        rollbackNewImplicitGroup();
        RVX_RENDER_ERROR("GPUCulling: failed to admit stable instance key");
        return RVX_INVALID_INDEX;
    }
    // All frame-local vectors were reserved to maxInstances during
    // Initialize. After the hash admission succeeds, publication has no
    // allocating step and therefore cannot leave a half-collected packet.
    if (initializeGroup)
    {
        group.indexCount = instance.indexCount;
        group.firstIndex = instance.firstIndex;
        group.vertexOffset = instance.vertexOffset;
    }
    m_instances.push_back(groupedInstance);
    m_collectedKeys.push_back(key);
    m_collectedRasterSemanticIdentities.push_back(rasterSemanticIdentity);
    m_collectedSourceRevisions.push_back(sourceRevision);
    const auto existing = m_canonicalRowByKey.find(key);
    if (existing != m_canonicalRowByKey.end())
    {
        const uint32 residentRow = existing->second;
        if (residentRow >= m_canonicalKeys.size() ||
            !(m_canonicalKeys[residentRow] == key))
        {
            m_instances.pop_back();
            m_collectedKeys.pop_back();
            m_collectedRasterSemanticIdentities.pop_back();
            m_collectedSourceRevisions.pop_back();
            m_collectedInputByKey.erase(key);
            rollbackNewImplicitGroup();
            return RVX_INVALID_INDEX;
        }
        const bool sourceChanged = m_fullCanonicalMutation ||
            m_changedObjectIds.contains(key.objectId) ||
            m_removedObjectIds.contains(key.objectId) ||
            m_canonicalSourceRevisions[residentRow] != sourceRevision;
        m_collectedCanonicalRows.push_back(residentRow);
        m_collectedRequiresComparison.push_back(sourceChanged ? 1u : 0u);
    }
    else
    {
        m_collectedCanonicalRows.push_back(RVX_INVALID_INDEX);
        m_collectedRequiresComparison.push_back(1u);
    }
    ++m_instanceCount;
    ++group.maxDrawCount;
    return index;
}

void GPUCulling::AddInstances(const GPUInstanceData* instances, uint32 count)
{
    uint32 availableSlots = m_config.maxInstances - m_instanceCount;
    count = std::min(count, availableSlots);

    for (uint32 i = 0; i < count; ++i)
    {
        AddInstance(instances[i]);
    }
}

uint32 GPUCulling::AddDrawItemInstance(const RenderScene& scene,
                                       const RenderDrawItem& drawItem,
                                       const GPUIndexedDrawDesc& drawDesc,
                                       uint32 sourceIndex)
{
    if (drawItem.objectIndex >= scene.GetObjectCount() || drawDesc.indexCount == 0)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderObject& object = scene.GetObject(drawItem.objectIndex);
    if (!object.visible)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderVisibilityGPUInput visibilityInput =
        MakeRenderVisibilityGPUInput(object.bounds);
    const Vec3 center = visibilityInput.forceVisible == 0
        ? object.bounds.GetCenter()
        : Vec3(0.0f);
    const float radius = visibilityInput.forceVisible == 0
        ? length(object.bounds.GetExtent())
        : 0.0f;

    GPUInstanceData instance = {};
    instance.worldMatrix = object.worldMatrix;
    instance.normalMatrix = object.normalMatrix;
    instance.boundingSphere = Vec4(center, radius);
    instance.aabbMin = visibilityInput.aabbMin;
    instance.aabbMax = visibilityInput.aabbMax;
    instance.meshId = drawItem.mesh.slot;
    instance.materialId = drawItem.material.slot;
    instance.indexCount = drawDesc.indexCount;
    instance.firstIndex = drawDesc.firstIndex;
    instance.vertexOffset = drawDesc.vertexOffset;
    instance.sourceIndex = sourceIndex;
    instance.candidateIndex = sourceIndex;
    instance.forceVisible = visibilityInput.forceVisible;
    const uint32 instanceIndex = AddInstanceWithStableKey(
        instance,
        {object.entityId, drawItem.submeshIndex},
        object.objectRevision);
    if (instanceIndex != RVX_INVALID_INDEX && instanceIndex < m_instances.size())
    {
        const uint32 groupIndex = m_instances[instanceIndex].drawGroupIndex;
        if (groupIndex >= m_drawGroups.size())
        {
            return RVX_INVALID_INDEX;
        }
        GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
        group.mesh = drawItem.mesh;
        group.material = drawItem.material;
        group.meshId = (static_cast<uint64>(drawItem.mesh.slot) << 32U) |
                       drawItem.mesh.generation;
        group.materialId =
            (static_cast<uint64>(drawItem.material.slot) << 32U) |
            drawItem.material.generation;
        group.pipelineVariant = GetGPUCullingPipelineVariant(drawItem.renderMode);
    }
    return instanceIndex;
}

uint32 GPUCulling::AddVisibilityCandidateInstance(
    const RenderScene& scene,
    const RenderVisibilityCandidate& candidate,
    const RenderDrawPacket& packet,
    const GPUIndexedDrawDesc& drawDesc,
    const RenderResourceRegistry* resourceRegistry)
{
    if (m_activeDrawGroupIndex == RVX_INVALID_INDEX ||
        m_activeDrawGroupIndex >= m_drawGroups.size())
    {
        return RVX_INVALID_INDEX;
    }
    const GPUCullingDrawGroup& activeGroup =
        m_drawGroups[m_activeDrawGroupIndex];
    if (activeGroup.batchKey.pass != RenderPassKind::None &&
        MakeRenderInstanceBatchKey(packet, activeGroup.batchKey.layout) !=
            activeGroup.batchKey)
    {
        return RVX_INVALID_INDEX;
    }
    if (candidate.candidateIndex == RVX_INVALID_INDEX ||
        candidate.sourcePacketIndex == RVX_INVALID_INDEX ||
        candidate.pass == RenderPassKind::None ||
        candidate.pass != packet.pass ||
        packet.primitiveData != candidate.objectIndex ||
        packet.objectId == 0 ||
        candidate.objectIndex >= scene.GetObjectCount() ||
        !candidate.objectVisible || !candidate.drawable ||
        drawDesc.indexCount == 0 ||
        drawDesc.indexCount != packet.arguments.indexCount ||
        drawDesc.firstIndex != packet.arguments.firstIndex ||
        drawDesc.vertexOffset != packet.arguments.vertexOffset)
    {
        return RVX_INVALID_INDEX;
    }

    const RenderObject& object = scene.GetObject(candidate.objectIndex);
    if (!object.visible || !object.drawable ||
        object.entityId != packet.objectId ||
        object.mesh != packet.geometryKey.mesh)
    {
        return RVX_INVALID_INDEX;
    }
    const RenderVisibilityGPUInput visibilityInput =
        MakeRenderVisibilityGPUInput(candidate.worldBounds);
    const Vec3 center = visibilityInput.forceVisible == 0
        ? candidate.worldBounds.GetCenter()
        : Vec3(0.0f);
    const float radius = visibilityInput.forceVisible == 0
        ? length(candidate.worldBounds.GetExtent())
        : 0.0f;

    GPUInstanceData instance{};
    instance.worldMatrix = object.worldMatrix;
    instance.normalMatrix = object.normalMatrix;
    instance.boundingSphere = Vec4(center, radius);
    instance.aabbMin = visibilityInput.aabbMin;
    instance.aabbMax = visibilityInput.aabbMax;
    instance.meshId = packet.geometryKey.mesh.slot;
    instance.materialId = packet.materialKey.material.slot;
    // Exact raster semantic evidence is finalized by OpaquePass only after
    // its real material bindings/table have committed. Collection remains
    // CPU-only and must not predict descriptor fallback here.
    static_cast<void>(resourceRegistry);
    instance.indexCount = drawDesc.indexCount;
    instance.firstIndex = drawDesc.firstIndex;
    instance.vertexOffset = drawDesc.vertexOffset;
    instance.sourceIndex = candidate.sourcePacketIndex;
    instance.candidateIndex = candidate.candidateIndex;
    instance.forceVisible = visibilityInput.forceVisible;
    return AddInstanceWithStableKey(
        instance,
        {packet.objectId, packet.submeshIndex},
        object.objectRevision,
        0);
}

bool GPUCulling::AddGPUSceneCandidate(
    const GPUSceneCullingCandidate& candidate,
    uint64 committedVersion)
{
    if (committedVersion == 0 || candidate.primitiveSlot == 0 ||
        candidate.primitiveGeneration == 0 || candidate.drawSlot == 0 ||
        candidate.drawGeneration == 0 ||
        (candidate.objectIdLow == 0 && candidate.objectIdHigh == 0) ||
        candidate.requiredPassMask == 0 ||
        (candidate.requiredPassMask & (candidate.requiredPassMask - 1u)) != 0 ||
        candidate.materialParameterSlot == RVX_INVALID_INDEX ||
        m_instances.empty() ||
        candidate.rasterInstanceIndex != m_instanceCount - 1u ||
        candidate.rasterInstanceIndex != m_gpuSceneCandidates.size() ||
        candidate.rasterInstanceIndex >= m_instances.size())
    {
        return false;
    }

    const GPUInstanceData& instance = m_instances[candidate.rasterInstanceIndex];
    if (candidate.drawGroupIndex != instance.drawGroupIndex ||
        candidate.drawGroupVisibleOffset != instance.drawGroupVisibleOffset ||
        candidate.drawGroupIndex >= m_drawGroups.size())
    {
        return false;
    }

    if (m_gpuSceneCandidateVersion != 0 &&
        m_gpuSceneCandidateVersion != committedVersion)
    {
        return false;
    }

    m_gpuSceneCandidateVersion = committedVersion;
    m_gpuSceneCandidates.push_back(candidate);
    ++m_gpuSceneCandidateCount;
    InvalidateFrameSnapshot();
    return true;
}

void GPUCulling::InvalidateGPUSceneCandidates() noexcept
{
    m_gpuSceneCandidates.clear();
    m_gpuSceneCandidateVersion = 0;
    m_gpuSceneCandidateCount = 0;
    m_finalizedGPUSceneCandidateVersion = 0;
}

bool GPUCulling::HasCompleteGPUSceneCandidates() const noexcept
{
    return HasCompleteGPUSceneCandidates(m_gpuSceneCandidateVersion);
}

bool GPUCulling::HasCompleteGPUSceneCandidates(
    uint64 requiredVersion) const noexcept
{
    return requiredVersion != 0 &&
        m_gpuSceneCandidateVersion == requiredVersion &&
        m_instanceCount != 0 &&
        m_gpuSceneCandidateCount == m_instanceCount &&
        (!m_recordingGpuOnly
             ? m_gpuSceneCandidates.size() == m_instanceCount
             : true);
}

void GPUCulling::EndFrame()
{
    InvalidateFrameSnapshot();
    if (!FinalizeCpuSnapshotVersions())
    {
        return;
    }

    if (GPUCullingFrameInputs* inputs = GetActiveFrameInputs())
    {
        inputs->instanceDesiredVersion = m_finalizedInstanceVersion;
        inputs->activeRowsDesiredVersion = m_finalizedActiveRowVersion;
        inputs->gpuSceneCandidateDesiredVersion =
            HasCompleteGPUSceneCandidates()
            ? m_finalizedGPUSceneCandidateVersion
            : 0;
    }

    // EndFrame freezes CPU state only.  Tier 1 and Tier 2 seals upload their
    // own independent input streams after their execution path is selected.
    m_stats.totalInstances = m_instanceCount;
}

bool GPUCulling::UploadInstances()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (m_instances.empty() || inputs == nullptr || !inputs->instanceBuffer ||
        inputs->instanceDesiredVersion == 0)
    {
        return false;
    }
    m_dirtyRangeScratch.clear();
    bool fullMaterialization = false;
    bool continuityBroken = false;
    if (!ResolveDirtyRanges(m_instanceDirtyJournals,
                            inputs->instanceResidentVersion,
                            inputs->instanceDesiredVersion,
                            static_cast<uint32>(m_canonicalInstances.size()),
                            m_dirtyRangeScratch,
                            fullMaterialization,
                            continuityBroken) ||
        !UploadCanonicalRows(inputs->instanceBuffer.Get(),
                             std::span<const GPUInstanceData>(m_canonicalInstances),
                             std::span<const DirtyRange>(m_dirtyRangeScratch),
                             fullMaterialization,
                             m_lastInstanceUploadBytes,
                             m_incrementalDiagnostics.instanceUploadWork))
    {
        // Preserve receipt work in the diagnostic value, but never leave the
        // legacy resident-byte publication looking like a successful stream.
        // A partial range transaction must retry as a full materialization.
        m_lastInstanceUploadBytes = 0;
        inputs->instanceResidentVersion = 0;
        inputs->instanceAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs->instanceAccess.rangeOverrides.clear();
        return false;
    }
    if (fullMaterialization)
    {
        ++m_incrementalDiagnostics.instanceFullMaterializationCount;
        SaturatingAdd(m_mutationTotals.instanceFullMaterializationCount,
                      1,
                      m_mutationTotalsSaturated);
        if (continuityBroken)
        {
            ++m_incrementalDiagnostics.continuityFullMaterializationCount;
            SaturatingAdd(m_mutationTotals.continuityFullMaterializationCount,
                          1,
                          m_mutationTotalsSaturated);
        }
    }
    SaturatingAdd(m_mutationTotals.instanceUploadBytes,
                  m_lastInstanceUploadBytes,
                  m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.instanceUploadedRowCount,
                  m_lastInstanceUploadBytes / sizeof(GPUInstanceData),
                  m_mutationTotalsSaturated);
    const GPUQueueDomain lastGpuDomain =
        inputs->instanceAccess.uniformAccess.domain;
    inputs->instanceAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    inputs->instanceResidentVersion = inputs->instanceDesiredVersion;
    m_accessSnapshots.instances = inputs->instanceAccess;
    inputs->accessSnapshots.instances = inputs->instanceAccess;
    m_stats.totalInstances = m_instanceCount;
    return true;
}

template<typename T>
bool GPUCulling::UploadCanonicalRows(RHIBuffer* buffer,
                                     std::span<const T> canonicalRows,
                                     std::span<const DirtyRange> ranges,
                                     bool fullMaterialization,
                                     uint64& outBytes,
                                     RenderUploadWorkDiagnostics& outWork)
{
    outBytes = 0;
    outWork = {};
    if (buffer == nullptr || canonicalRows.empty())
    {
        return false;
    }
    const uint32 fullRowCount = std::min<uint32>(
        static_cast<uint32>(canonicalRows.size()),
        std::max(1u, m_incrementalDiagnostics.activeRowHighWatermark));
    const uint64 requiredBytes = static_cast<uint64>(fullRowCount) * sizeof(T);
    if (requiredBytes > buffer->GetSize())
    {
        RVX_RENDER_ERROR("GPUCulling: canonical row upload exceeds buffer capacity");
        return false;
    }
    if (!fullMaterialization && ranges.empty())
    {
        return true;
    }
    const DirtyRange fullRange{0, fullRowCount};
    const std::span<const DirtyRange> uploadRanges = fullMaterialization
        ? std::span<const DirtyRange>(&fullRange, 1)
        : ranges;
    for (const DirtyRange& range : uploadRanges)
    {
        const uint64 rangeEnd = static_cast<uint64>(range.firstRow) +
            range.rowCount;
        if (range.rowCount == 0 || rangeEnd > canonicalRows.size() ||
            rangeEnd * sizeof(T) > buffer->GetSize())
        {
            RVX_RENDER_ERROR("GPUCulling: invalid canonical dirty range");
            return false;
        }
    }

    for (const DirtyRange& range : uploadRanges)
    {
        const uint64 byteOffset = static_cast<uint64>(range.firstRow) *
            sizeof(T);
        const uint64 bytes = static_cast<uint64>(range.rowCount) * sizeof(T);
        RHIMappedWriteAccess access = buffer->MapWriteRange(byteOffset, bytes);
        if (!access.IsValid())
        {
            RVX_RENDER_ERROR("GPUCulling: failed to map canonical row range");
            return false;
        }
        std::memcpy(access.GetData(),
                    canonicalRows.data() + range.firstRow,
                    static_cast<size_t>(bytes));
        RecordMappedUploadRange(outWork, bytes);
        const RHIHostWriteReceipt receipt = buffer->CommitMappedWriteRange(
            std::move(access));
        if (!receipt.IsPublished())
        {
            RVX_RENDER_ERROR("GPUCulling: failed to commit canonical row range");
            return false;
        }
        RecordCommittedUploadRange(outWork, receipt, bytes);
        outBytes += bytes;
    }
    return true;
}

bool GPUCulling::ResolveDirtyRanges(
    const std::deque<DirtyJournal>& journals,
    uint64 residentVersion,
    uint64 desiredVersion,
    uint32 rowCapacity,
    std::vector<DirtyRange>& outRanges,
    bool& outFullMaterialization,
    bool& outContinuityBroken) const
{
    outRanges.clear();
    outFullMaterialization = false;
    outContinuityBroken = false;
    if (desiredVersion == 0 || rowCapacity == 0)
    {
        return false;
    }
    if (residentVersion == desiredVersion)
    {
        return true;
    }
    if (residentVersion == 0 || journals.empty())
    {
        outFullMaterialization = true;
        outContinuityBroken = residentVersion != 0;
        return true;
    }

    uint64 expectedVersion = residentVersion + 1u;
    bool foundDesired = false;
    for (const DirtyJournal& journal : journals)
    {
        if (journal.version <= residentVersion)
        {
            continue;
        }
        if (journal.version != expectedVersion || journal.fullMaterialization)
        {
            outFullMaterialization = true;
            outContinuityBroken = true;
            return true;
        }
        for (const DirtyRange& range : journal.ranges)
        {
            if (range.rowCount == 0 ||
                static_cast<uint64>(range.firstRow) + range.rowCount > rowCapacity)
            {
                return false;
            }
            outRanges.push_back(range);
        }
        if (journal.version == desiredVersion)
        {
            foundDesired = true;
            break;
        }
        ++expectedVersion;
    }
    if (!foundDesired)
    {
        outRanges.clear();
        outFullMaterialization = true;
        outContinuityBroken = true;
        return true;
    }

    std::vector<uint32> rows;
    try
    {
        for (const DirtyRange& range : outRanges)
        {
            for (uint32 offset = 0; offset < range.rowCount; ++offset)
            {
                rows.push_back(range.firstRow + offset);
            }
        }
    }
    catch (...)
    {
        return false;
    }
    MergeDirtyRows(rows, outRanges);
    return true;
}

bool GPUCulling::UploadActiveRows()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (inputs == nullptr || !inputs->instanceIndexBuffer ||
        inputs->activeRowsDesiredVersion == 0)
    {
        return false;
    }
    m_dirtyRangeScratch.clear();
    bool fullMaterialization = false;
    bool continuityBroken = false;
    if (!ResolveDirtyRanges(m_activeRowDirtyJournals,
                            inputs->activeRowsResidentVersion,
                            inputs->activeRowsDesiredVersion,
                            static_cast<uint32>(m_canonicalActiveRows.size()),
                            m_dirtyRangeScratch,
                            fullMaterialization,
                            continuityBroken) ||
        !UploadCanonicalRows(inputs->instanceIndexBuffer.Get(),
                             std::span<const GPUCullingActiveRow>(m_canonicalActiveRows),
                             std::span<const DirtyRange>(m_dirtyRangeScratch),
                             fullMaterialization,
                             m_incrementalDiagnostics.activeRowUploadBytes,
                             m_incrementalDiagnostics.activeRowUploadWork))
    {
        m_incrementalDiagnostics.activeRowUploadBytes = 0;
        InvalidateActiveRowResidency(*inputs);
        return false;
    }
    if (fullMaterialization)
    {
        ++m_incrementalDiagnostics.activeRowFullMaterializationCount;
        SaturatingAdd(m_mutationTotals.activeRowFullMaterializationCount,
                      1,
                      m_mutationTotalsSaturated);
        if (continuityBroken)
        {
            ++m_incrementalDiagnostics.continuityFullMaterializationCount;
            SaturatingAdd(m_mutationTotals.continuityFullMaterializationCount,
                          1,
                          m_mutationTotalsSaturated);
        }
    }
    SaturatingAdd(m_mutationTotals.activeRowUploadBytes,
                  m_incrementalDiagnostics.activeRowUploadBytes,
                  m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.activeRowUploadedRowCount,
                  m_incrementalDiagnostics.activeRowUploadBytes /
                      sizeof(GPUCullingActiveRow),
                  m_mutationTotalsSaturated);
    const GPUQueueDomain lastGpuDomain =
        inputs->accessSnapshots.instanceIndices.uniformAccess.domain;
    inputs->accessSnapshots.instanceIndices = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    inputs->activeRowsResidentVersion = inputs->activeRowsDesiredVersion;
    m_accessSnapshots.instanceIndices = inputs->accessSnapshots.instanceIndices;
    return true;
}

bool GPUCulling::BuildGpuIndirectCommandPrefill()
{
    try
    {
        // Compaction owns the visible instance count. The prefill carries the
        // immutable draw metadata and uses firstInstance as the dense active
        // range start for the workgroup that owns this draw group.
        m_indirectCommands.resize(m_drawGroups.size());
    }
    catch (...)
    {
        RVX_RENDER_ERROR(
            "GPUCulling: failed to allocate indirect-command prefill");
        return false;
    }

    for (uint32 groupIndex = 0;
         groupIndex < static_cast<uint32>(m_drawGroups.size());
         ++groupIndex)
    {
        const GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
        IndirectDrawIndexedCommand& command =
            m_indirectCommands[group.commandOffset];
        command.indexCount = group.indexCount;
        // This is a compaction input range until CSFinalizeDrawGroups replaces
        // it with the final visible count. It is never submitted in this form.
        command.instanceCount = group.maxDrawCount;
        command.firstIndex = group.firstIndex;
        command.vertexOffset = group.vertexOffset;
        command.firstInstance = group.visibleInstanceOffset;
    }
    return true;
}

void GPUCulling::BuildCpuCullResults(const Mat4& viewMatrix, const Vec4* frustumPlanes)
{
    m_visibleInstanceIndices.clear();
    m_rasterVisibleInstanceIndices.assign(m_instanceCount, RVX_INVALID_INDEX);
    m_visibleSourceIndices.clear();
    m_indirectCommands.assign(m_drawGroups.size(), {});
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_stats = {};
    m_stats.totalInstances = m_instanceCount;

    const Vec3 cameraPosition = Vec3(inverse(viewMatrix)[3]);

    for (uint32 instanceIndex = 0; instanceIndex < m_instanceCount; ++instanceIndex)
    {
        const GPUInstanceData& instance = m_instances[instanceIndex];
        const Vec3 center(instance.boundingSphere.x, instance.boundingSphere.y, instance.boundingSphere.z);
        const Vec3 extent = Vec3(instance.aabbMax - instance.aabbMin) * 0.5f;

        bool visible = true;
        if (m_config.enableFrustumCulling && instance.forceVisible == 0)
        {
            for (uint32 planeIndex = 0; planeIndex < 6; ++planeIndex)
            {
                const Vec4& plane = frustumPlanes[planeIndex];
                const float distanceToPlane = plane.x * center.x + plane.y * center.y +
                                              plane.z * center.z + plane.w;
                const Vec3 normal(plane.x, plane.y, plane.z);
                if (dot(normal, normal) <= 1.0e-12f)
                {
                    continue;
                }
                const float projectedRadius = dot(glm::abs(normal), extent);
                if (IsConservativelyOutsideFrustumPlane(
                        distanceToPlane, projectedRadius))
                {
                    visible = false;
                    ++m_stats.frustumCulled;
                    break;
                }
            }
        }

        if (!visible)
        {
            continue;
        }

        if (visible && instance.forceVisible == 0 &&
            m_config.enableDistanceCulling && m_config.maxDrawDistance > 0.0f)
        {
            const float radius = std::max(instance.boundingSphere.w, 0.0f);
            const float distanceToCamera = length(center - cameraPosition);
            if (distanceToCamera - radius > m_config.maxDrawDistance)
            {
                ++m_stats.distanceCulled;
                continue;
            }
        }

        if (instance.indexCount == 0)
        {
            continue;
        }

        m_visibleInstanceIndices.push_back(instanceIndex);
        if (instance.sourceIndex != RVX_INVALID_INDEX)
        {
            m_visibleSourceIndices.push_back(instance.sourceIndex);
        }

        if (instance.drawGroupIndex >= m_drawGroups.size())
        {
            continue;
        }
        GPUCullingDrawGroup& group = m_drawGroups[instance.drawGroupIndex];
        const uint32 visibleIndex =
            group.visibleInstanceOffset + group.visibleDrawCount;
        if (visibleIndex >= m_rasterVisibleInstanceIndices.size() ||
            instanceIndex >= m_collectedCanonicalRows.size() ||
            m_collectedCanonicalRows[instanceIndex] == RVX_INVALID_INDEX)
        {
            continue;
        }
        m_rasterVisibleInstanceIndices[visibleIndex] =
            m_collectedCanonicalRows[instanceIndex];
        ++group.visibleDrawCount;
    }

    for (uint32 groupIndex = 0;
         groupIndex < static_cast<uint32>(m_drawGroups.size());
         ++groupIndex)
    {
        GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
        if (group.visibleDrawCount == 0)
        {
            continue;
        }
        IndirectDrawIndexedCommand& command =
            m_indirectCommands[group.commandOffset];
        command.indexCount = group.indexCount;
        command.instanceCount = group.visibleDrawCount;
        command.firstIndex = group.firstIndex;
        command.vertexOffset = group.vertexOffset;
        command.firstInstance = group.visibleInstanceOffset;
        m_groupDrawCounts[groupIndex] = 1;
        ++m_drawCount;
    }

    m_stats.visibleInstances = static_cast<uint32>(m_visibleInstanceIndices.size());
}

bool GPUCulling::UploadBufferData(RHIBuffer* buffer,
                                  const void* data,
                                  uint64 size,
                                  RHICommandContext* ctx)
{
    if (!buffer || !data || size == 0)
    {
        return true;
    }

    if (size > buffer->GetSize())
    {
        RVX_RENDER_ERROR(
            "GPUCulling: rejected buffer upload that exceeds the destination capacity");
        return false;
    }

    if (buffer->GetMemoryType() != RHIMemoryType::Default)
    {
        RHIMappedWriteAccess access = buffer->MapWriteRange(0, size);
        if (!access.IsValid())
        {
            RVX_RENDER_ERROR("GPUCulling: failed to map upload buffer");
            return false;
        }

        std::memcpy(access.GetData(), data, static_cast<size_t>(size));
        if (!buffer->CommitMappedWriteRange(std::move(access)).IsPublished())
        {
            RVX_RENDER_ERROR("GPUCulling: failed to commit upload buffer write");
            return false;
        }

        return true;
    }

    if (!ctx || !m_device)
    {
        return false;
    }

    RHIBufferDesc stagingDesc;
    stagingDesc.size = size;
    stagingDesc.usage = RHIBufferUsage::CopySrc;
    stagingDesc.memoryType = RHIMemoryType::Upload;
    stagingDesc.debugName = "GPUCulling.TransientUpload";

    RHIBufferRef stagingBuffer = m_device->CreateBuffer(stagingDesc);
    if (!stagingBuffer)
    {
        return false;
    }

    RHIMappedWriteAccess stagingAccess = stagingBuffer->MapWriteRange(0, size);
    if (!stagingAccess.IsValid())
    {
        return false;
    }

    std::memcpy(stagingAccess.GetData(), data, static_cast<size_t>(size));
    if (!stagingBuffer->CommitMappedWriteRange(std::move(stagingAccess)).IsPublished())
    {
        RVX_RENDER_ERROR("GPUCulling: failed to commit transient upload buffer write");
        return false;
    }

    ctx->CopyBuffer(stagingBuffer.Get(), buffer, 0, 0, size);
    m_transientUploadBuffers.push_back(stagingBuffer);
    return true;
}

bool GPUCulling::UploadCullOutputs(RHICommandContext* ctx)
{
    if (!m_drawCountBuffer)
    {
        return false;
    }

    std::vector<uint32> drawCounts;
    drawCounts.reserve(m_groupDrawCounts.size() + 1);
    drawCounts.push_back(m_drawCount);
    drawCounts.insert(drawCounts.end(), m_groupDrawCounts.begin(), m_groupDrawCounts.end());
    if (!UploadBufferData(
            m_drawCountBuffer.Get(),
            drawCounts.data(),
            static_cast<uint64>(drawCounts.size() * sizeof(uint32)),
            ctx))
    {
        return false;
    }

    if (!m_rasterVisibleInstanceIndices.empty())
    {
        if (!m_visibleInstanceBuffer || !UploadBufferData(
                m_visibleInstanceBuffer.Get(),
                m_rasterVisibleInstanceIndices.data(),
                static_cast<uint64>(
                    m_rasterVisibleInstanceIndices.size() * sizeof(uint32)),
                ctx))
        {
            return false;
        }
    }

    if (!m_indirectCommands.empty())
    {
        if (!m_indirectBuffer || !UploadBufferData(
                m_indirectBuffer.Get(),
                m_indirectCommands.data(),
                static_cast<uint64>(
                    m_indirectCommands.size() * sizeof(IndirectDrawIndexedCommand)),
                ctx))
        {
            return false;
        }
    }

    return true;
}

void GPUCulling::ExtractFrustumPlanes(const Mat4& viewProj, Vec4* planes)
{
    // Extract 6 frustum planes from view-projection matrix
    // Left, Right, Bottom, Top, Near, Far

    // Left plane
    planes[0] = Vec4(
        viewProj[0][3] + viewProj[0][0],
        viewProj[1][3] + viewProj[1][0],
        viewProj[2][3] + viewProj[2][0],
        viewProj[3][3] + viewProj[3][0]
    );

    // Right plane
    planes[1] = Vec4(
        viewProj[0][3] - viewProj[0][0],
        viewProj[1][3] - viewProj[1][0],
        viewProj[2][3] - viewProj[2][0],
        viewProj[3][3] - viewProj[3][0]
    );

    // Bottom plane
    planes[2] = Vec4(
        viewProj[0][3] + viewProj[0][1],
        viewProj[1][3] + viewProj[1][1],
        viewProj[2][3] + viewProj[2][1],
        viewProj[3][3] + viewProj[3][1]
    );

    // Top plane
    planes[3] = Vec4(
        viewProj[0][3] - viewProj[0][1],
        viewProj[1][3] - viewProj[1][1],
        viewProj[2][3] - viewProj[2][1],
        viewProj[3][3] - viewProj[3][1]
    );

    // Near plane
    planes[4] = Vec4(
        viewProj[0][2],
        viewProj[1][2],
        viewProj[2][2],
        viewProj[3][2]
    );

    // Far plane
    planes[5] = Vec4(
        viewProj[0][3] - viewProj[0][2],
        viewProj[1][3] - viewProj[1][2],
        viewProj[2][3] - viewProj[2][2],
        viewProj[3][3] - viewProj[3][2]
    );

    // Keep the exact unnormalized plane convention used by
    // RenderVisibilityFrustum::FromViewProjection. Scaling an AABB plane is
    // mathematically harmless, but a separate normalization changes which
    // side a boundary candidate lands on after CPU/GPU float rounding.
}

void GPUCulling::Cull(RHICommandContext& ctx,
                      const Mat4& viewMatrix,
                      const Mat4& projMatrix,
                      RHITexture* hiZTexture)
{
    m_visibleInstanceIndices.clear();
    m_rasterVisibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;

    if (m_instanceCount == 0)
    {
        if (!UploadCullOutputs(&ctx))
        {
            m_lastFallbackReason =
                GPUCullingFallbackReason::PipelineResourcesUnavailable;
        }
        return;
    }

    if (!IsCurrentInstanceSnapshotWithinConfiguredCapacity())
    {
        // A resize may have replaced the per-slot buffers with a smaller
        // capacity after collection.  Do not turn the old CPU vector into
        // stale output or attempt any GPU write until a legal frame rebuilds
        // and finalizes a new snapshot.
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    Mat4 viewProj = projMatrix * viewMatrix;

    // Update the shared, fixed ABI.  The GPU-scene table capacity blocks are
    // explicitly zero for Tier 1 culling so no stale lease values can affect
    // a normal dispatch.
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();

    constants.viewProj = viewProj;
    ExtractFrustumPlanes(viewProj, constants.frustumPlanes);
    constants.cameraPosition = Vec4(inverse(viewMatrix)[3]);
    constants.params = Vec4(
        m_config.maxDrawDistance,
        0.0f,
        m_config.enableFrustumCulling ? 1.0f : 0.0f,
        m_config.enableDistanceCulling ? 1.0f : 0.0f
    );
    const uint32 drawGroupCount = static_cast<uint32>(m_drawGroups.size());
    const CompactDispatchDimensions compactDispatch =
        GetCompactDispatchDimensions(drawGroupCount);
    // Counts.x addresses the dense active dispatch stream. Sparse canonical
    // instance/candidate buffers keep their independent resident high-water.
    constants.counts[0] = m_incrementalDiagnostics.activeRowCount;
    constants.counts[1] = drawGroupCount;
    constants.counts[2] = compactDispatch.width;

    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    const GPUCullingExecutionDecision executionDecision = EvaluateGpuExecution(true);
    const bool tierOneInputsResident =
        executionDecision.mode != GPUCullingExecutionMode::GpuCompute ||
        (m_recordingGpuOnly
             ? inputs != nullptr && IsInstanceResident(*inputs) &&
                   IsActiveRowsResident(*inputs)
             : EnsureInstanceResidency());
    if (executionDecision.mode == GPUCullingExecutionMode::GpuCompute &&
        !tierOneInputsResident)
    {
        // A CPU-only EndFrame leaves this stream intentionally deferred.  A
        // GPU dispatch may begin only after this exact physical slot contains
        // the finalized Tier 1 version.  This also rejects Tier 2-only seals,
        // which do not retain a Tier 1 instance binding.
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    bool constantsUploaded = false;
    if (inputs != nullptr && inputs->constantsBuffer)
    {
        const GPUQueueDomain lastGpuDomain =
            inputs->constantsAccess.uniformAccess.domain;
        inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
            RHIResourceState::ConstantBuffer,
            RHIShaderStage::Compute,
            lastGpuDomain,
            RHIContentValidity::Invalid);
        m_accessSnapshots.constants = inputs->constantsAccess;
        inputs->accessSnapshots.constants = inputs->constantsAccess;

        constantsUploaded = UploadBufferData(
            inputs->constantsBuffer.Get(), &constants, sizeof(constants), &ctx);
        if (constantsUploaded)
        {
            inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
                RHIResourceState::ConstantBuffer,
                RHIShaderStage::Compute,
                lastGpuDomain,
                RHIContentValidity::Valid);
            m_accessSnapshots.constants = inputs->constantsAccess;
            inputs->accessSnapshots.constants = inputs->constantsAccess;
        }
    }

    if (executionDecision.mode == GPUCullingExecutionMode::GpuCompute &&
        !constantsUploaded)
    {
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    if (executionDecision.mode != GPUCullingExecutionMode::GpuCompute)
    {
        if (m_recordingGpuOnly)
        {
            // A graph seal intentionally owns no CPU collection payload. Do
            // not reinterpret a post-seal capability loss as permission to
            // run a dense CPU fallback with stale/empty data.
            m_lastFallbackReason = executionDecision.fallbackReason;
            return;
        }
        const Mat4 cpuViewProj = projMatrix * viewMatrix;
        Vec4 frustumPlanes[6];
        ExtractFrustumPlanes(cpuViewProj, frustumPlanes);
        BuildCpuCullResults(viewMatrix, frustumPlanes);
        if (!UploadCullOutputs(&ctx))
        {
            m_lastFallbackReason =
                GPUCullingFallbackReason::PipelineResourcesUnavailable;
            return;
        }

        m_usedCpuFallbackLastCull = true;
        m_lastFallbackReason = executionDecision.fallbackReason;
        return;
    }

    const RHIAccessSnapshot computeUAVAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const RHIAccessSnapshot copyDestinationAccess = MakeRHIAccessSnapshot(
        RHIResourceState::CopyDest,
        RHIShaderStage::All,
        GPUQueueDomain::Graphics);

    const bool qualificationReferenceReady =
        !m_gpuSceneQualificationCapture.referencePrepared ||
        BuildGPUSceneQualificationReference(viewMatrix, projMatrix);

    // Compact workgroups use the command prefill as their per-group dense
    // range table. The RenderGraph has already placed this output in UAV state
    // for this pass, so make the copy transition explicit before restoring the
    // declared UAV access for the three compute passes.
    if (!BuildGpuIndirectCommandPrefill() || !m_indirectBuffer)
    {
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }
    ctx.BufferBarrier(
        m_indirectBuffer.Get(), computeUAVAccess, copyDestinationAccess);
    if (!UploadBufferData(
            m_indirectBuffer.Get(),
            m_indirectCommands.data(),
            static_cast<uint64>(m_indirectCommands.size()) *
                sizeof(IndirectDrawIndexedCommand),
            &ctx))
    {
        ctx.BufferBarrier(
            m_indirectBuffer.Get(), copyDestinationAccess, computeUAVAccess);
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }
    // UploadBufferData records its copy before returning. The destination must
    // transition back to the graph-declared UAV state before compaction reads
    // its prefilled firstInstance range.
    ctx.BufferBarrier(
        m_indirectBuffer.Get(), copyDestinationAccess, computeUAVAccess);

    // Dispatch frustum culling compute shader
    ctx.SetPipeline(m_frustumCullPipeline.Get());
    ctx.SetDescriptorSet(0, inputs->descriptorSet.Get());
    // CSFrustumCull clears total + every per-group counter. Dispatch enough
    // threads for the sparse active-row address space and a potentially sparse
    // group set. m_instanceCount is intentionally insufficient after a row
    // has been retired below the high-water mark.
    const uint32 clearThreadCount = std::max(
        m_incrementalDiagnostics.activeRowCount,
        drawGroupCount + 1u);
    const uint32 cullGroupCount = (clearThreadCount + 63u) / 64u;
    ctx.Dispatch(cullGroupCount, 1, 1);
    const auto insertCullUAVBarriers = [&ctx, this, &computeUAVAccess]()
    {
        ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess);
        ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess);
        ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess);
    };

    // Frustum writes are consumed by compaction. Keep the barrier before an
    // optional occlusion pass as well, because it is another visibility consumer.
    insertCullUAVBarriers();

    // Dispatch occlusion culling if enabled and HiZ available
    if (m_config.enableOcclusionCulling && hiZTexture && m_occlusionCullPipeline)
    {
        ctx.SetPipeline(m_occlusionCullPipeline.Get());
        // Bind HiZ texture and buffers...
        ctx.Dispatch(cullGroupCount, 1, 1);
        insertCullUAVBarriers();
    }

    // Compact visible instances into draw commands
    ctx.SetPipeline(m_compactPipeline.Get());
    ctx.SetDescriptorSet(0, inputs->descriptorSet.Get());
    ctx.Dispatch(compactDispatch.width, compactDispatch.height, 1);

    insertCullUAVBarriers();
    ctx.SetPipeline(m_finalizePipeline.Get());
    ctx.SetDescriptorSet(0, inputs->descriptorSet.Get());
    const uint32 finalizeGroupCount =
        (drawGroupCount + 63u) / 64u;
    ctx.Dispatch(finalizeGroupCount, 1, 1);

    // Tier 1 (IndirectGrouped) uses the same finalized output ABI as the
    // GPU-resident path. Keep this opt-in copy after the finalizer so the
    // qualification names its actual execution tier instead of silently
    // waiting for a GPUResidentScene pass that this frame never selected.
    if (qualificationReferenceReady &&
        !RecordGPUSceneQualificationReadback(ctx))
    {
        RVX_RENDER_WARN(
            "GPUCulling: Tier 1 qualification readback was not recorded");
    }

    m_usedGpuExecutionLastCull = true;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats.totalInstances = m_instanceCount;
}

bool GPUCulling::UploadGPUSceneCandidates()
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!HasCompleteGPUSceneCandidates() || inputs == nullptr ||
        !inputs->gpuSceneCandidateBuffer ||
        inputs->gpuSceneCandidateDesiredVersion == 0)
    {
        return false;
    }

    m_dirtyRangeScratch.clear();
    bool fullMaterialization = false;
    bool continuityBroken = false;
    if (!ResolveDirtyRanges(m_candidateDirtyJournals,
                            inputs->gpuSceneCandidateResidentVersion,
                            inputs->gpuSceneCandidateDesiredVersion,
                            static_cast<uint32>(m_canonicalGPUSceneCandidates.size()),
                            m_dirtyRangeScratch,
                            fullMaterialization,
                            continuityBroken) ||
        !UploadCanonicalRows(
            inputs->gpuSceneCandidateBuffer.Get(),
            std::span<const GPUSceneCullingCandidate>(m_canonicalGPUSceneCandidates),
            std::span<const DirtyRange>(m_dirtyRangeScratch),
            fullMaterialization,
            m_lastGPUSceneCandidateUploadBytes,
            m_incrementalDiagnostics.candidateUploadWork))
    {
        m_lastGPUSceneCandidateUploadBytes = 0;
        inputs->gpuSceneCandidateResidentVersion = 0;
        inputs->gpuSceneCandidateAccess.uniformAccess.contentValidity =
            RHIContentValidity::Invalid;
        inputs->gpuSceneCandidateAccess.rangeOverrides.clear();
        return false;
    }
    if (fullMaterialization)
    {
        ++m_incrementalDiagnostics.candidateFullMaterializationCount;
        SaturatingAdd(m_mutationTotals.candidateFullMaterializationCount,
                      1,
                      m_mutationTotalsSaturated);
        if (continuityBroken)
        {
            ++m_incrementalDiagnostics.continuityFullMaterializationCount;
            SaturatingAdd(m_mutationTotals.continuityFullMaterializationCount,
                          1,
                          m_mutationTotalsSaturated);
        }
    }
    SaturatingAdd(m_mutationTotals.candidateUploadBytes,
                  m_lastGPUSceneCandidateUploadBytes,
                  m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.candidateUploadedRowCount,
                  m_lastGPUSceneCandidateUploadBytes /
                      sizeof(GPUSceneCullingCandidate),
                  m_mutationTotalsSaturated);
    const GPUQueueDomain lastGpuDomain =
        inputs->gpuSceneCandidateAccess.uniformAccess.domain;
    inputs->gpuSceneCandidateAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ShaderResource,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    inputs->gpuSceneCandidateResidentVersion =
        inputs->gpuSceneCandidateDesiredVersion;
    m_accessSnapshots.gpuSceneCandidates = inputs->gpuSceneCandidateAccess;
    inputs->accessSnapshots.gpuSceneCandidates =
        inputs->gpuSceneCandidateAccess;
    return true;
}

bool GPUCulling::ConfigureGPUSceneRecording(
    const GPUSceneResidentGraphLease& lease)
{
    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!lease.IsValid() || !HasCompleteGPUSceneCandidates(lease.version) ||
        !IsGPUSceneExecutionReady() || inputs == nullptr ||
        !inputs->constantsBuffer || !inputs->visibilityBuffer ||
        !inputs->instanceIndexBuffer ||
        !inputs->visibleInstanceBuffer || !inputs->indirectBuffer ||
        !inputs->drawCountBuffer)
    {
        return false;
    }

    // Validate the complete immutable binding before allocating or updating
    // any slot-owned resource.  A malformed lease therefore cannot partially
    // mutate the frame slot and cannot poison a previously valid descriptor.
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();
    const uint32 drawGroupCount = static_cast<uint32>(m_drawGroups.size());
    const CompactDispatchDimensions compactDispatch =
        GetCompactDispatchDimensions(drawGroupCount);
    constants.counts[0] = m_incrementalDiagnostics.activeRowCount;
    constants.counts[1] = drawGroupCount;
    constants.counts[2] = compactDispatch.width;
    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (lease.capacities[tableIndex] == 0)
        {
            return false;
        }
        const uint32 rowStride = GetGPUSceneTableRowStride(tableIndex);
        const uint64 requiredBytes =
            static_cast<uint64>(lease.capacities[tableIndex]) * rowStride;
        if (rowStride == 0 || !lease.buffers[tableIndex] ||
            lease.buffers[tableIndex]->GetStride() != rowStride ||
            requiredBytes > lease.buffers[tableIndex]->GetSize())
        {
            return false;
        }
        if (tableIndex < 4)
        {
            constants.gpuSceneTableCounts0[tableIndex] =
                lease.capacities[tableIndex];
        }
        else
        {
            constants.gpuSceneTableCounts1[tableIndex - 4] =
                lease.capacities[tableIndex];
        }
    }

    if (!inputs->gpuSceneCandidateBuffer)
    {
        RHIBufferDesc candidateDesc;
        candidateDesc.size =
            static_cast<uint64>(m_config.maxInstances) * sizeof(GPUSceneCullingCandidate);
        candidateDesc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;
        candidateDesc.memoryType = RHIMemoryType::Upload;
        candidateDesc.stride = sizeof(GPUSceneCullingCandidate);
        candidateDesc.debugName = "GPUCulling.GPUSceneCandidateBuffer";
        inputs->gpuSceneCandidateBuffer = m_device->CreateBuffer(candidateDesc);
        if (!inputs->gpuSceneCandidateBuffer)
        {
            return false;
        }
        inputs->gpuSceneCandidateAccess = MakeRHIBufferAccessSnapshot(
            RHIResourceState::ShaderResource,
            RHIShaderStage::Compute,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Unknown);
    }

    if (!EnsureActiveRowsResidency() ||
        !EnsureGPUSceneCandidateResidency())
    {
        return false;
    }

    // A GPU-scene descriptor is usable only with constants written from the
    // same exact lease.  This seals all capacity reads before any later
    // consumer can dispatch these pipelines.
    const GPUQueueDomain lastGpuDomain =
        inputs->constantsAccess.uniformAccess.domain;
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Invalid);
    m_accessSnapshots.constants = inputs->constantsAccess;
    inputs->accessSnapshots.constants = inputs->constantsAccess;
    if (!UploadBufferData(
            inputs->constantsBuffer.Get(), &constants, sizeof(constants), nullptr))
    {
        return false;
    }
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    m_accessSnapshots.constants = inputs->constantsAccess;
    inputs->accessSnapshots.constants = inputs->constantsAccess;

    bool descriptorMatches = inputs->gpuSceneDescriptorSet &&
        inputs->gpuSceneLeaseVersion == lease.version &&
        inputs->gpuSceneTableCapacities == lease.capacities;
    if (descriptorMatches)
    {
        for (uint32 tableIndex = 0;
             tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
             ++tableIndex)
        {
            if (inputs->gpuSceneTableBuffers[tableIndex].Get() !=
                lease.buffers[tableIndex].Get())
            {
                descriptorMatches = false;
                break;
            }
        }
    }

    if (!descriptorMatches)
    {
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "GPUCulling.GPUSceneFrameSlotDescriptorSet";
        descriptorDesc.SetLayout(m_gpuSceneDescriptorSetLayout.Get())
            .BindBuffer(0, inputs->constantsBuffer.Get())
            .BindBuffer(1, inputs->gpuSceneCandidateBuffer.Get())
            .BindBuffer(2, inputs->instanceIndexBuffer.Get())
            .BindBuffer(3, inputs->visibilityBuffer.Get())
            .BindBuffer(4, inputs->visibleInstanceBuffer.Get())
            .BindBuffer(5, inputs->indirectBuffer.Get())
            .BindBuffer(6, inputs->drawCountBuffer.Get());
        for (uint32 tableIndex = 0;
             tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
             ++tableIndex)
        {
            descriptorDesc.BindBuffer(
                7 + tableIndex, lease.buffers[tableIndex].Get());
        }

        RHIDescriptorSetRef descriptorSet =
            m_device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            return false;
        }
        inputs->gpuSceneDescriptorSet = std::move(descriptorSet);
        inputs->gpuSceneTableBuffers = lease.buffers;
        inputs->gpuSceneTableCapacities = lease.capacities;
        inputs->gpuSceneLeaseVersion = lease.version;
    }

    RefreshActiveInputAccessSnapshots();
    return true;
}

bool GPUCulling::CullGPUScene(RHICommandContext& ctx,
                               const Mat4& viewMatrix,
                               const Mat4& projMatrix)
{
    m_visibleInstanceIndices.clear();
    m_rasterVisibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::PipelineResourcesUnavailable;

    GPUCullingFrameInputs* inputs = GetActiveFrameInputs();
    if (!m_gpuSceneEnabled ||
        !HasCompleteGPUSceneCandidates(m_gpuSceneLeaseVersion) ||
        m_gpuSceneLeaseVersion == 0 ||
        !IsGPUSceneExecutionReady() || !m_gpuSceneDescriptorSet ||
        inputs == nullptr || !inputs->constantsBuffer ||
        !inputs->gpuSceneCandidateBuffer ||
        !IsGPUSceneCandidateResident(*inputs) ||
        !m_visibilityBuffer || !m_visibleInstanceBuffer || !m_indirectBuffer ||
        !m_drawCountBuffer)
    {
        return false;
    }

    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (!m_gpuSceneTableBuffers[tableIndex] ||
            m_gpuSceneTableCapacities[tableIndex] == 0)
        {
            return false;
        }
    }

    const Mat4 viewProj = projMatrix * viewMatrix;
    const bool qualificationReferenceReady =
        !m_gpuSceneQualificationCapture.referencePrepared ||
        BuildGPUSceneQualificationReference(viewMatrix, projMatrix);
    GPUCullingConstants constants = MakeDefaultGPUCullingConstants();
    constants.viewProj = viewProj;
    ExtractFrustumPlanes(viewProj, constants.frustumPlanes);
    constants.cameraPosition = Vec4(inverse(viewMatrix)[3]);
    constants.params = Vec4(
        m_config.maxDrawDistance,
        0.0f,
        m_config.enableFrustumCulling ? 1.0f : 0.0f,
        m_config.enableDistanceCulling ? 1.0f : 0.0f);
    const uint32 drawGroupCount = static_cast<uint32>(m_drawGroups.size());
    const CompactDispatchDimensions compactDispatch =
        GetCompactDispatchDimensions(drawGroupCount);
    constants.counts[0] = m_incrementalDiagnostics.activeRowCount;
    constants.counts[1] = drawGroupCount;
    constants.counts[2] = compactDispatch.width;
    for (uint32 tableIndex = 0;
         tableIndex < RVX_GPU_SCENE_CULLING_TABLE_COUNT;
         ++tableIndex)
    {
        if (tableIndex < 4)
        {
            constants.gpuSceneTableCounts0[tableIndex] =
                m_gpuSceneTableCapacities[tableIndex];
        }
        else
        {
            constants.gpuSceneTableCounts1[tableIndex - 4] =
                m_gpuSceneTableCapacities[tableIndex];
        }
    }

    const GPUQueueDomain lastGpuDomain =
        inputs->constantsAccess.uniformAccess.domain;
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Invalid);
    m_accessSnapshots.constants = inputs->constantsAccess;
    inputs->accessSnapshots.constants = inputs->constantsAccess;
    if (!UploadBufferData(
            inputs->constantsBuffer.Get(), &constants, sizeof(constants), &ctx))
    {
        return false;
    }
    inputs->constantsAccess = MakeRHIBufferAccessSnapshot(
        RHIResourceState::ConstantBuffer,
        RHIShaderStage::Compute,
        lastGpuDomain,
        RHIContentValidity::Valid);
    m_accessSnapshots.constants = inputs->constantsAccess;
    inputs->accessSnapshots.constants = inputs->constantsAccess;

    ctx.SetPipeline(m_gpuSceneFrustumCullPipeline.Get());
    ctx.SetDescriptorSet(0, m_gpuSceneDescriptorSet.Get());

    const RHIAccessSnapshot computeUAVAccess = MakeRHIAccessSnapshot(
        RHIResourceState::UnorderedAccess,
        RHIShaderStage::Compute,
        GPUQueueDomain::Graphics);
    const RHIAccessSnapshot copyDestinationAccess = MakeRHIAccessSnapshot(
        RHIResourceState::CopyDest,
        RHIShaderStage::All,
        GPUQueueDomain::Graphics);
    if (!BuildGpuIndirectCommandPrefill() || !m_indirectBuffer)
    {
        return false;
    }
    ctx.BufferBarrier(
        m_indirectBuffer.Get(), computeUAVAccess, copyDestinationAccess);
    if (!UploadBufferData(
            m_indirectBuffer.Get(),
            m_indirectCommands.data(),
            static_cast<uint64>(m_indirectCommands.size()) *
                sizeof(IndirectDrawIndexedCommand),
            &ctx))
    {
        ctx.BufferBarrier(
            m_indirectBuffer.Get(), copyDestinationAccess, computeUAVAccess);
        return false;
    }
    ctx.BufferBarrier(
        m_indirectBuffer.Get(), copyDestinationAccess, computeUAVAccess);

    const uint32 clearThreadCount = std::max(
        m_incrementalDiagnostics.activeRowCount,
        drawGroupCount + 1u);
    const uint32 cullGroupCount = (clearThreadCount + 63u) / 64u;
    ctx.Dispatch(cullGroupCount, 1, 1);

    ctx.BufferBarrier(m_visibilityBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess);

    ctx.SetPipeline(m_gpuSceneCompactPipeline.Get());
    ctx.SetDescriptorSet(0, m_gpuSceneDescriptorSet.Get());
    ctx.Dispatch(compactDispatch.width, compactDispatch.height, 1);

    ctx.BufferBarrier(m_indirectBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.BufferBarrier(m_drawCountBuffer.Get(), computeUAVAccess, computeUAVAccess);
    ctx.SetPipeline(m_gpuSceneFinalizePipeline.Get());
    ctx.SetDescriptorSet(0, m_gpuSceneDescriptorSet.Get());
    const uint32 finalizeGroupCount =
        (drawGroupCount + 63u) / 64u;
    ctx.Dispatch(finalizeGroupCount, 1, 1);

    // The four copies intentionally follow the GPU-scene finalizer.  They
    // therefore observe the finalized per-group instance counts and draw
    // counters, not compaction's transient count values.
    if (qualificationReferenceReady &&
        !RecordGPUSceneQualificationReadback(ctx))
    {
        // Preserve the rendering result. This capture is expressly diagnostic
        // and a failed allocation/copy is reported as fail-closed evidence,
        // never as a hidden CPU fallback or alternate raster path.
        RVX_RENDER_WARN(
            "GPUCulling: GPU-scene qualification readback was not recorded");
    }

    m_usedGpuExecutionLastCull = true;
    m_lastFallbackReason = GPUCullingFallbackReason::None;
    m_stats.totalInstances = m_instanceCount;
    return true;
}

void GPUCulling::CullCpuFallback(const Mat4& viewMatrix, const Mat4& projMatrix)
{
    m_visibleInstanceIndices.clear();
    m_rasterVisibleInstanceIndices.clear();
    m_visibleSourceIndices.clear();
    m_indirectCommands.clear();
    std::fill(m_groupDrawCounts.begin(), m_groupDrawCounts.end(), 0);
    for (GPUCullingDrawGroup& group : m_drawGroups)
    {
        group.visibleDrawCount = 0;
    }
    m_drawCount = 0;
    m_usedCpuFallbackLastCull = false;
    m_usedGpuExecutionLastCull = false;
    m_lastFallbackReason = GPUCullingFallbackReason::None;

    if (m_instanceCount == 0)
    {
        m_stats = {};
        if (!UploadCullOutputs())
        {
            m_lastFallbackReason =
                GPUCullingFallbackReason::PipelineResourcesUnavailable;
            return;
        }

        m_usedCpuFallbackLastCull = true;
        return;
    }

    const Mat4 viewProj = projMatrix * viewMatrix;
    Vec4 frustumPlanes[6];
    ExtractFrustumPlanes(viewProj, frustumPlanes);
    BuildCpuCullResults(viewMatrix, frustumPlanes);
    if (!UploadCullOutputs())
    {
        m_lastFallbackReason =
            GPUCullingFallbackReason::PipelineResourcesUnavailable;
        return;
    }

    m_usedCpuFallbackLastCull = true;
}

GPUCullingIndexedIndirectSubmission GPUCulling::BuildIndexedIndirectSubmission(
    uint32 maxDrawCount) const
{
    GPUCullingIndexedIndirectSubmission submission;
    if (!m_usedGpuExecutionLastCull && !m_usedCpuFallbackLastCull)
    {
        return submission;
    }

    if (!m_indirectBuffer)
    {
        return submission;
    }

    if (m_drawGroups.size() > 1)
    {
        return submission;
    }

    submission.capabilities = m_device != nullptr
        ? &m_device->GetCapabilities()
        : nullptr;
    submission.execution.argumentBuffer = m_indirectBuffer.Get();
    submission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);
    submission.execution.requiresFirstInstance = true;

    if (m_usedGpuExecutionLastCull && m_drawCountBuffer)
    {
        submission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
        submission.execution.countBuffer = m_drawCountBuffer.Get();
        submission.execution.maxDrawCount = maxDrawCount > 0
            ? std::min(1u, maxDrawCount)
            : 1u;
        return submission;
    }

    submission.execution.mode = RHIIndirectExecutionMode::FixedCount;
    submission.execution.maxDrawCount = maxDrawCount > 0
        ? std::min(m_drawCount, maxDrawCount)
        : m_drawCount;
    return submission;
}

GPUCullingIndexedIndirectSubmission GPUCulling::BuildIndexedIndirectGroupSubmission(
    uint32 groupIndex) const
{
    GPUCullingIndexedIndirectSubmission submission;
    if ((!m_usedGpuExecutionLastCull && !m_usedCpuFallbackLastCull) ||
        !m_indirectBuffer || groupIndex >= m_drawGroups.size())
    {
        return submission;
    }

    const GPUCullingDrawGroup& group = m_drawGroups[groupIndex];
    submission.capabilities = m_device != nullptr
        ? &m_device->GetCapabilities()
        : nullptr;
    submission.execution.argumentBuffer = m_indirectBuffer.Get();
    submission.execution.argumentOffset =
        static_cast<uint64>(group.commandOffset) * sizeof(IndirectDrawIndexedCommand);
    submission.execution.commandStride = sizeof(IndirectDrawIndexedCommand);
    submission.execution.requiresFirstInstance = true;

    if (m_usedGpuExecutionLastCull && m_drawCountBuffer)
    {
        submission.execution.mode = RHIIndirectExecutionMode::CountBuffer;
        submission.execution.countBuffer = m_drawCountBuffer.Get();
        submission.execution.countOffset = group.countBufferOffset;
        submission.execution.maxDrawCount = 1;
        return submission;
    }

    submission.execution.mode = RHIIndirectExecutionMode::FixedCount;
    submission.execution.maxDrawCount = group.visibleDrawCount > 0 ? 1u : 0u;
    return submission;
}

// ============================================================================
// MeshletRenderer
// ============================================================================

MeshletRenderer::~MeshletRenderer()
{
    Shutdown();
}

void MeshletRenderer::Initialize(IRHIDevice* device)
{
    m_device = device;
}

void MeshletRenderer::Shutdown()
{
    m_meshletBuffer.Reset();
    m_vertexBuffer.Reset();
    m_indexBuffer.Reset();
    m_visibleMeshletBuffer.Reset();
    m_meshletCullPipeline.Reset();
    m_meshletDrawPipeline.Reset();
    m_device = nullptr;
}

void MeshletRenderer::GenerateMeshlets(
    const Vec3* vertices,
    uint32 vertexCount,
    const uint32* indices,
    uint32 indexCount,
    uint32 maxVertices,
    uint32 maxTriangles,
    std::vector<Meshlet>& outMeshlets)
{
    // Simple meshlet generation algorithm
    // For production, use meshoptimizer or similar

    outMeshlets.clear();

    uint32 triangleCount = indexCount / 3;
    uint32 currentTriangle = 0;

    while (currentTriangle < triangleCount)
    {
        Meshlet meshlet = {};
        meshlet.vertexOffset = 0;  // Would be calculated based on vertex deduplication
        meshlet.triangleOffset = currentTriangle * 3;
        meshlet.vertexCount = 0;
        meshlet.triangleCount = 0;

        // Calculate bounding sphere
        Vec3 center(0.0f);
        float radius = 0.0f;

        // Add triangles to meshlet. This simple generator does not deduplicate
        // vertices, so maxVertices is treated as a conservative triangle cap.
        const uint32 vertexLimitedTriangles = maxVertices > 0 ? std::max(1u, maxVertices / 3u) : maxTriangles;
        const uint32 trianglesPerMeshlet = std::max(1u, std::min(maxTriangles, vertexLimitedTriangles));
        uint32 trianglesToAdd = std::min(trianglesPerMeshlet, triangleCount - currentTriangle);
        meshlet.triangleCount = trianglesToAdd;

        // Calculate bounding sphere from vertices in this meshlet
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    center += vertices[idx];
                    meshlet.vertexCount++;
                }
            }
        }

        if (meshlet.vertexCount > 0)
        {
            center /= static_cast<float>(meshlet.vertexCount);
        }

        // Calculate radius
        for (uint32 t = 0; t < trianglesToAdd; ++t)
        {
            for (int v = 0; v < 3; ++v)
            {
                uint32 idx = indices[(currentTriangle + t) * 3 + v];
                if (idx < vertexCount)
                {
                    float dist = length(vertices[idx] - center);
                    radius = std::max(radius, dist);
                }
            }
        }

        meshlet.boundingSphere = Vec4(center, radius);

        // TODO: Calculate cone for backface culling

        outMeshlets.push_back(meshlet);
        currentTriangle += trianglesToAdd;
    }
}

void MeshletRenderer::Render(RHICommandContext& ctx,
                              const Mat4& viewMatrix,
                              const Mat4& projMatrix)
{
    (void)ctx;
    (void)viewMatrix;
    (void)projMatrix;

    // TODO: Implement meshlet rendering
    // 1. Cull meshlets using compute shader
    // 2. Generate indirect draw commands
    // 3. Execute mesh shader or indirect draws
}

} // namespace RVX
