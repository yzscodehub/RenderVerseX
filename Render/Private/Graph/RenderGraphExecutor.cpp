#include "RenderGraphInternal.h"
#include "Core/Log.h"
#include <vector>

namespace RVX
{
    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx)
    {
        if (!graph.executionOrder.empty())
        {
            for (uint32 passIndex : graph.executionOrder)
            {
                const auto& pass = graph.passes[passIndex];
                if (pass.culled)
                    continue;
                ctx.BeginEvent(pass.name.c_str());

                // Note: Aliasing barriers for placed resources
                // Currently handled implicitly via Undefined -> desired state transitions
                // When a placed resource is first used, its state is Undefined, which tells
                // the GPU that previous contents are invalid (equivalent to aliasing barrier)
                // Future: Could add explicit RHI AliasingBarrier support for more control
                // if (!pass.aliasingBarriers.empty())
                // {
                //     ctx.AliasingBarriers(graph, pass.aliasingBarriers);
                // }

                if (!pass.bufferBarriers.empty() || !pass.textureBarriers.empty())
                {
                    ctx.Barriers(pass.bufferBarriers, pass.textureBarriers);
                }
                if (pass.execute)
                {
                    pass.execute(ctx);
                }
                ctx.EndEvent();
            }
        }
        else
        {
            for (const auto& pass : graph.passes)
            {
                if (pass.culled)
                    continue;
                ctx.BeginEvent(pass.name.c_str());
                if (!pass.bufferBarriers.empty() || !pass.textureBarriers.empty())
                {
                    ctx.Barriers(pass.bufferBarriers, pass.textureBarriers);
                }
                if (pass.execute)
                {
                    pass.execute(ctx);
                }
                ctx.EndEvent();
            }
        }

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
                                 RHISubresourceRange{mip, 1, layer, 1, RHITextureAspect::Color}});
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
                    {resource.GetTexture(), resource.currentState, desired, RHISubresourceRange::All()});
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

    void ExecuteRenderGraphAsync(RenderGraphImpl& graph, 
                                  RHICommandContext& graphicsCtx,
                                  RHICommandContext* computeCtx,
                                  RHIFence* computeFence)
    {
        // If no compute context or fence, fall back to sync execution
        if (!computeCtx || !computeFence)
        {
            ExecuteRenderGraph(graph, graphicsCtx);
            return;
        }

        // Track fence value for synchronization
        // Note: In a full implementation, this would be managed per-frame
        (void)computeFence;  // TODO: Implement actual fence signaling/waiting
        
        // Track which queue last touched each resource for cross-queue barriers
        enum class LastQueue { None, Graphics, Compute };
        std::vector<LastQueue> textureQueues(graph.textures.size(), LastQueue::None);
        std::vector<LastQueue> bufferQueues(graph.buffers.size(), LastQueue::None);

        // Lambda to execute passes in given order
        auto executeInOrder = [&](const std::vector<uint32>& indices) {
            for (uint32 passIndex : indices)
            {
                const auto& pass = graph.passes[passIndex];
                if (pass.culled)
                    continue;

                // Determine which queue to use
                bool useCompute = (pass.type == RenderGraphPassType::Compute);
                RHICommandContext& ctx = useCompute ? *computeCtx : graphicsCtx;
                LastQueue currentQueue = useCompute ? LastQueue::Compute : LastQueue::Graphics;

                // Check if we need cross-queue synchronization
                bool needsWaitOnCompute = false;
                bool needsWaitOnGraphics = false;

                for (const auto& usage : pass.usages)
                {
                    LastQueue lastQueue = LastQueue::None;
                    if (usage.type == ResourceType::Texture)
                    {
                        if (usage.index < textureQueues.size())
                            lastQueue = textureQueues[usage.index];
                    }
                    else
                    {
                        if (usage.index < bufferQueues.size())
                            lastQueue = bufferQueues[usage.index];
                    }

                    if (lastQueue != LastQueue::None && lastQueue != currentQueue)
                    {
                        if (lastQueue == LastQueue::Compute)
                            needsWaitOnCompute = true;
                        else
                            needsWaitOnGraphics = true;
                    }
                }

                // Insert fence wait if switching queues
                if (useCompute && needsWaitOnGraphics)
                {
                    // Graphics signaled, compute waits
                    // Note: Actual fence signaling/waiting would require RHI queue-level support
                    // This is a simplified placeholder - real implementation needs:
                    // graphicsQueue->Signal(computeFence, fenceValue)
                    // computeQueue->Wait(computeFence, fenceValue)
                    RVX_CORE_DEBUG("RenderGraph: Compute pass '{}' waiting on graphics", pass.name);
                }
                else if (!useCompute && needsWaitOnCompute)
                {
                    // Compute signaled, graphics waits
                    RVX_CORE_DEBUG("RenderGraph: Graphics pass '{}' waiting on compute", pass.name);
                }

                // Execute the pass
                ctx.BeginEvent(pass.name.c_str());

                if (!pass.bufferBarriers.empty() || !pass.textureBarriers.empty())
                {
                    ctx.Barriers(pass.bufferBarriers, pass.textureBarriers);
                }

                if (pass.execute)
                {
                    pass.execute(ctx);
                }

                ctx.EndEvent();

                // Update resource queue tracking
                for (const auto& usage : pass.usages)
                {
                    if (usage.access == RGAccessType::Write || usage.access == RGAccessType::ReadWrite)
                    {
                        if (usage.type == ResourceType::Texture && usage.index < textureQueues.size())
                        {
                            textureQueues[usage.index] = currentQueue;
                        }
                        else if (usage.type == ResourceType::Buffer && usage.index < bufferQueues.size())
                        {
                            bufferQueues[usage.index] = currentQueue;
                        }
                    }
                }
            }
        };

        // Build execution order if not already computed
        if (graph.executionOrder.empty())
        {
            std::vector<uint32> allPasses;
            for (uint32 i = 0; i < static_cast<uint32>(graph.passes.size()); ++i)
            {
                allPasses.push_back(i);
            }
            executeInOrder(allPasses);
        }
        else
        {
            executeInOrder(graph.executionOrder);
        }

        // Final export barriers (on graphics queue)
        std::vector<RHIBufferBarrier> exportBufferBarriers;
        std::vector<RHITextureBarrier> exportTextureBarriers;

        for (auto& resource : graph.textures)
        {
            if (!resource.exportState || !resource.GetTexture())
                continue;

            RHIResourceState desired = *resource.exportState;
            if (resource.currentState != desired)
            {
                exportTextureBarriers.push_back(
                    {resource.GetTexture(), resource.currentState, desired, RHISubresourceRange::All()});
                resource.currentState = desired;
            }
        }

        for (auto& resource : graph.buffers)
        {
            if (!resource.exportState || !resource.GetBuffer())
                continue;

            RHIResourceState desired = *resource.exportState;
            if (resource.currentState != desired)
            {
                exportBufferBarriers.push_back(
                    {resource.GetBuffer(), resource.currentState, desired, 0, RVX_WHOLE_SIZE});
                resource.currentState = desired;
            }
        }

        if (!exportBufferBarriers.empty() || !exportTextureBarriers.empty())
        {
            graphicsCtx.Barriers(exportBufferBarriers, exportTextureBarriers);
        }
    }

} // namespace RVX
