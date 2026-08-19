#include "RenderGraphInternal.h"
#include "Core/Assert.h"
#include "Core/Job/JobSystem.h"
#include "Core/Log.h"
#include "Render/Graph/RenderGraphExecutor.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <unordered_map>
#include <vector>

namespace RVX
{
    namespace
    {
        bool AcquireTransientTexture(RenderGraphImpl& graph,
                                     TextureResource& resource)
        {
            if (!graph.transientResourcePool ||
                !graph.transientResourcePool->IsInitialized())
            {
                return false;
            }

            TransientTextureLease lease =
                graph.transientResourcePool->AcquireTextureLease(resource.desc);
            if (!lease)
            {
                return false;
            }

            resource.initialAccessSnapshot = std::move(lease.accessSnapshot);
            resource.initialState = ProjectRHIResourceState(
                resource.initialAccessSnapshot.uniformAccess);
            resource.pooledLease.emplace(std::move(lease));
            resource.binding = RGPhysicalBinding::Pooled;
            return true;
        }

        bool AcquireTransientBuffer(RenderGraphImpl& graph,
                                    BufferResource& resource)
        {
            if (!graph.transientResourcePool ||
                !graph.transientResourcePool->IsInitialized())
            {
                return false;
            }

            TransientBufferLease lease =
                graph.transientResourcePool->AcquireBufferLease(resource.desc);
            if (!lease)
            {
                return false;
            }

            resource.initialAccessSnapshot = std::move(lease.accessSnapshot);
            resource.initialState = ProjectRHIResourceState(
                resource.initialAccessSnapshot.uniformAccess);
            resource.pooledLease.emplace(std::move(lease));
            resource.binding = RGPhysicalBinding::Pooled;
            return true;
        }

        void RollbackRealizedResources(RenderGraphImpl& graph)
        {
            bool releasedAny = false;
            for (TextureViewResource& view : graph.textureViews)
            {
                releasedAny |= static_cast<bool>(view.realizedView);
                view.realizedView.Reset();
            }
            releasedAny |= !graph.executionResources.empty();
            graph.executionResources.clear();
            for (TextureResource& texture : graph.textures)
            {
                if (texture.pooledLease)
                {
                    static_cast<void>(
                        texture.pooledLease->AbortUnsubmitted());
                    texture.pooledLease.reset();
                    releasedAny = true;
                }
                if (!texture.imported)
                {
                    releasedAny |= static_cast<bool>(texture.strongBinding);
                    texture.strongBinding.Reset();
                    texture.binding = RGPhysicalBinding::Unrealized;
                }
            }
            for (BufferResource& buffer : graph.buffers)
            {
                if (buffer.pooledLease)
                {
                    static_cast<void>(
                        buffer.pooledLease->AbortUnsubmitted());
                    buffer.pooledLease.reset();
                    releasedAny = true;
                }
                if (!buffer.imported)
                {
                    releasedAny |= static_cast<bool>(buffer.strongBinding);
                    buffer.strongBinding.Reset();
                    buffer.binding = RGPhysicalBinding::Unrealized;
                }
            }
            for (TransientHeap& heap : graph.transientHeaps)
            {
                releasedAny |= static_cast<bool>(heap.heap);
                heap.heap.Reset();
            }
            graph.runtimeState = graph.stats.compileValid
                ? RenderGraphRuntimeState::Compiled
                : RenderGraphRuntimeState::CompileFailed;
            if (releasedAny)
                ++graph.stats.partialRealizationRollbackCount;
        }

        RHITextureAspect GetDefaultTextureAspect(const RHITextureDesc& desc)
        {
            return IsDepthFormat(desc.format) ? RHITextureAspect::Depth : RHITextureAspect::Color;
        }

        RHISubresourceRange AllSubresourcesForTexture(const RHITextureDesc& desc)
        {
            RHISubresourceRange range = RHISubresourceRange::All();
            range.aspect = GetDefaultTextureAspect(desc);
            return range;
        }

        RHIContentValidity MergeContentValidity(RHIContentValidity left,
                                                RHIContentValidity right)
        {
            return left == right ? left : RHIContentValidity::Unknown;
        }

        void ResetExecutionDiagnostics(RenderGraphImpl& graph)
        {
            graph.stats.lastExecutedPassCount = 0;
            graph.stats.lastExecutionCpuDurationNanoseconds = 0;
            graph.stats.parallelRecordingEnabled =
                graph.parallelRecordingEnabled;
            graph.stats.parallelRecordingUsed = false;
            graph.stats.lastParallelRecordingLevelCount = 0;
            graph.stats.lastParallelRecordingBatchCount = 0;
            graph.stats.accessSnapshotMismatchCount = 0;
            graph.stats.executionQueueMismatchCount = 0;
            graph.lastQueueSyncs.clear();

            for (Pass& pass : graph.passes)
            {
                pass.executedLastRun = false;
                pass.lastExecutionQueue = RenderGraph::DiagnosticExecutionQueue::Unknown;
                pass.lastExecutionSerial = RVX_INVALID_INDEX;
                pass.lastCpuDurationNanoseconds = 0;
            }
        }

        void ValidatePlannedAccessSources(RenderGraphImpl& graph)
        {
            std::unordered_map<uint32, RHIAccessSnapshot> textureAccesses;
            std::unordered_map<uint32, RHIAccessSnapshot> bufferAccesses;
            for (uint32 index = 0; index < graph.textures.size(); ++index)
            {
                textureAccesses[index] =
                    graph.textures[index].initialAccessSnapshot.uniformAccess;
            }
            for (uint32 index = 0; index < graph.buffers.size(); ++index)
            {
                bufferAccesses[index] =
                    graph.buffers[index].initialAccessSnapshot.uniformAccess;
            }

            const auto validatePass = [&](const Pass& pass)
            {
                if (pass.culled)
                    return;
                for (const ResourceUsage& usage : pass.usages)
                {
                    if (!usage.hasPlannedBeforeAccess)
                        continue;

                    RHIAccessSnapshot& realized = usage.type == ResourceType::Texture
                        ? textureAccesses[usage.index]
                        : bufferAccesses[usage.index];
                    if (realized != usage.plannedBeforeAccess)
                    {
                        ++graph.stats.accessSnapshotMismatchCount;
                        RVX_CORE_ERROR(
                            "RenderGraph scoped access mismatch before pass '{}': planned [{}], realized [{}]",
                            pass.name,
                            DescribeRHIAccessSnapshot(usage.plannedBeforeAccess),
                            DescribeRHIAccessSnapshot(realized));
                    }
                    realized = usage.desiredAccess;
                }
            };

            if (!graph.executionOrder.empty())
            {
                for (uint32 passIndex : graph.executionOrder)
                {
                    if (passIndex < graph.passes.size())
                        validatePass(graph.passes[passIndex]);
                }
            }
            else
            {
                for (const Pass& pass : graph.passes)
                    validatePass(pass);
            }
        }

        RHIAccessSnapshot ResolveTextureLeaseAccess(
            const TextureResource& resource,
            const RHISubresourceRange& range)
        {
            const RHITextureAccessSnapshot& snapshot =
                resource.initialAccessSnapshot;
            for (const RHITextureSubresourceAccessSnapshot& overrideAccess :
                 snapshot.subresourceOverrides)
            {
                const RHISubresourceRange& candidate = overrideAccess.range;
                const uint32 candidateMipCount = ResolveTextureMipLevelCount(
                    resource.desc, candidate);
                const uint32 candidateLayerCount =
                    ResolveTextureArrayLayerCount(resource.desc, candidate);
                const uint32 requestedMipCount = ResolveTextureMipLevelCount(
                    resource.desc, range);
                const uint32 requestedLayerCount =
                    ResolveTextureArrayLayerCount(resource.desc, range);
                if (range.baseMipLevel >= candidate.baseMipLevel &&
                    range.baseMipLevel + requestedMipCount <=
                        candidate.baseMipLevel + candidateMipCount &&
                    range.baseArrayLayer >= candidate.baseArrayLayer &&
                    range.baseArrayLayer + requestedLayerCount <=
                        candidate.baseArrayLayer + candidateLayerCount)
                {
                    return overrideAccess.access;
                }
            }
            return snapshot.uniformAccess;
        }

        RHIAccessSnapshot ResolveBufferLeaseAccess(
            const BufferResource& resource,
            uint64 offset,
            uint64 size)
        {
            const RHIBufferAccessSnapshot& snapshot =
                resource.initialAccessSnapshot;
            const uint64 resolvedSize = size == RVX_WHOLE_SIZE
                ? resource.desc.size - std::min(offset, resource.desc.size)
                : size;
            for (const RHIBufferRangeAccessSnapshot& overrideAccess :
                 snapshot.rangeOverrides)
            {
                const uint64 candidateSize =
                    overrideAccess.size == RVX_WHOLE_SIZE
                        ? resource.desc.size - std::min(
                              overrideAccess.offset, resource.desc.size)
                        : overrideAccess.size;
                if (offset >= overrideAccess.offset &&
                    offset + resolvedSize <=
                        overrideAccess.offset + candidateSize)
                {
                    return overrideAccess.access;
                }
            }
            return snapshot.uniformAccess;
        }

        bool MaterializeBarrier(const RenderGraphImpl& graph,
                                const PlannedTextureBarrier& planned,
                                RHITextureBarrier& barrier)
        {
            if (planned.resourceIndex >= graph.textures.size())
                return false;
            const TextureResource& resource =
                graph.textures[planned.resourceIndex];
            RHITexture* texture = resource.GetTexture();
            if (!texture)
                return false;
            barrier = planned.barrier;
            barrier.texture = texture;
            if (planned.resolveBeforeFromLease)
            {
                const RHIAccessSnapshot before = ResolveTextureLeaseAccess(
                    resource, barrier.subresourceRange);
                barrier = MakeRHITextureBarrier(
                    texture,
                    before,
                    barrier.accessAfter,
                    barrier.subresourceRange,
                    barrier.discardIntent);
            }
            return barrier.dependencyKind != RHIDependencyKind::None;
        }

        bool MaterializeBarrier(const RenderGraphImpl& graph,
                                const PlannedBufferBarrier& planned,
                                RHIBufferBarrier& barrier)
        {
            if (planned.resourceIndex >= graph.buffers.size())
                return false;
            const BufferResource& resource = graph.buffers[planned.resourceIndex];
            RHIBuffer* buffer = resource.GetBuffer();
            if (!buffer)
                return false;
            barrier = planned.barrier;
            barrier.buffer = buffer;
            if (planned.resolveBeforeFromLease)
            {
                const RHIAccessSnapshot before = ResolveBufferLeaseAccess(
                    resource, barrier.offset, barrier.size);
                barrier = MakeRHIBufferBarrier(
                    buffer,
                    before,
                    barrier.accessAfter,
                    barrier.offset,
                    barrier.size,
                    barrier.discardIntent);
            }
            return barrier.dependencyKind != RHIDependencyKind::None;
        }

        void ExecutePassOnContext(Pass& pass,
                                  RHICommandContext& ctx,
                                  RenderGraph::DiagnosticExecutionQueue queue,
                                  uint32 executionSerial,
                                  RenderGraphImpl& graph)
        {
            const auto beginTime = std::chrono::steady_clock::now();

            ctx.BeginEvent(pass.name.c_str());

            if (!pass.aliasingBarriers.empty())
            {
                std::vector<RHIResourceAliasingBarrier> aliasingBarriers;
                aliasingBarriers.reserve(pass.aliasingBarriers.size());
                const auto resolveResource = [&graph](ResourceType type,
                                                      uint32 index) -> RHIResource*
                {
                    if (type == ResourceType::Texture)
                    {
                        return index < graph.textures.size()
                            ? graph.textures[index].GetTexture()
                            : nullptr;
                    }
                    return index < graph.buffers.size()
                        ? graph.buffers[index].GetBuffer()
                        : nullptr;
                };
                for (const AliasingBarrier& barrier : pass.aliasingBarriers)
                {
                    RHIResourceAliasingBarrier nativeBarrier;
                    nativeBarrier.resourceBefore = resolveResource(
                        barrier.beforeType,
                        barrier.beforeResourceIndex);
                    nativeBarrier.resourceAfter = resolveResource(
                        barrier.afterType,
                        barrier.afterResourceIndex);
                    if (nativeBarrier.resourceBefore && nativeBarrier.resourceAfter)
                    {
                        aliasingBarriers.push_back(nativeBarrier);
                    }
                }
                ctx.AliasingBarriers(aliasingBarriers);
            }
            if (!pass.bufferBarriers.empty() || !pass.textureBarriers.empty())
            {
                std::vector<RHIBufferBarrier> bufferBarriers;
                std::vector<RHITextureBarrier> textureBarriers;
                bufferBarriers.reserve(pass.bufferBarriers.size());
                textureBarriers.reserve(pass.textureBarriers.size());
                for (const PlannedBufferBarrier& planned : pass.bufferBarriers)
                {
                    RHIBufferBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        bufferBarriers.push_back(std::move(barrier));
                }
                for (const PlannedTextureBarrier& planned : pass.textureBarriers)
                {
                    RHITextureBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        textureBarriers.push_back(std::move(barrier));
                }
                ctx.Barriers(bufferBarriers, textureBarriers);
            }
            if (pass.execute)
            {
                RVX_ASSERT_MSG(graph.owner != nullptr,
                               "RenderGraph execution has no owning graph");
                RenderGraphPassContext passContext(*graph.owner, ctx);
                pass.execute(passContext);
            }
            if (!pass.postBufferBarriers.empty() ||
                !pass.postTextureBarriers.empty())
            {
                std::vector<RHIBufferBarrier> bufferBarriers;
                std::vector<RHITextureBarrier> textureBarriers;
                bufferBarriers.reserve(pass.postBufferBarriers.size());
                textureBarriers.reserve(pass.postTextureBarriers.size());
                for (const PlannedBufferBarrier& planned :
                     pass.postBufferBarriers)
                {
                    RHIBufferBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        bufferBarriers.push_back(std::move(barrier));
                }
                for (const PlannedTextureBarrier& planned :
                     pass.postTextureBarriers)
                {
                    RHITextureBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        textureBarriers.push_back(std::move(barrier));
                }
                ctx.Barriers(bufferBarriers, textureBarriers);
            }
            ctx.EndEvent();

            const auto endTime = std::chrono::steady_clock::now();
            pass.executedLastRun = true;
            pass.lastExecutionQueue = queue;
            pass.lastExecutionSerial = executionSerial;
            pass.lastCpuDurationNanoseconds =
                static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime).count());
        }

        void AccumulateExecutionDiagnostics(RenderGraphImpl& graph)
        {
            graph.stats.lastExecutedPassCount = 0;
            graph.stats.lastExecutionCpuDurationNanoseconds = 0;
            for (const Pass& pass : graph.passes)
            {
                if (!pass.executedLastRun)
                    continue;
                ++graph.stats.lastExecutedPassCount;
                graph.stats.lastExecutionCpuDurationNanoseconds +=
                    pass.lastCpuDurationNanoseconds;
            }
        }

        void EmitExportBarriers(RenderGraphImpl& graph, RHICommandContext& ctx)
        {
            std::vector<RHIBufferBarrier> exportBufferBarriers;
            std::vector<RHITextureBarrier> exportTextureBarriers;

            for (auto& resource : graph.textures)
            {
                if (!resource.exportAccess || !resource.GetTexture())
                    continue;

                RHIAccessSnapshot desired = *resource.exportAccess;
                if (resource.hasSubresourceTracking)
                {
                    uint32 baseMip = 0;
                    uint32 mipCount = resource.desc.mipLevels;
                    uint32 baseLayer = 0;
                    uint32 layerCount = GetTexturePhysicalLayerCount(resource.desc);
                    bool hasRealizedValidity = false;
                    RHIContentValidity realizedValidity = RHIContentValidity::Unknown;
                    for (uint32 mip = baseMip; mip < baseMip + mipCount; ++mip)
                    {
                        for (uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer)
                        {
                            const uint32 key = mip + layer * resource.desc.mipLevels;
                            const auto it = resource.subresourceAccesses.find(key);
                            const RHIAccessSnapshot& current =
                                it != resource.subresourceAccesses.end()
                                    ? it->second
                                    : resource.currentAccessSnapshot.uniformAccess;
                            realizedValidity = hasRealizedValidity
                                ? MergeContentValidity(realizedValidity,
                                                       current.contentValidity)
                                : current.contentValidity;
                            hasRealizedValidity = true;
                        }
                    }
                    desired.contentValidity = hasRealizedValidity
                        ? realizedValidity
                        : resource.currentAccessSnapshot.uniformAccess.contentValidity;
                    for (uint32 mip = baseMip; mip < baseMip + mipCount; ++mip)
                    {
                        for (uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer)
                        {
                            uint32 key = mip + layer * resource.desc.mipLevels;
                            auto it = resource.subresourceAccesses.find(key);
                            const RHIAccessSnapshot current =
                                it != resource.subresourceAccesses.end()
                                    ? it->second
                                    : resource.currentAccessSnapshot.uniformAccess;
                            if (ClassifyRHIDependency(current, desired) !=
                                RHIDependencyKind::None)
                            {
                                exportTextureBarriers.push_back(
                                    MakeRHITextureBarrier(
                                        resource.GetTexture(),
                                        current,
                                        desired,
                                        RHISubresourceRange{mip, 1, layer, 1, GetDefaultTextureAspect(resource.desc)}));
                            }
                        }
                    }
                    resource.subresourceStates.clear();
                    resource.subresourceAccesses.clear();
                    resource.hasSubresourceTracking = false;
                    resource.currentAccessSnapshot.uniformAccess = desired;
                    resource.currentAccessSnapshot.subresourceOverrides.clear();
                    resource.currentState = ProjectRHIResourceState(desired);
                }
                else
                {
                    const RHIAccessSnapshot current =
                        resource.currentAccessSnapshot.uniformAccess;
                    desired.contentValidity = current.contentValidity;
                    if (ClassifyRHIDependency(current, desired) !=
                        RHIDependencyKind::None)
                    {
                        exportTextureBarriers.push_back(
                            MakeRHITextureBarrier(
                                resource.GetTexture(),
                                current,
                                desired,
                                AllSubresourcesForTexture(resource.desc)));
                    }
                    resource.currentAccessSnapshot.uniformAccess = desired;
                    resource.currentState = ProjectRHIResourceState(desired);
                }
            }

            for (auto& resource : graph.buffers)
            {
                if (!resource.exportAccess || !resource.GetBuffer())
                    continue;

                RHIAccessSnapshot desired = *resource.exportAccess;
                if (resource.hasRangeTracking)
                {
                    bool hasRealizedValidity = false;
                    RHIContentValidity realizedValidity = RHIContentValidity::Unknown;
                    for (const auto& range : resource.rangeStates)
                    {
                        realizedValidity = hasRealizedValidity
                            ? MergeContentValidity(realizedValidity,
                                                   range.access.contentValidity)
                            : range.access.contentValidity;
                        hasRealizedValidity = true;
                    }
                    desired.contentValidity = hasRealizedValidity
                        ? realizedValidity
                        : resource.currentAccessSnapshot.uniformAccess.contentValidity;
                    for (const auto& range : resource.rangeStates)
                    {
                        if (ClassifyRHIDependency(range.access, desired) !=
                            RHIDependencyKind::None)
                        {
                            exportBufferBarriers.push_back(
                                MakeRHIBufferBarrier(
                                    resource.GetBuffer(),
                                    range.access,
                                    desired,
                                    range.offset,
                                    range.size));
                        }
                    }
                    resource.rangeStates.clear();
                    resource.hasRangeTracking = false;
                    resource.currentAccessSnapshot.uniformAccess = desired;
                    resource.currentAccessSnapshot.rangeOverrides.clear();
                    resource.currentState = ProjectRHIResourceState(desired);
                }
                else
                {
                    const RHIAccessSnapshot current =
                        resource.currentAccessSnapshot.uniformAccess;
                    desired.contentValidity = current.contentValidity;
                    if (ClassifyRHIDependency(current, desired) !=
                        RHIDependencyKind::None)
                    {
                        exportBufferBarriers.push_back(
                            MakeRHIBufferBarrier(
                                resource.GetBuffer(),
                                current,
                                desired));
                    }
                    resource.currentAccessSnapshot.uniformAccess = desired;
                    resource.currentState = ProjectRHIResourceState(desired);
                }
            }

            if (!exportBufferBarriers.empty() || !exportTextureBarriers.empty())
            {
                ctx.Barriers(exportBufferBarriers, exportTextureBarriers);
            }
        }

        void EmitInitialQueueReleaseBarriers(
            const RenderGraphImpl& graph,
            RenderGraph::DiagnosticExecutionQueue queue,
            RHICommandContext& ctx)
        {
            const auto release = std::find_if(
                graph.initialQueueReleaseBatches.begin(),
                graph.initialQueueReleaseBatches.end(),
                [queue](const InitialQueueReleaseBatch& candidate)
                {
                    return candidate.queue == queue;
                });
            if (release != graph.initialQueueReleaseBatches.end() &&
                (!release->bufferBarriers.empty() ||
                 !release->textureBarriers.empty()))
            {
                std::vector<RHIBufferBarrier> bufferBarriers;
                std::vector<RHITextureBarrier> textureBarriers;
                bufferBarriers.reserve(release->bufferBarriers.size());
                textureBarriers.reserve(release->textureBarriers.size());
                for (const PlannedBufferBarrier& planned :
                     release->bufferBarriers)
                {
                    RHIBufferBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        bufferBarriers.push_back(std::move(barrier));
                }
                for (const PlannedTextureBarrier& planned :
                     release->textureBarriers)
                {
                    RHITextureBarrier barrier;
                    if (MaterializeBarrier(graph, planned, barrier))
                        textureBarriers.push_back(std::move(barrier));
                }
                ctx.Barriers(bufferBarriers, textureBarriers);
            }
        }

    } // namespace

    // =============================================================================
    // Create Transient Resources (with optional memory aliasing)
    // =============================================================================
    bool RealizeRenderGraphResources(RenderGraphImpl& graph)
    {
        if (graph.runtimeState ==
            RenderGraphRuntimeState::ResourcesRealized)
            return true;
        if (!graph.device)
        {
            const bool hasTransientTexture = std::any_of(
                graph.textures.begin(),
                graph.textures.end(),
                [](const TextureResource& texture)
                {
                    return !texture.imported;
                });
            const bool hasTransientBuffer = std::any_of(
                graph.buffers.begin(),
                graph.buffers.end(),
                [](const BufferResource& buffer)
                {
                    return !buffer.imported;
                });
            const bool resourcesRealized = !hasTransientTexture &&
                !hasTransientBuffer && graph.textureViews.empty();
            if (resourcesRealized)
            {
                graph.runtimeState =
                    RenderGraphRuntimeState::ResourcesRealized;
                ++graph.stats.physicalRealizationCount;
            }
            return resourcesRealized;
        }

        // If memory aliasing is enabled and heaps have been computed, use placed resources
        if (graph.enableMemoryAliasing && !graph.transientHeaps.empty())
        {
            // Create RHI Heaps
            for (auto& th : graph.transientHeaps)
            {
                if (!th.heap && th.size > 0)
                {
                    RHIHeapDesc heapDesc;
                    heapDesc.size = th.size;
                    heapDesc.type = RHIHeapType::Default;
                    heapDesc.flags = RHIHeapFlags::AllowAll;
                    heapDesc.debugName = "TransientHeap";

                    th.heap = graph.device->CreateHeap(heapDesc);
                    if (!th.heap)
                    {
                        RVX_CORE_WARN("RenderGraph: Failed to create transient heap, falling back to independent resources");
                    }
                }
            }

            // Create Placed Textures
            for (auto& texture : graph.textures)
            {
                if (texture.binding != RGPhysicalBinding::Unrealized)
                    continue;

                if (texture.alias.heapIndex < graph.transientHeaps.size() &&
                    graph.transientHeaps[texture.alias.heapIndex].heap)
                {
                    auto* heap = graph.transientHeaps[texture.alias.heapIndex].heap.Get();
                    texture.strongBinding = graph.device->CreatePlacedTexture(
                        heap,
                        texture.alias.heapOffset,
                        texture.desc);
                }

                // Fallback to independent resource if placed creation fails
                if (!texture.strongBinding)
                {
                    texture.strongBinding =
                        graph.device->CreateTexture(texture.desc);
                }
                if (texture.strongBinding)
                    texture.binding = RGPhysicalBinding::Owned;

                texture.initialState = RHIResourceState::Undefined;
                texture.initialAccessSnapshot = MakeRHITextureAccessSnapshot(
                    RHIResourceState::Undefined,
                    RHIShaderStage::None,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Invalid);
            }

            // Create Placed Buffers
            for (auto& buffer : graph.buffers)
            {
                if (buffer.binding != RGPhysicalBinding::Unrealized)
                    continue;

                if (buffer.alias.heapIndex < graph.transientHeaps.size() &&
                    graph.transientHeaps[buffer.alias.heapIndex].heap)
                {
                    auto* heap = graph.transientHeaps[buffer.alias.heapIndex].heap.Get();
                    buffer.strongBinding = graph.device->CreatePlacedBuffer(
                        heap,
                        buffer.alias.heapOffset,
                        buffer.desc);
                }

                // Fallback to independent resource if placed creation fails
                if (!buffer.strongBinding)
                {
                    buffer.strongBinding =
                        graph.device->CreateBuffer(buffer.desc);
                }
                if (buffer.strongBinding)
                    buffer.binding = RGPhysicalBinding::Owned;

                buffer.initialState = RHIResourceState::Undefined;
                buffer.initialAccessSnapshot = MakeRHIBufferAccessSnapshot(
                    RHIResourceState::Undefined,
                    RHIShaderStage::None,
                    GPUQueueDomain::Graphics,
                    RHIContentValidity::Invalid);
            }
        }
        else
        {
            // No aliasing: create independent resources
            for (auto& texture : graph.textures)
            {
                if (texture.binding == RGPhysicalBinding::Unrealized)
                {
                    if (!AcquireTransientTexture(graph, texture))
                    {
                        texture.strongBinding =
                            graph.device->CreateTexture(texture.desc);
                        if (texture.strongBinding)
                            texture.binding = RGPhysicalBinding::Owned;
                        texture.initialAccessSnapshot = MakeRHITextureAccessSnapshot(
                            RHIResourceState::Undefined,
                            RHIShaderStage::None,
                            GPUQueueDomain::Graphics,
                            RHIContentValidity::Invalid);
                        texture.initialState = RHIResourceState::Undefined;
                    }
                }
            }

            for (auto& buffer : graph.buffers)
            {
                if (buffer.binding == RGPhysicalBinding::Unrealized)
                {
                    if (!AcquireTransientBuffer(graph, buffer))
                    {
                        buffer.strongBinding =
                            graph.device->CreateBuffer(buffer.desc);
                        if (buffer.strongBinding)
                            buffer.binding = RGPhysicalBinding::Owned;
                        buffer.initialAccessSnapshot = MakeRHIBufferAccessSnapshot(
                            RHIResourceState::Undefined,
                            RHIShaderStage::None,
                            GPUQueueDomain::Graphics,
                            RHIContentValidity::Invalid);
                        buffer.initialState = RHIResourceState::Undefined;
                    }
                }
            }
        }

        bool viewsReady = true;
        for (TextureViewResource& view : graph.textureViews)
        {
            if (view.realizedView)
                continue;
            if (!view.texture.IsValid() || graph.owner == nullptr ||
                view.texture.graphIdentity != graph.owner->GetGraphIdentity() ||
                view.texture.recordingGeneration !=
                    graph.owner->GetRecordingGeneration() ||
                view.texture.index >= graph.textures.size())
            {
                viewsReady = false;
                break;
            }

            RHITexture* texture =
                graph.textures[view.texture.index].GetTexture();
            if (!texture)
            {
                viewsReady = false;
                break;
            }
            RHITextureViewDesc desc = view.desc;
            if (desc.format == RHIFormat::Unknown)
                desc.format = texture->GetFormat();
            if (!view.debugName.empty())
                desc.debugName = view.debugName.c_str();
            TextureResource& source = graph.textures[view.texture.index];
            view.realizedView = source.pooledLease
                ? source.pooledLease->GetOrCreateView(desc)
                : graph.device->CreateTextureView(texture, desc);
            if (!view.realizedView)
            {
                viewsReady = false;
                break;
            }
        }

        const bool texturesReady = std::all_of(
            graph.textures.begin(),
            graph.textures.end(),
            [](const TextureResource& texture)
            {
                return texture.GetTexture() != nullptr;
            });
        const bool buffersReady = std::all_of(
            graph.buffers.begin(),
            graph.buffers.end(),
            [](const BufferResource& buffer)
            {
                return buffer.GetBuffer() != nullptr;
            });
        const bool resourcesRealized =
            texturesReady && buffersReady && viewsReady;
        if (!resourcesRealized)
        {
            RVX_CORE_ERROR(
                "RenderGraph physical resource realization failed closed");
            RollbackRealizedResources(graph);
        }
        else
        {
            graph.runtimeState = RenderGraphRuntimeState::ResourcesRealized;
            ++graph.stats.physicalRealizationCount;
            // The compiler records a symbolic lease-before edge. Once the
            // physical lease is known, bind the diagnostic/source snapshot
            // without changing the already compiled desired accesses.
            std::vector<uint8> firstTextureUse(graph.textures.size(), 1);
            std::vector<uint8> firstBufferUse(graph.buffers.size(), 1);
            const auto bindPass = [&](Pass& pass)
            {
                if (pass.culled)
                    return;
                for (ResourceUsage& usage : pass.usages)
                {
                    if (usage.type == ResourceType::Texture &&
                        usage.index < graph.textures.size() &&
                        !graph.textures[usage.index].imported &&
                        firstTextureUse[usage.index] != 0)
                    {
                        usage.plannedBeforeAccess = graph.textures[usage.index]
                            .initialAccessSnapshot.uniformAccess;
                        usage.hasPlannedBeforeAccess = true;
                        firstTextureUse[usage.index] = 0;
                    }
                    else if (usage.type == ResourceType::Buffer &&
                             usage.index < graph.buffers.size() &&
                             !graph.buffers[usage.index].imported &&
                             firstBufferUse[usage.index] != 0)
                    {
                        usage.plannedBeforeAccess = graph.buffers[usage.index]
                            .initialAccessSnapshot.uniformAccess;
                        usage.hasPlannedBeforeAccess = true;
                        firstBufferUse[usage.index] = 0;
                    }
                }
            };
            for (uint32 passIndex : graph.executionOrder)
            {
                if (passIndex < graph.passes.size())
                    bindPass(graph.passes[passIndex]);
            }
        }
        return resourcesRealized;
    }


    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx)
    {
        ResetExecutionDiagnostics(graph);

        if (!graph.stats.compileValid)
        {
            RVX_CORE_ERROR("RenderGraph execution skipped because the graph did not compile successfully");
            return;
        }

        if (!RealizeRenderGraphResources(graph))
        {
            RVX_CORE_ERROR(
                "RenderGraph execution skipped because physical resource realization failed");
            return;
        }

        for (const Pass& pass : graph.passes)
        {
            if (!pass.culled &&
                pass.plannedExecutionQueue !=
                    RenderGraph::DiagnosticExecutionQueue::Graphics)
            {
                ++graph.stats.executionQueueMismatchCount;
                RVX_CORE_ERROR(
                    "RenderGraph graphics-only execution rejected pass '{}' planned for a non-Graphics queue",
                    pass.name);
            }
        }
        if (graph.stats.executionQueueMismatchCount != 0)
        {
            RollbackRealizedResources(graph);
            return;
        }

        ValidatePlannedAccessSources(graph);

        try
        {
            if (!graph.executionOrder.empty())
            {
                uint32 executionSerial = 0;
                for (uint32 passIndex : graph.executionOrder)
                {
                    auto& pass = graph.passes[passIndex];
                    if (pass.culled)
                        continue;

                    ExecutePassOnContext(
                        pass,
                        ctx,
                        RenderGraph::DiagnosticExecutionQueue::Graphics,
                        executionSerial++,
                        graph);
                }
            }
            else
            {
                uint32 executionSerial = 0;
                for (auto& pass : graph.passes)
                {
                    if (pass.culled)
                        continue;
                    ExecutePassOnContext(
                        pass,
                        ctx,
                        RenderGraph::DiagnosticExecutionQueue::Graphics,
                        executionSerial++,
                        graph);
                }
            }
            EmitExportBarriers(graph, ctx);
        }
        catch (const std::exception& error)
        {
            RVX_CORE_ERROR(
                "RenderGraph command recording failed: {}", error.what());
            RollbackRealizedResources(graph);
            return;
        }
        catch (...)
        {
            RVX_CORE_ERROR(
                "RenderGraph command recording failed with an unknown exception");
            RollbackRealizedResources(graph);
            return;
        }
        AccumulateExecutionDiagnostics(graph);
        graph.runtimeState = RenderGraphRuntimeState::Recorded;
    }

    bool RecordRenderGraphQueueSubmission(
        RenderGraphImpl& graph,
        RenderGraph::RecordedQueueSubmission& submission)
    {
        submission = {};
        ResetExecutionDiagnostics(graph);
        if (!graph.stats.compileValid ||
            graph.queueExecutionMode !=
                RenderGraph::QueueExecutionMode::MultiQueue ||
            !graph.device ||
            !graph.device->GetCapabilities().supportsQueueSubmissionPlan)
        {
            return false;
        }

        const RenderGraph::SubmissionPlan planned =
            BuildRenderGraphSubmissionPlan(graph);
        if (planned.queueBatches.empty() ||
            planned.terminalGraphicsBatchIndex == RVX_INVALID_INDEX)
        {
            return false;
        }

        ValidatePlannedAccessSources(graph);
        submission.plan.batches.reserve(planned.queueBatches.size());
        submission.ownedContexts.reserve(planned.queueBatches.size());

        for (const RenderGraph::PlannedQueueBatchDiagnostic& plannedBatch :
             planned.queueBatches)
        {
            RHICommandQueueType queueType = RHICommandQueueType::Graphics;
            switch (plannedBatch.queue)
            {
                case RenderGraph::DiagnosticExecutionQueue::Compute:
                    queueType = RHICommandQueueType::Compute;
                    break;
                case RenderGraph::DiagnosticExecutionQueue::Copy:
                    queueType = RHICommandQueueType::Copy;
                    break;
                case RenderGraph::DiagnosticExecutionQueue::Graphics:
                    queueType = RHICommandQueueType::Graphics;
                    break;
                case RenderGraph::DiagnosticExecutionQueue::Unknown:
                default:
                    RVX_CORE_ERROR(
                        "RenderGraph queue plan contains an unknown execution queue");
                    submission = {};
                    return false;
            }

            if (std::any_of(
                    plannedBatch.passIndices.begin(),
                    plannedBatch.passIndices.end(),
                    [&graph](uint32 passIndex)
                    {
                        return passIndex >= graph.passes.size();
                    }))
            {
                RVX_CORE_ERROR(
                    "RenderGraph queue batch {} references an invalid pass",
                    plannedBatch.batchIndex);
                submission = {};
                return false;
            }

            RHICommandContextRef context =
                graph.device->CreateCommandContext(queueType);
            if (!context || context->GetQueueType() != queueType)
            {
                RVX_CORE_ERROR(
                    "RenderGraph failed to create a context for queue batch {}",
                    plannedBatch.batchIndex);
                submission = {};
                return false;
            }

            RHIQueueSubmissionBatch batch;
            batch.queueType = queueType;
            batch.contexts.push_back(context.Get());
            batch.prerequisiteBatchIndices =
                plannedBatch.prerequisiteBatchIndices;
            submission.ownedContexts.push_back(std::move(context));
            submission.plan.batches.push_back(std::move(batch));
        }
        submission.plan.terminalGraphicsBatchIndex =
            planned.terminalGraphicsBatchIndex;

        const RHIQueueSubmissionPlanValidationResult validation =
            ValidateRHIQueueSubmissionPlan(submission.plan);
        if (!validation)
        {
            RVX_CORE_ERROR(
                "RenderGraph produced an invalid RHI queue plan: {}",
                validation.message);
            submission = {};
            return false;
        }

        // All queue contexts and the final DAG are valid before the first
        // physical resource lease is acquired. Resource realization is a
        // one-shot execution concern, never a compiler side effect.
        if (!RealizeRenderGraphResources(graph))
        {
            submission = {};
            return false;
        }

        // All fallible plan construction and validation completes before any
        // pass callback records commands. A GraphicsOnly fallback can
        // therefore never execute a pass callback twice. Serial identities are
        // assigned up front so diagnostics remain deterministic when batches
        // in one dependency level record concurrently.
        std::vector<uint32> batchFirstExecutionSerial(
            planned.queueBatches.size(), 0U);
        uint32 nextExecutionSerial = 0;
        for (uint32 batchIndex = 0;
             batchIndex < static_cast<uint32>(planned.queueBatches.size());
             ++batchIndex)
        {
            batchFirstExecutionSerial[batchIndex] = nextExecutionSerial;
            for (uint32 passIndex : planned.queueBatches[batchIndex].passIndices)
            {
                if (!graph.passes[passIndex].culled)
                    ++nextExecutionSerial;
            }
        }

        const auto recordBatch =
            [&](uint32 batchIndex)
        {
            const RenderGraph::PlannedQueueBatchDiagnostic& plannedBatch =
                planned.queueBatches[batchIndex];
            RHICommandContextRef& context =
                submission.ownedContexts[batchIndex];
            context->Reset();
            context->Begin();
            if (plannedBatch.syntheticInitialRelease)
            {
                EmitInitialQueueReleaseBarriers(
                    graph, plannedBatch.queue, *context);
            }
            uint32 executionSerial = batchFirstExecutionSerial[batchIndex];
            for (uint32 passIndex : plannedBatch.passIndices)
            {
                Pass& pass = graph.passes[passIndex];
                if (!pass.culled)
                {
                    ExecutePassOnContext(pass,
                                         *context,
                                         plannedBatch.queue,
                                         executionSerial++,
                                         graph);
                }
            }
            if (plannedBatch.syntheticTerminal)
                EmitExportBarriers(graph, *context);
            context->End();
        };

        JobSystem& jobs = JobSystem::Get();
        try
        {
            uint32 levelBegin = 0;
            while (levelBegin < planned.queueBatches.size())
            {
                const uint32 dependencyLevel =
                    planned.queueBatches[levelBegin].dependencyLevel;
                uint32 levelEnd = levelBegin + 1U;
                while (levelEnd < planned.queueBatches.size() &&
                       planned.queueBatches[levelEnd].dependencyLevel ==
                           dependencyLevel)
                {
                    ++levelEnd;
                }

                const bool recordInParallel = graph.parallelRecordingEnabled &&
                                              jobs.IsInitialized() &&
                                              jobs.GetWorkerCount() > 1U &&
                                              levelEnd - levelBegin > 1U;
                if (recordInParallel)
                {
                    std::vector<std::future<void>> recordings;
                    recordings.reserve(levelEnd - levelBegin);
                    for (uint32 batchIndex = levelBegin;
                         batchIndex < levelEnd;
                         ++batchIndex)
                    {
                        recordings.push_back(jobs.SubmitWithResult(
                            [&, batchIndex]() { recordBatch(batchIndex); }));
                    }

                    std::exception_ptr firstFailure;
                    for (std::future<void>& recording : recordings)
                    {
                        try
                        {
                            recording.get();
                        }
                        catch (...)
                        {
                            if (!firstFailure)
                                firstFailure = std::current_exception();
                        }
                    }
                    if (firstFailure)
                        std::rethrow_exception(firstFailure);

                    graph.stats.parallelRecordingUsed = true;
                    ++graph.stats.lastParallelRecordingLevelCount;
                    graph.stats.lastParallelRecordingBatchCount +=
                        levelEnd - levelBegin;
                }
                else
                {
                    for (uint32 batchIndex = levelBegin;
                         batchIndex < levelEnd;
                         ++batchIndex)
                    {
                        recordBatch(batchIndex);
                    }
                }
                levelBegin = levelEnd;
            }
        }
        catch (const std::exception& error)
        {
            RVX_CORE_ERROR(
                "RenderGraph queue recording failed: {}", error.what());
            RollbackRealizedResources(graph);
            submission = {};
            return false;
        }
        catch (...)
        {
            RVX_CORE_ERROR(
                "RenderGraph queue recording failed with an unknown exception");
            RollbackRealizedResources(graph);
            submission = {};
            return false;
        }

        graph.stats.asyncComputeScheduledPasses = 0;
        graph.stats.asyncGraphicsScheduledPasses = 0;
        for (const Pass& pass : graph.passes)
        {
            if (!pass.executedLastRun)
                continue;
            if (pass.lastExecutionQueue ==
                RenderGraph::DiagnosticExecutionQueue::Compute)
            {
                ++graph.stats.asyncComputeScheduledPasses;
            }
            else
            {
                ++graph.stats.asyncGraphicsScheduledPasses;
            }
        }
        graph.stats.asyncCrossQueueDependencyCount =
            planned.crossQueueSyncCount;
        graph.stats.asyncFinalQueueJoinCount = static_cast<uint32>(
            std::count_if(
                planned.queueSyncs.begin(),
                planned.queueSyncs.end(),
                [](const RenderGraph::PlannedQueueSyncDiagnostic& sync)
                {
                    return sync.reason ==
                        RenderGraph::DiagnosticSyncReason::FinalQueueJoin;
                }));
        graph.stats.asyncFallbackUsed = false;
        graph.stats.asyncFallbackReason =
            RenderGraph::AsyncComputeFallbackReason::None;
        AccumulateExecutionDiagnostics(graph);
        graph.runtimeState = RenderGraphRuntimeState::Recorded;
        return true;
    }

    RenderGraphExecution RenderGraphExecutor::Prepare(
        CompiledRenderGraphPlan& plan,
        const RenderGraphExecutionEnvironment& environment) const
    {
        RenderGraphExecution execution;
        RenderGraph* graph = plan.m_graph;
        const bool validPlan = graph != nullptr &&
            plan.m_graphIdentity == graph->GetGraphIdentity() &&
            plan.m_recordingGeneration == graph->GetRecordingGeneration() &&
            plan.m_planHash == graph->GetCompileStats().planHash;

        // A compiled plan is a one-shot execution capability. Consuming it on
        // failure prevents callers from retrying after partial external state.
        plan.m_graph = nullptr;
        if (!validPlan)
        {
            RVX_CORE_ERROR(
                "RenderGraph executor rejected a stale or foreign compiled plan");
            return execution;
        }
        return graph->PrepareExecutionInternal(environment);
    }

} // namespace RVX
