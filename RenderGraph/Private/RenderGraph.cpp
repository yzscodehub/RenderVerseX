#include "RenderGraph/RenderGraph.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX
{
    namespace
    {
        enum class ResourceType
        {
            Texture,
            Buffer,
        };

        struct TextureResource
        {
            RHITextureDesc desc;
            RHITextureRef texture;
            RHIResourceState initialState = RHIResourceState::Undefined;
            RHIResourceState currentState = RHIResourceState::Undefined;
            bool imported = false;
        };

        struct BufferResource
        {
            RHIBufferDesc desc;
            RHIBufferRef buffer;
            RHIResourceState initialState = RHIResourceState::Undefined;
            RHIResourceState currentState = RHIResourceState::Undefined;
            bool imported = false;
        };

        struct ResourceUsage
        {
            ResourceType type = ResourceType::Texture;
            uint32 index = RVX_INVALID_INDEX;
            RHIResourceState desiredState = RHIResourceState::Common;
        };

        struct Pass
        {
            std::string name;
            RenderGraphPassType type = RenderGraphPassType::Graphics;
            std::vector<ResourceUsage> usages;
            std::vector<RHITextureBarrier> textureBarriers;
            std::vector<RHIBufferBarrier> bufferBarriers;
            std::function<void(RHICommandContext&)> execute;
        };
    }

    class RenderGraph::Impl
    {
    public:
        IRHIDevice* device = nullptr;
        std::vector<TextureResource> textures;
        std::vector<BufferResource> buffers;
        std::vector<Pass> passes;
    };

    RGTextureHandle RGTextureHandle::Subresource(uint32 mipLevel, uint32 arraySlice) const
    {
        (void)mipLevel;
        (void)arraySlice;
        return *this;
    }

    RGTextureHandle RGTextureHandle::MipRange(uint32 baseMip, uint32 mipCount) const
    {
        (void)baseMip;
        (void)mipCount;
        return *this;
    }

    RGBufferHandle RGBufferHandle::Range(uint64 offset, uint64 size) const
    {
        (void)offset;
        (void)size;
        return *this;
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
        resource.initialState = initialState;
        resource.currentState = initialState;
        resource.imported = true;
        m_impl->buffers.push_back(std::move(resource));
        return RGBufferHandle{static_cast<uint32>(m_impl->buffers.size() - 1)};
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
        m_impl->pass->usages.push_back(usage);
        return buffer;
    }

    RGTextureHandle RenderGraphBuilder::ReadWrite(RGTextureHandle texture)
    {
        return Write(texture, RHIResourceState::UnorderedAccess);
    }

    RGBufferHandle RenderGraphBuilder::ReadWrite(RGBufferHandle buffer)
    {
        return Write(buffer, RHIResourceState::UnorderedAccess);
    }

    RGTextureHandle RenderGraphBuilder::ReadMip(RGTextureHandle texture, uint32 mipLevel)
    {
        (void)mipLevel;
        return Read(texture);
    }

    RGTextureHandle RenderGraphBuilder::WriteMip(RGTextureHandle texture, uint32 mipLevel)
    {
        (void)mipLevel;
        return Write(texture, RHIResourceState::RenderTarget);
    }

    void RenderGraphBuilder::SetDepthStencil(RGTextureHandle texture, bool depthWrite, bool stencilWrite)
    {
        (void)stencilWrite;
        Write(texture, depthWrite ? RHIResourceState::DepthWrite : RHIResourceState::DepthRead);
    }

    void RenderGraph::Compile()
    {
        if (m_impl->device)
        {
            for (auto& texture : m_impl->textures)
            {
                if (!texture.imported && !texture.texture)
                {
                    texture.texture = m_impl->device->CreateTexture(texture.desc);
                    texture.initialState = RHIResourceState::Undefined;
                    texture.currentState = texture.initialState;
                }
            }

            for (auto& buffer : m_impl->buffers)
            {
                if (!buffer.imported && !buffer.buffer)
                {
                    buffer.buffer = m_impl->device->CreateBuffer(buffer.desc);
                    buffer.initialState = RHIResourceState::Undefined;
                    buffer.currentState = buffer.initialState;
                }
            }
        }

        for (auto& pass : m_impl->passes)
        {
            pass.textureBarriers.clear();
            pass.bufferBarriers.clear();

            for (const auto& usage : pass.usages)
            {
                if (usage.type == ResourceType::Texture)
                {
                    auto& resource = m_impl->textures[usage.index];
                    if (!resource.texture)
                        continue;
                    if (resource.currentState != usage.desiredState)
                    {
                        pass.textureBarriers.push_back(
                            {resource.texture.Get(), resource.currentState, usage.desiredState, RHISubresourceRange::All()});
                        resource.currentState = usage.desiredState;
                    }
                }
                else
                {
                    auto& resource = m_impl->buffers[usage.index];
                    if (!resource.buffer)
                        continue;
                    if (resource.currentState != usage.desiredState)
                    {
                        pass.bufferBarriers.push_back(
                            {resource.buffer.Get(), resource.currentState, usage.desiredState, 0, RVX_WHOLE_SIZE});
                        resource.currentState = usage.desiredState;
                    }
                }
            }
        }
    }

    void RenderGraph::Execute(RHICommandContext& ctx)
    {
        for (const auto& pass : m_impl->passes)
        {
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

    void RenderGraph::Clear()
    {
        m_impl->passes.clear();
        m_impl->textures.clear();
        m_impl->buffers.clear();
    }

} // namespace RVX
