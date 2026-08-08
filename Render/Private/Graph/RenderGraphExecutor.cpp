#include "RenderGraphInternal.h"
#include "Core/Job/JobSystem.h"
#include "Core/Log.h"

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
                            ? graph.textures[index].texture.Get()
                            : nullptr;
                    }
                    return index < graph.buffers.size()
                        ? graph.buffers[index].buffer.Get()
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
                ctx.Barriers(pass.bufferBarriers, pass.textureBarriers);
            }
            if (pass.execute)
            {
                pass.execute(ctx);
            }
            if (!pass.postBufferBarriers.empty() ||
                !pass.postTextureBarriers.empty())
            {
                ctx.Barriers(pass.postBufferBarriers,
                             pass.postTextureBarriers);
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
                ctx.Barriers(release->bufferBarriers,
                             release->textureBarriers);
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
            return pass.plannedExecutionQueue ==
                RenderGraph::DiagnosticExecutionQueue::Compute;
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

            if (graph.queueExecutionMode !=
                RenderGraph::QueueExecutionMode::MultiQueue)
            {
                return RenderGraph::AsyncComputeFallbackReason::AsyncPlanningDisabled;
            }

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
            return;
        }

        ValidatePlannedAccessSources(graph);

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
        AccumulateExecutionDiagnostics(graph);
        graph.executionRealized = true;
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
        graph.executionRealized = true;
        return true;
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
        ValidatePlannedAccessSources(graph);

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

            if (RunsOnComputeQueue(pass))
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
        AccumulateExecutionDiagnostics(graph);
        graph.executionRealized = true;

        if (graph.stats.asyncComputeScheduledPasses == 0)
        {
            graph.stats.asyncFallbackUsed = true;
            graph.stats.asyncFallbackReason = RenderGraph::AsyncComputeFallbackReason::NoEligibleComputePasses;
        }
    }

} // namespace RVX
