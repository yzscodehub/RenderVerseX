#include "RenderGraphInternal.h"
#include <sstream>
#include <fstream>

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

    std::string RenderGraph::ExportGraphviz() const
    {
        std::ostringstream ss;
        ss << "digraph RenderGraph {\n";
        ss << "  rankdir=LR;\n";
        ss << "  node [fontname=\"Helvetica\", fontsize=10];\n";
        ss << "  edge [color=\"#666666\"];\n\n";
        
        // Subgraph for resources
        ss << "  subgraph cluster_resources {\n";
        ss << "    label=\"Resources\";\n";
        ss << "    style=dashed;\n";
        ss << "    color=\"#cccccc\";\n\n";
        
        // Texture nodes (ellipse)
        for (size_t i = 0; i < m_impl->textures.size(); ++i)
        {
            const auto& tex = m_impl->textures[i];
            std::string name = tex.desc.debugName ? tex.desc.debugName : ("Tex" + std::to_string(i));
            std::string fillColor = tex.imported ? "#b3d9ff" : (tex.alias.isAliased ? "#ffffb3" : "#b3ffb3");
            
            ss << "    tex" << i << " [shape=ellipse, style=filled, fillcolor=\"" << fillColor << "\", ";
            ss << "label=\"" << name << "\\n" << tex.desc.width << "x" << tex.desc.height;
            if (tex.alias.isAliased)
            {
                ss << "\\n(aliased H" << tex.alias.heapIndex << ")";
            }
            ss << "\"];\n";
        }
        
        // Buffer nodes (ellipse with different shape)
        for (size_t i = 0; i < m_impl->buffers.size(); ++i)
        {
            const auto& buf = m_impl->buffers[i];
            std::string name = buf.desc.debugName ? buf.desc.debugName : ("Buf" + std::to_string(i));
            std::string fillColor = buf.imported ? "#b3d9ff" : (buf.alias.isAliased ? "#ffffb3" : "#b3ffb3");
            
            ss << "    buf" << i << " [shape=box, style=\"filled,rounded\", fillcolor=\"" << fillColor << "\", ";
            ss << "label=\"" << name << "\\n" << (buf.desc.size / 1024) << " KB";
            if (buf.alias.isAliased)
            {
                ss << "\\n(aliased H" << buf.alias.heapIndex << ")";
            }
            ss << "\"];\n";
        }
        
        ss << "  }\n\n";
        
        // Pass nodes (boxes)
        ss << "  // Passes\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            
            std::string color;
            if (pass.culled)
                color = "#e0e0e0";
            else if (pass.type == RenderGraphPassType::Compute)
                color = "#fff2cc";
            else if (pass.type == RenderGraphPassType::Copy)
                color = "#d9ead3";
            else
                color = "#f4cccc";
            
            std::string style = pass.culled ? "dashed" : "filled";
            
            ss << "  pass" << i << " [shape=box, style=\"" << style << "\", fillcolor=\"" << color << "\", ";
            ss << "label=\"" << pass.name;
            if (pass.culled)
                ss << "\\n(CULLED)";
            ss << "\"];\n";
        }
        
        // Dependencies (resource -> pass for reads, pass -> resource for writes)
        ss << "\n  // Read edges (resource -> pass)\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            for (uint32 texIdx : pass.readTextures)
            {
                ss << "  tex" << texIdx << " -> pass" << i << " [color=\"#3366cc\"];\n";
            }
            for (uint32 bufIdx : pass.readBuffers)
            {
                ss << "  buf" << bufIdx << " -> pass" << i << " [color=\"#3366cc\"];\n";
            }
        }
        
        ss << "\n  // Write edges (pass -> resource)\n";
        for (size_t i = 0; i < m_impl->passes.size(); ++i)
        {
            const auto& pass = m_impl->passes[i];
            for (uint32 texIdx : pass.writeTextures)
            {
                ss << "  pass" << i << " -> tex" << texIdx << " [color=\"#cc3333\", style=bold];\n";
            }
            for (uint32 bufIdx : pass.writeBuffers)
            {
                ss << "  pass" << i << " -> buf" << bufIdx << " [color=\"#cc3333\", style=bold];\n";
            }
        }
        
        // Execution order edges
        if (m_impl->executionOrder.size() > 1)
        {
            ss << "\n  // Execution order (invisible edges for layout)\n";
            ss << "  edge [style=invis];\n";
            for (size_t i = 1; i < m_impl->executionOrder.size(); ++i)
            {
                ss << "  pass" << m_impl->executionOrder[i - 1] << " -> pass" << m_impl->executionOrder[i] << ";\n";
            }
        }
        
        // Legend
        ss << "\n  // Legend\n";
        ss << "  subgraph cluster_legend {\n";
        ss << "    label=\"Legend\";\n";
        ss << "    style=solid;\n";
        ss << "    rank=sink;\n";
        ss << "    legend_imported [shape=ellipse, style=filled, fillcolor=\"#b3d9ff\", label=\"Imported\"];\n";
        ss << "    legend_transient [shape=ellipse, style=filled, fillcolor=\"#b3ffb3\", label=\"Transient\"];\n";
        ss << "    legend_aliased [shape=ellipse, style=filled, fillcolor=\"#ffffb3\", label=\"Aliased\"];\n";
        ss << "    legend_graphics [shape=box, style=filled, fillcolor=\"#f4cccc\", label=\"Graphics\"];\n";
        ss << "    legend_compute [shape=box, style=filled, fillcolor=\"#fff2cc\", label=\"Compute\"];\n";
        ss << "    legend_copy [shape=box, style=filled, fillcolor=\"#d9ead3\", label=\"Copy\"];\n";
        ss << "    legend_imported -> legend_transient -> legend_aliased -> legend_graphics -> legend_compute -> legend_copy [style=invis];\n";
        ss << "  }\n";
        
        ss << "}\n";
        
        return ss.str();
    }

    bool RenderGraph::SaveGraphviz(const char* filename) const
    {
        std::string dot = ExportGraphviz();
        
        std::ofstream file(filename);
        if (!file.is_open())
        {
            return false;
        }
        
        file << dot;
        return file.good();
    }

} // namespace RVX
