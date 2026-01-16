#include "RenderGraphInternal.h"
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
            if (!resource.exportState || !resource.texture)
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
                                {resource.texture.Get(),
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
                    {resource.texture.Get(), resource.currentState, desired, RHISubresourceRange::All()});
                resource.currentState = desired;
            }
        }

        for (auto& resource : graph.buffers)
        {
            if (!resource.exportState || !resource.buffer)
                continue;

            RHIResourceState desired = *resource.exportState;
            if (resource.hasRangeTracking)
            {
                for (const auto& range : resource.rangeStates)
                {
                    if (range.state != desired)
                    {
                        exportBufferBarriers.push_back(
                            {resource.buffer.Get(), range.state, desired, range.offset, range.size});
                    }
                }
                resource.rangeStates.clear();
                resource.hasRangeTracking = false;
                resource.currentState = desired;
            }
            else if (resource.currentState != desired)
            {
                exportBufferBarriers.push_back(
                    {resource.buffer.Get(), resource.currentState, desired, 0, RVX_WHOLE_SIZE});
                resource.currentState = desired;
            }
        }

        if (!exportBufferBarriers.empty() || !exportTextureBarriers.empty())
        {
            ctx.Barriers(exportBufferBarriers, exportTextureBarriers);
        }
    }

} // namespace RVX
