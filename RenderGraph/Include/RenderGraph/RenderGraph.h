#pragma once

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

        bool IsValid() const { return index != RVX_INVALID_INDEX; }
        
        // Subresource access
        RGTextureHandle Subresource(uint32 mipLevel, uint32 arraySlice = 0) const;
        RGTextureHandle MipRange(uint32 baseMip, uint32 mipCount) const;
    };

    struct RGBufferHandle
    {
        uint32 index = RVX_INVALID_INDEX;

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
