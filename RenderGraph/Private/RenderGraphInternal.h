#pragma once

#include "RenderGraph/RenderGraph.h"
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    enum class ResourceType
    {
        Texture,
        Buffer,
    };

    enum class RGAccessType
    {
        Read,
        Write,
        ReadWrite,
    };

    struct TextureResource
    {
        RHITextureDesc desc;
        RHITextureRef texture;
        RHIResourceState initialState = RHIResourceState::Undefined;
        RHIResourceState currentState = RHIResourceState::Undefined;
        std::unordered_map<uint32, RHIResourceState> subresourceStates;
        bool hasSubresourceTracking = false;
        std::optional<RHIResourceState> exportState;
        bool imported = false;
    };

    struct BufferResource
    {
        RHIBufferDesc desc;
        RHIBufferRef buffer;
        RHIResourceState initialState = RHIResourceState::Undefined;
        RHIResourceState currentState = RHIResourceState::Undefined;
        std::optional<RHIResourceState> exportState;
        struct RangeState
        {
            uint64 offset = 0;
            uint64 size = 0;
            RHIResourceState state = RHIResourceState::Common;
        };
        std::vector<RangeState> rangeStates;
        bool hasRangeTracking = false;
        bool imported = false;
    };

    struct ResourceUsage
    {
        ResourceType type = ResourceType::Texture;
        uint32 index = RVX_INVALID_INDEX;
        RHIResourceState desiredState = RHIResourceState::Common;
        RGAccessType access = RGAccessType::Read;
        RHISubresourceRange subresourceRange = RHISubresourceRange::All();
        bool hasSubresourceRange = false;
        uint64 offset = 0;
        uint64 size = RVX_WHOLE_SIZE;
        bool hasRange = false;
    };

    struct Pass
    {
        std::string name;
        RenderGraphPassType type = RenderGraphPassType::Graphics;
        std::vector<ResourceUsage> usages;
        std::vector<uint32> readTextures;
        std::vector<uint32> writeTextures;
        std::vector<uint32> readBuffers;
        std::vector<uint32> writeBuffers;
        bool culled = false;
        std::vector<RHITextureBarrier> textureBarriers;
        std::vector<RHIBufferBarrier> bufferBarriers;
        std::function<void(RHICommandContext&)> execute;
    };

    struct RenderGraphImpl
    {
        IRHIDevice* device = nullptr;
        std::vector<TextureResource> textures;
        std::vector<BufferResource> buffers;
        std::vector<Pass> passes;
        std::vector<uint32> executionOrder;
        RenderGraph::CompileStats stats;
    };

    void CompileRenderGraph(RenderGraphImpl& graph);
    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx);
} // namespace RVX
