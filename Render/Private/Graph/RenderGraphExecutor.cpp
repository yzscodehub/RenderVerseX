#include "RenderGraphInternal.h"
#include "Core/Log.h"

#include <algorithm>
#include <chrono>
#include <vector>

namespace RVX
{
    namespace
    {
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

        void ResetExecutionDiagnostics(RenderGraphImpl& graph)
        {
            graph.stats.lastExecutedPassCount = 0;
            graph.stats.lastExecutionCpuDurationNanoseconds = 0;
            graph.lastQueueSyncs.clear();

            for (Pass& pass : graph.passes)
            {
                pass.executedLastRun = false;
                pass.lastExecutionQueue = RenderGraph::DiagnosticExecutionQueue::Unknown;
                pass.lastExecutionSerial = RVX_INVALID_INDEX;
                pass.lastCpuDurationNanoseconds = 0;
            }
        }

        void ExecutePassOnContext(Pass& pass,
                                  RHICommandContext& ctx,
                                  RenderGraph::DiagnosticExecutionQueue queue,
                                  uint32 executionSerial,
                                  RenderGraphImpl& graph)
        {
            const auto beginTime = std::chrono::steady_clock::now();

            ctx.BeginEvent(pass.name.c_str());

            // Note: Aliasing barriers for placed resources are currently handled
            // implicitly through Undefined -> desired state transitions.
            if (!pass.bufferBarriers.empty() || !pass.textureBarriers.empty())
            {
                ctx.Barriers(pass.bufferBarriers, pass.textureBarriers);
            }
            if (pass.execute)
            {
                pass.execute(ctx);
            }
            ctx.EndEvent();

            const auto endTime = std::chrono::steady_clock::now();
            pass.executedLastRun = true;
            pass.lastExecutionQueue = queue;
            pass.lastExecutionSerial = executionSerial;
            pass.lastCpuDurationNanoseconds =
                static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - beginTime).count());
            graph.stats.lastExecutedPassCount++;
            graph.stats.lastExecutionCpuDurationNanoseconds += pass.lastCpuDurationNanoseconds;
        }

        void EmitExportBarriers(RenderGraphImpl& graph, RHICommandContext& ctx)
        {
            std::vector<RHIBufferBarrier> exportBufferBarriers;
            std::vector<RHITextureBarrier> exportTextureBarriers;

            for (auto& resource : graph.textures)
            {
                if (!resource.exportState || !resource.GetTexture())
                    continue;

                RHIResourceState desired = *resource.exportState;
                if (resource.hasSubresourceTracking)
                {
                    uint32 baseMip = 0;
                    uint32 mipCount = resource.desc.mipLevels;
                    uint32 baseLayer = 0;
                    uint32 layerCount = resource.desc.arraySize;
                    for (uint32 mip = baseMip; mip < baseMip + mipCount; ++mip)
                    {
                        for (uint32 layer = baseLayer; layer < baseLayer + layerCount; ++layer)
                        {
                            uint32 key = mip + layer * resource.desc.mipLevels;
                            auto it = resource.subresourceStates.find(key);
                            RHIResourceState current = (it != resource.subresourceStates.end()) ? it->second : resource.currentState;
                            if (current != desired)
                            {
                                exportTextureBarriers.push_back(
                                    {resource.GetTexture(),
                                     current,
                                     desired,
                                     RHISubresourceRange{mip, 1, layer, 1, GetDefaultTextureAspect(resource.desc)}});
                            }
                        }
                    }
                    resource.subresourceStates.clear();
                    resource.hasSubresourceTracking = false;
                    resource.currentState = desired;
                }
                else if (resource.currentState != desired)
                {
                    exportTextureBarriers.push_back(
                        {resource.GetTexture(),
                         resource.currentState,
                         desired,
                         AllSubresourcesForTexture(resource.desc)});
                    resource.currentState = desired;
                }
            }

            for (auto& resource : graph.buffers)
            {
                if (!resource.exportState || !resource.GetBuffer())
                    continue;

                RHIResourceState desired = *resource.exportState;
                if (resource.hasRangeTracking)
                {
                    for (const auto& range : resource.rangeStates)
                    {
                        if (range.state != desired)
                        {
                            exportBufferBarriers.push_back(
                                {resource.GetBuffer(), range.state, desired, range.offset, range.size});
                        }
                    }
                    resource.rangeStates.clear();
                    resource.hasRangeTracking = false;
                    resource.currentState = desired;
                }
                else if (resource.currentState != desired)
                {
                    exportBufferBarriers.push_back(
                        {resource.GetBuffer(), resource.currentState, desired, 0, RVX_WHOLE_SIZE});
                    resource.currentState = desired;
                }
            }

            if (!exportBufferBarriers.empty() || !exportTextureBarriers.empty())
            {
                ctx.Barriers(exportBufferBarriers, exportTextureBarriers);
            }
        }

        uint32 CountEligibleAsyncComputePasses(const RenderGraphImpl& graph)
        {
            uint32 count = 0;
            for (uint32 passIndex : graph.executionOrder)
            {
                if (passIndex >= graph.passes.size())
                    continue;

                const Pass& pass = graph.passes[passIndex];
                if (!pass.culled && pass.type == RenderGraphPassType::Compute)
                {
                    ++count;
                }
            }
            return count;
        }

        bool RunsOnComputeQueue(const Pass& pass)
        {
            return pass.type == RenderGraphPassType::Compute;
        }

        struct PlannedQueueSyncRequirements
        {
            std::vector<uint8> requiresGraphicsForCompute;
            std::vector<uint8> requiresComputeForGraphics;
        };

        PlannedQueueSyncRequirements BuildPlannedQueueSyncRequirements(
            const RenderGraph::SubmissionPlan& submissionPlan,
            uint32 passCount)
        {
            PlannedQueueSyncRequirements requirements;
            requirements.requiresGraphicsForCompute.resize(passCount, 0);
            requirements.requiresComputeForGraphics.resize(passCount, 0);

            auto markTargetBatch = [&](const RenderGraph::PlannedQueueSyncDiagnostic& sync,
                                       std::vector<uint8>& targetRequirements)
            {
                if (sync.targetBatchIndex < submissionPlan.queueBatches.size())
                {
                    const RenderGraph::PlannedQueueBatchDiagnostic& targetBatch =
                        submissionPlan.queueBatches[sync.targetBatchIndex];
                    for (uint32 targetPassIndex : targetBatch.passIndices)
                    {
                        if (targetPassIndex < targetRequirements.size())
                        {
                            targetRequirements[targetPassIndex] = 1;
                        }
                    }
                    return;
                }

                if (sync.targetPassIndex < targetRequirements.size())
                {
                    targetRequirements[sync.targetPassIndex] = 1;
                }
            };

            for (const RenderGraph::PlannedQueueSyncDiagnostic& sync : submissionPlan.queueSyncs)
            {
                if (sync.reason != RenderGraph::DiagnosticSyncReason::CrossQueueDependency)
                    continue;

                if (sync.sourceQueue == RenderGraph::DiagnosticExecutionQueue::Graphics &&
                    sync.targetQueue == RenderGraph::DiagnosticExecutionQueue::Compute)
                {
                    markTargetBatch(sync, requirements.requiresGraphicsForCompute);
                }
                else if (sync.sourceQueue == RenderGraph::DiagnosticExecutionQueue::Compute &&
                         sync.targetQueue == RenderGraph::DiagnosticExecutionQueue::Graphics)
                {
                    markTargetBatch(sync, requirements.requiresComputeForGraphics);
                }
            }

            return requirements;
        }

        bool HasPlannedQueueRequirement(const std::vector<uint8>& requirements, uint32 passIndex)
        {
            return passIndex < requirements.size() && requirements[passIndex] != 0;
        }

        RenderGraph::AsyncComputeFallbackReason GetAsyncFallbackReason(
            const RenderGraphImpl& graph,
            RHICommandContext* computeCtx,
            RHIFence* computeFence,
            uint32 eligibleComputePasses)
        {
            if (!graph.stats.compileValid)
                return RenderGraph::AsyncComputeFallbackReason::GraphNotCompiled;

            if (eligibleComputePasses == 0)
                return RenderGraph::AsyncComputeFallbackReason::NoEligibleComputePasses;

            if (!graph.device || !graph.device->GetCapabilities().supportsAsyncCompute)
                return RenderGraph::AsyncComputeFallbackReason::BackendUnsupported;

            const RHICapabilities& caps = graph.device->GetCapabilities();
            if (!caps.supportsExplicitQueueFenceSignal)
                return RenderGraph::AsyncComputeFallbackReason::QueueFenceSignalUnsupported;

            if (!caps.supportsQueueFenceWait)
                return RenderGraph::AsyncComputeFallbackReason::QueueFenceWaitUnsupported;

            if (!computeCtx)
                return RenderGraph::AsyncComputeFallbackReason::MissingComputeContext;

            if (!computeFence)
                return RenderGraph::AsyncComputeFallbackReason::MissingFence;

            return RenderGraph::AsyncComputeFallbackReason::None;
        }
    } // namespace

    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx)
    {
        ResetExecutionDiagnostics(graph);

        if (!graph.stats.compileValid)
        {
            RVX_CORE_ERROR("RenderGraph execution skipped because the graph did not compile successfully");
            return;
        }

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

    void ExecuteRenderGraphAsync(RenderGraphImpl& graph,
                                  RHICommandContext& graphicsCtx,
                                  RHICommandContext* computeCtx,
                                  RHIFence* computeFence,
                                  uint64 frameIndex)
    {
        ResetExecutionDiagnostics(graph);

        const RenderGraph::SubmissionPlan submissionPlan = BuildRenderGraphSubmissionPlan(graph);
        const PlannedQueueSyncRequirements plannedSyncRequirements =
            BuildPlannedQueueSyncRequirements(submissionPlan, static_cast<uint32>(graph.passes.size()));

        graph.stats.asyncComputeEligiblePasses = CountEligibleAsyncComputePasses(graph);
        graph.stats.asyncComputeScheduledPasses = 0;
        graph.stats.asyncGraphicsScheduledPasses = 0;
        graph.stats.asyncFenceSignalCount = 0;
        graph.stats.asyncFenceWaitCount = 0;
        graph.stats.asyncCrossQueueDependencyCount = submissionPlan.crossQueueSyncCount;
        graph.stats.asyncFinalQueueJoinCount = 0;
        graph.stats.asyncComputeSupported =
            graph.device &&
            graph.device->GetCapabilities().supportsAsyncCompute &&
            graph.device->GetCapabilities().supportsExplicitQueueFenceSignal &&
            graph.device->GetCapabilities().supportsQueueFenceWait;

        RenderGraph::AsyncComputeFallbackReason fallbackReason =
            GetAsyncFallbackReason(graph, computeCtx, computeFence, graph.stats.asyncComputeEligiblePasses);

        if (fallbackReason != RenderGraph::AsyncComputeFallbackReason::None)
        {
            graph.stats.asyncFallbackUsed = true;
            graph.stats.asyncFallbackReason = fallbackReason;
            ExecuteRenderGraph(graph, graphicsCtx);
            return;
        }

        graph.stats.asyncFallbackUsed = false;
        graph.stats.asyncFallbackReason = RenderGraph::AsyncComputeFallbackReason::None;

        const uint64 fenceBaseValue = (frameIndex + 1u) << 32u;
        uint64 nextFenceValue = fenceBaseValue;
        std::vector<int32> passExecutionOrder(graph.passes.size(), -1);
        for (uint32 order = 0; order < static_cast<uint32>(graph.executionOrder.size()); ++order)
        {
            uint32 passIndex = graph.executionOrder[order];
            if (passIndex < passExecutionOrder.size())
            {
                passExecutionOrder[passIndex] = static_cast<int32>(order);
            }
        }

        int32 graphicsLastRecordedOrder = -1;
        int32 computeLastRecordedOrder = -1;
        uint32 graphicsLastRecordedPassIndex = RVX_INVALID_INDEX;
        uint32 computeLastRecordedPassIndex = RVX_INVALID_INDEX;
        int32 graphicsVisibleToComputeOrder = -1;
        int32 computeVisibleToGraphicsOrder = -1;
        uint32 executionSerial = 0;

        auto signalGraphicsForCompute = [&](uint32 targetPassIndex)
        {
            if (graphicsLastRecordedOrder < 0)
                return;

            ++nextFenceValue;
            graphicsCtx.SignalFence(computeFence, nextFenceValue);
            computeCtx->WaitFence(computeFence, nextFenceValue);
            graph.lastQueueSyncs.push_back(
                {RenderGraph::DiagnosticExecutionQueue::Graphics,
                 RenderGraph::DiagnosticExecutionQueue::Compute,
                 RenderGraph::DiagnosticSyncReason::CrossQueueDependency,
                 nextFenceValue,
                 graphicsLastRecordedPassIndex,
                 targetPassIndex});
            graph.stats.asyncFenceSignalCount++;
            graph.stats.asyncFenceWaitCount++;
            graphicsVisibleToComputeOrder = graphicsLastRecordedOrder;
        };

        auto signalComputeForGraphics = [&](uint32 targetPassIndex, bool finalQueueJoin)
        {
            if (computeLastRecordedOrder < 0)
                return;

            ++nextFenceValue;
            computeCtx->SignalFence(computeFence, nextFenceValue);
            graphicsCtx.WaitFence(computeFence, nextFenceValue);
            graph.lastQueueSyncs.push_back(
                {RenderGraph::DiagnosticExecutionQueue::Compute,
                 RenderGraph::DiagnosticExecutionQueue::Graphics,
                 finalQueueJoin ? RenderGraph::DiagnosticSyncReason::FinalQueueJoin
                                : RenderGraph::DiagnosticSyncReason::CrossQueueDependency,
                 nextFenceValue,
                 computeLastRecordedPassIndex,
                 targetPassIndex});
            graph.stats.asyncFenceSignalCount++;
            graph.stats.asyncFenceWaitCount++;
            if (finalQueueJoin)
            {
                graph.stats.asyncFinalQueueJoinCount++;
            }
            computeVisibleToGraphicsOrder = computeLastRecordedOrder;
        };

        for (uint32 passIndex : graph.executionOrder)
        {
            if (passIndex >= graph.passes.size())
                continue;

            Pass& pass = graph.passes[passIndex];
            if (pass.culled)
                continue;

            int32 maxGraphicsDependencyOrder = -1;
            int32 maxComputeDependencyOrder = -1;
            if (passIndex < graph.passDependencies.size())
            {
                for (uint32 dependencyIndex : graph.passDependencies[passIndex])
                {
                    if (dependencyIndex >= graph.passes.size() ||
                        dependencyIndex >= passExecutionOrder.size())
                    {
                        continue;
                    }

                    int32 dependencyOrder = passExecutionOrder[dependencyIndex];
                    if (dependencyOrder < 0)
                        continue;

                    const Pass& dependency = graph.passes[dependencyIndex];
                    if (RunsOnComputeQueue(dependency))
                    {
                        maxComputeDependencyOrder = std::max(maxComputeDependencyOrder, dependencyOrder);
                    }
                    else
                    {
                        maxGraphicsDependencyOrder = std::max(maxGraphicsDependencyOrder, dependencyOrder);
                    }
                }
            }

            if (pass.type == RenderGraphPassType::Compute)
            {
                if (HasPlannedQueueRequirement(plannedSyncRequirements.requiresGraphicsForCompute, passIndex) &&
                    maxGraphicsDependencyOrder > graphicsVisibleToComputeOrder)
                {
                    signalGraphicsForCompute(passIndex);
                }
                ExecutePassOnContext(
                    pass,
                    *computeCtx,
                    RenderGraph::DiagnosticExecutionQueue::Compute,
                    executionSerial++,
                    graph);
                graph.stats.asyncComputeScheduledPasses++;
                if (passIndex < passExecutionOrder.size())
                {
                    computeLastRecordedOrder = passExecutionOrder[passIndex];
                    computeLastRecordedPassIndex = passIndex;
                }
            }
            else
            {
                if (HasPlannedQueueRequirement(plannedSyncRequirements.requiresComputeForGraphics, passIndex) &&
                    maxComputeDependencyOrder > computeVisibleToGraphicsOrder)
                {
                    signalComputeForGraphics(passIndex, false);
                }
                ExecutePassOnContext(
                    pass,
                    graphicsCtx,
                    RenderGraph::DiagnosticExecutionQueue::Graphics,
                    executionSerial++,
                    graph);
                graph.stats.asyncGraphicsScheduledPasses++;
                if (passIndex < passExecutionOrder.size())
                {
                    graphicsLastRecordedOrder = passExecutionOrder[passIndex];
                    graphicsLastRecordedPassIndex = passIndex;
                }
            }
        }

        if (computeLastRecordedOrder > computeVisibleToGraphicsOrder)
        {
            signalComputeForGraphics(RVX_INVALID_INDEX, true);
        }
        EmitExportBarriers(graph, graphicsCtx);

        if (graph.stats.asyncComputeScheduledPasses == 0)
        {
            graph.stats.asyncFallbackUsed = true;
            graph.stats.asyncFallbackReason = RenderGraph::AsyncComputeFallbackReason::NoEligibleComputePasses;
        }
    }

} // namespace RVX
