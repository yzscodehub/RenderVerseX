#include "RenderGraphInternal.h"

namespace RVX
{
    class RenderGraph::Impl : public RenderGraphImpl
    {
    };

    RGTextureHandle RGTextureHandle::Subresource(uint32 mipLevel, uint32 arraySlice) const
    {
        RGTextureHandle handle = *this;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange{mipLevel, 1, arraySlice, 1, RHITextureAspect::Color};
        return handle;
    }

    RGTextureHandle RGTextureHandle::MipRange(uint32 baseMip, uint32 mipCount) const
    {
        RGTextureHandle handle = *this;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange{baseMip, mipCount, 0, RVX_ALL_LAYERS, RHITextureAspect::Color};
        return handle;
    }

    RGBufferHandle RGBufferHandle::Range(uint64 offset, uint64 size) const
    {
        RGBufferHandle handle = *this;
        handle.hasRange = true;
        handle.rangeOffset = offset;
        handle.rangeSize = size;
        return handle;
    }

    class RenderGraphBuilder::Impl
    {
    public:
        std::vector<TextureResource>* textures = nullptr;
        std::vector<BufferResource>* buffers = nullptr;
        Pass* pass = nullptr;
    };

    RenderGraph::RenderGraph() : m_impl(std::make_unique<Impl>()) {}
    RenderGraph::~RenderGraph() = default;

    void RenderGraph::SetDevice(IRHIDevice* device)
    {
        m_impl->device = device;
    }

    RGTextureHandle RenderGraph::CreateTexture(const RHITextureDesc& desc)
    {
        TextureResource resource;
        resource.desc = desc;
        resource.initialState = RHIResourceState::Undefined;
        resource.currentState = resource.initialState;
        resource.imported = false;
        m_impl->textures.push_back(std::move(resource));
        return RGTextureHandle{static_cast<uint32>(m_impl->textures.size() - 1)};
    }

    RGBufferHandle RenderGraph::CreateBuffer(const RHIBufferDesc& desc)
    {
        BufferResource resource;
        resource.desc = desc;
        resource.initialState = RHIResourceState::Undefined;
        resource.currentState = resource.initialState;
        resource.imported = false;
        m_impl->buffers.push_back(std::move(resource));
        return RGBufferHandle{static_cast<uint32>(m_impl->buffers.size() - 1)};
    }

    RGTextureHandle RenderGraph::ImportTexture(RHITexture* texture, RHIResourceState initialState)
    {
        TextureResource resource;
        resource.texture.Reset(texture);
        if (texture)
        {
            resource.desc.width = texture->GetWidth();
            resource.desc.height = texture->GetHeight();
            resource.desc.depth = texture->GetDepth();
            resource.desc.mipLevels = texture->GetMipLevels();
            resource.desc.arraySize = texture->GetArraySize();
            resource.desc.format = texture->GetFormat();
            resource.desc.usage = texture->GetUsage();
            resource.desc.dimension = texture->GetDimension();
            resource.desc.sampleCount = texture->GetSampleCount();
        }
        resource.initialState = initialState;
        resource.currentState = initialState;
        resource.imported = true;
        m_impl->textures.push_back(std::move(resource));
        return RGTextureHandle{static_cast<uint32>(m_impl->textures.size() - 1)};
    }

    RGBufferHandle RenderGraph::ImportBuffer(RHIBuffer* buffer, RHIResourceState initialState)
    {
        BufferResource resource;
        resource.buffer.Reset(buffer);
        if (buffer)
        {
            resource.desc.size = buffer->GetSize();
            resource.desc.usage = buffer->GetUsage();
            resource.desc.memoryType = buffer->GetMemoryType();
            resource.desc.stride = buffer->GetStride();
        }
        resource.initialState = initialState;
        resource.currentState = initialState;
        resource.imported = true;
        m_impl->buffers.push_back(std::move(resource));
        return RGBufferHandle{static_cast<uint32>(m_impl->buffers.size() - 1)};
    }

    void RenderGraph::SetExportState(RGTextureHandle texture, RHIResourceState finalState)
    {
        if (!texture.IsValid())
            return;
        m_impl->textures[texture.index].exportState = finalState;
    }

    void RenderGraph::SetExportState(RGBufferHandle buffer, RHIResourceState finalState)
    {
        if (!buffer.IsValid())
            return;
        m_impl->buffers[buffer.index].exportState = finalState;
    }

    void RenderGraph::AddPassInternal(
        const char* name,
        RenderGraphPassType type,
        std::function<void(RenderGraphBuilder&)> setup,
        std::function<void(RHICommandContext&)> execute)
    {
        Pass pass;
        pass.name = name ? name : "RenderPass";
        pass.type = type;
        pass.execute = std::move(execute);

        RenderGraphBuilder builder;
        RenderGraphBuilder::Impl builderImpl;
        builderImpl.textures = &m_impl->textures;
        builderImpl.buffers = &m_impl->buffers;
        builderImpl.pass = &pass;
        builder.m_impl = &builderImpl;
        if (setup)
        {
            setup(builder);
        }

        m_impl->passes.push_back(std::move(pass));
    }

    RGTextureHandle RenderGraphBuilder::Read(RGTextureHandle texture, RHIShaderStage stages)
    {
        (void)stages;
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = RHIResourceState::ShaderResource;
        usage.access = RGAccessType::Read;
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }

    RGBufferHandle RenderGraphBuilder::Read(RGBufferHandle buffer, RHIShaderStage stages)
    {
        (void)stages;
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = RHIResourceState::ShaderResource;
        usage.access = RGAccessType::Read;
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::Write(RGTextureHandle texture, RHIResourceState state)
    {
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        ResourceUsage usage;
        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = state;
        usage.access = RGAccessType::Write;
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }

    RGBufferHandle RenderGraphBuilder::Write(RGBufferHandle buffer, RHIResourceState state)
    {
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        ResourceUsage usage;
        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = state;
        usage.access = RGAccessType::Write;
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::ReadWrite(RGTextureHandle texture)
    {
        ResourceUsage usage;
        if (!m_impl || !m_impl->pass || !texture.IsValid())
            return texture;

        usage.type = ResourceType::Texture;
        usage.index = texture.index;
        usage.desiredState = RHIResourceState::UnorderedAccess;
        usage.access = RGAccessType::ReadWrite;
        usage.hasSubresourceRange = texture.hasSubresourceRange;
        if (texture.hasSubresourceRange)
            usage.subresourceRange = texture.subresourceRange;
        m_impl->pass->usages.push_back(usage);
        return texture;
    }

    RGBufferHandle RenderGraphBuilder::ReadWrite(RGBufferHandle buffer)
    {
        ResourceUsage usage;
        if (!m_impl || !m_impl->pass || !buffer.IsValid())
            return buffer;

        usage.type = ResourceType::Buffer;
        usage.index = buffer.index;
        usage.desiredState = RHIResourceState::UnorderedAccess;
        usage.access = RGAccessType::ReadWrite;
        usage.hasRange = buffer.hasRange;
        if (buffer.hasRange)
        {
            usage.offset = buffer.rangeOffset;
            usage.size = buffer.rangeSize;
        }
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::ReadMip(RGTextureHandle texture, uint32 mipLevel)
    {
        RGTextureHandle handle = texture;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange::Mip(mipLevel);
        return Read(handle);
    }

    RGTextureHandle RenderGraphBuilder::WriteMip(RGTextureHandle texture, uint32 mipLevel)
    {
        RGTextureHandle handle = texture;
        handle.hasSubresourceRange = true;
        handle.subresourceRange = RHISubresourceRange::Mip(mipLevel);
        return Write(handle, RHIResourceState::RenderTarget);
    }

    void RenderGraphBuilder::SetDepthStencil(RGTextureHandle texture, bool depthWrite, bool stencilWrite)
    {
        (void)stencilWrite;
        Write(texture, depthWrite ? RHIResourceState::DepthWrite : RHIResourceState::DepthRead);
    }

    void RenderGraph::Compile()
    {
        CompileRenderGraph(*m_impl);
    }

    void RenderGraph::Execute(RHICommandContext& ctx)
    {
        ExecuteRenderGraph(*m_impl, ctx);
    }

    const RenderGraph::CompileStats& RenderGraph::GetCompileStats() const
    {
        return m_impl->stats;
    }

    void RenderGraph::SetMemoryAliasingEnabled(bool enabled)
    {
        m_impl->enableMemoryAliasing = enabled;
    }

    bool RenderGraph::IsMemoryAliasingEnabled() const
    {
        return m_impl->enableMemoryAliasing;
    }

    void RenderGraph::Clear()
    {
        m_impl->passes.clear();
        m_impl->textures.clear();
        m_impl->buffers.clear();
        m_impl->executionOrder.clear();
        m_impl->transientHeaps.clear();
        m_impl->stats = {};
        m_impl->totalMemoryWithoutAliasing = 0;
        m_impl->totalMemoryWithAliasing = 0;
        m_impl->aliasedTextureCount = 0;
        m_impl->aliasedBufferCount = 0;
    }

} // namespace RVX
