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

    // =============================================================================
    // Resource Lifetime - for memory aliasing
    // =============================================================================
    struct ResourceLifetime
    {
        uint32 firstUsePass = UINT32_MAX;  // First pass that uses this resource
        uint32 lastUsePass = 0;             // Last pass that uses this resource
        uint64 memorySize = 0;              // Required memory size
        uint64 alignment = 0;               // Alignment requirement
        bool isUsed = false;                // Whether this resource is actually used
    };

    // =============================================================================
    // Memory Alias - assigned heap and offset for a resource
    // =============================================================================
    struct MemoryAlias
    {
        uint32 heapIndex = UINT32_MAX;      // Which heap this resource is allocated from
        uint64 heapOffset = 0;               // Offset within the heap
        bool isAliased = false;              // Whether this resource shares memory with others
    };

    // =============================================================================
    // Transient Heap - a single memory heap for aliased resources
    // =============================================================================
    struct TransientHeap
    {
        uint64 size = 0;                     // Total heap size
        uint32 resourceCount = 0;            // Number of resources using this heap
        
        // Platform-specific heap handles (set during resource creation)
        // DX12: ID3D12Heap*
        // Vulkan: VkDeviceMemory
        void* platformHeap = nullptr;
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
        
        // Memory aliasing
        ResourceLifetime lifetime;
        MemoryAlias alias;
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
        
        // Memory aliasing
        ResourceLifetime lifetime;
        MemoryAlias alias;
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
        
        // Memory aliasing
        std::vector<TransientHeap> transientHeaps;
        bool enableMemoryAliasing = true;
        
        // Aliasing statistics
        uint64 totalMemoryWithoutAliasing = 0;
        uint64 totalMemoryWithAliasing = 0;
        uint32 aliasedTextureCount = 0;
        uint32 aliasedBufferCount = 0;
    };

    void CompileRenderGraph(RenderGraphImpl& graph);
    void ExecuteRenderGraph(RenderGraphImpl& graph, RHICommandContext& ctx);
    
    // Memory aliasing functions
    void CalculateResourceLifetimes(RenderGraphImpl& graph);
    void ComputeMemoryAliases(RenderGraphImpl& graph);
    
} // namespace RVX
