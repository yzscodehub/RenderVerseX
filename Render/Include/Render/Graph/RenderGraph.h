#pragma once

/**
 * @file RenderGraph.h
 * @brief Frame graph and automatic resource management
 * 
 * RenderGraph provides automatic resource state tracking, barrier insertion,
 * pass culling, and memory aliasing for transient resources.
 */

#include "RHI/RHI.h"
#include <functional>
#include <memory>

namespace RVX
{
    // =============================================================================
    // Render Graph Handle Types
    // =============================================================================
    struct RGTextureHandle
    {
        uint32 index = RVX_INVALID_INDEX;
        bool hasSubresourceRange = false;
        RHISubresourceRange subresourceRange = RHISubresourceRange::All();

        bool IsValid() const { return index != RVX_INVALID_INDEX; }
        
        // Subresource access
        RGTextureHandle Subresource(uint32 mipLevel, uint32 arraySlice = 0) const;
        RGTextureHandle MipRange(uint32 baseMip, uint32 mipCount) const;
    };

    struct RGBufferHandle
    {
        uint32 index = RVX_INVALID_INDEX;
        bool hasRange = false;
        uint64 rangeOffset = 0;
        uint64 rangeSize = RVX_WHOLE_SIZE;

        bool IsValid() const { return index != RVX_INVALID_INDEX; }

        // Range access
        RGBufferHandle Range(uint64 offset, uint64 size) const;
    };

    // =============================================================================
    // Render Graph Pass Type
    // =============================================================================
    enum class RenderGraphPassType
    {
        Graphics,
        Compute,
        Copy,
    };

    // =============================================================================
    // Render Graph Builder
    // =============================================================================
    class RenderGraphBuilder
    {
    public:
        // Read resources
        RGTextureHandle Read(RGTextureHandle texture, RHIShaderStage stages = RHIShaderStage::AllGraphics);
        RGBufferHandle Read(RGBufferHandle buffer, RHIShaderStage stages = RHIShaderStage::AllGraphics);

        // Write resources
        RGTextureHandle Write(RGTextureHandle texture, RHIResourceState state = RHIResourceState::RenderTarget);
        RGBufferHandle Write(RGBufferHandle buffer, RHIResourceState state = RHIResourceState::UnorderedAccess);

        // Read-write resources
        RGTextureHandle ReadWrite(RGTextureHandle texture);
        RGBufferHandle ReadWrite(RGBufferHandle buffer);

        // Subresource-level access
        RGTextureHandle ReadMip(RGTextureHandle texture, uint32 mipLevel);
        RGTextureHandle WriteMip(RGTextureHandle texture, uint32 mipLevel);

        // Depth-stencil
        void SetDepthStencil(RGTextureHandle texture, bool depthWrite = true, bool stencilWrite = false);

    private:
        class Impl;
        Impl* m_impl = nullptr;
        friend class RenderGraph;
    };

    // =============================================================================
    // Render Graph
    // =============================================================================
    class RenderGraph
    {
    public:
        RenderGraph();
        ~RenderGraph();

        void SetDevice(IRHIDevice* device);

        // Create transient resources
        RGTextureHandle CreateTexture(const RHITextureDesc& desc);
        RGBufferHandle CreateBuffer(const RHIBufferDesc& desc);

        // Import external resources
        RGTextureHandle ImportTexture(RHITexture* texture, RHIResourceState initialState);
        RGBufferHandle ImportBuffer(RHIBuffer* buffer, RHIResourceState initialState);

        // Export final state for external usage
        void SetExportState(RGTextureHandle texture, RHIResourceState finalState);
        void SetExportState(RGBufferHandle buffer, RHIResourceState finalState);

        // Add passes
        template<typename Data>
        void AddPass(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&, Data&)> setup,
            std::function<void(const Data&, RHICommandContext&)> execute);

        // Compile the graph
        void Compile();

        // Execute the graph
        void Execute(RHICommandContext& ctx);

        struct CompileStats
        {
            // Pass statistics
            uint32 totalPasses = 0;
            uint32 culledPasses = 0;
            
            // Barrier statistics
            uint32 barrierCount = 0;
            uint32 textureBarrierCount = 0;
            uint32 bufferBarrierCount = 0;
            uint32 mergedBarrierCount = 0;
            uint32 mergedTextureBarrierCount = 0;
            uint32 mergedBufferBarrierCount = 0;
            uint32 crossPassMergedBarrierCount = 0;
            
            // Memory aliasing statistics
            uint32 totalTransientTextures = 0;
            uint32 totalTransientBuffers = 0;
            uint32 aliasedTextureCount = 0;
            uint32 aliasedBufferCount = 0;
            uint64 memoryWithoutAliasing = 0;  // Total memory if no aliasing
            uint64 memoryWithAliasing = 0;      // Actual memory used with aliasing
            uint32 transientHeapCount = 0;
            
            // Memory savings percentage (0-100)
            float GetMemorySavingsPercent() const {
                if (memoryWithoutAliasing == 0) return 0.0f;
                return 100.0f * (1.0f - static_cast<float>(memoryWithAliasing) / 
                    static_cast<float>(memoryWithoutAliasing));
            }
        };

        const CompileStats& GetCompileStats() const;

        // Memory aliasing control
        void SetMemoryAliasingEnabled(bool enabled);
        bool IsMemoryAliasingEnabled() const;

        // Debug/Visualization
        std::string ExportGraphviz() const;
        bool SaveGraphviz(const char* filename) const;

        // Clear for next frame
        void Clear();

    private:
        void AddPassInternal(
            const char* name,
            RenderGraphPassType type,
            std::function<void(RenderGraphBuilder&)> setup,
            std::function<void(RHICommandContext&)> execute);

        class Impl;
        std::unique_ptr<Impl> m_impl;
    };

    template<typename Data>
    void RenderGraph::AddPass(
        const char* name,
        RenderGraphPassType type,
        std::function<void(RenderGraphBuilder&, Data&)> setup,
        std::function<void(const Data&, RHICommandContext&)> execute)
    {
        auto data = std::make_shared<Data>();
        AddPassInternal(
            name,
            type,
            [data, setup](RenderGraphBuilder& builder)
            {
                setup(builder, *data);
            },
            [data, execute](RHICommandContext& ctx)
            {
                execute(*data, ctx);
            });
    }

} // namespace RVX
