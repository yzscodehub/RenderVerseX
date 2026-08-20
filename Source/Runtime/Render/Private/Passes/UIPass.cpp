/**
 * @file UIPass.cpp
 * @brief Render-owned UI packet realization and RHI submission
 */

#include "Render/Passes/UIPass.h"

#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"
#include "RHI/RHIPipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace RVX
{
    namespace
    {
        struct UIPushConstants
        {
            float targetSize[2] = {0.0f, 0.0f};
            float inverseTargetSize[2] = {0.0f, 0.0f};
        };

        uint64 GrowCapacity(uint64 current, uint64 required)
        {
            uint64 capacity = std::max<uint64>(current, 4096);
            while (capacity < required)
            {
                capacity *= 2;
            }
            return capacity;
        }

        RHIRect ToScissorRect(const UIRenderRect& rect,
                              uint32 targetWidth,
                              uint32 targetHeight)
        {
            const int32 left = std::clamp(
                static_cast<int32>(std::floor(rect.x)),
                0,
                static_cast<int32>(targetWidth));
            const int32 top = std::clamp(
                static_cast<int32>(std::floor(rect.y)),
                0,
                static_cast<int32>(targetHeight));
            const int32 right = std::clamp(
                static_cast<int32>(std::ceil(rect.x + rect.width)),
                0,
                static_cast<int32>(targetWidth));
            const int32 bottom = std::clamp(
                static_cast<int32>(std::ceil(rect.y + rect.height)),
                0,
                static_cast<int32>(targetHeight));
            return {left,
                    top,
                    static_cast<uint32>(std::max(0, right - left)),
                    static_cast<uint32>(std::max(0, bottom - top))};
        }

        bool SameScissor(const RHIRect& lhs, const RHIRect& rhs)
        {
            return lhs.x == rhs.x && lhs.y == rhs.y &&
                   lhs.width == rhs.width && lhs.height == rhs.height;
        }

        std::vector<uint8> ExpandAlphaAtlasToRGBA(
            const UIRenderFontAtlas& atlas)
        {
            std::vector<uint8> rgbaPixels(atlas.alphaPixels->size() * 4u,
                                          255u);
            for (size_t i = 0; i < atlas.alphaPixels->size(); ++i)
            {
                rgbaPixels[i * 4u + 3u] = (*atlas.alphaPixels)[i];
            }
            return rgbaPixels;
        }
    } // namespace

    bool UIPass::Initialize(IRHIDevice* device)
    {
        if (!device)
        {
            return false;
        }
        if (m_device != device)
        {
            ResetGPUResources();
            m_device = device;
        }
        return true;
    }

    void UIPass::Shutdown()
    {
        ResetGPUResources();
        m_device = nullptr;
    }

    bool UIPass::Submit(const UIRenderPacket& packet,
                        const UIPassSubmitDesc& desc)
    {
        m_submitStats = {};
        const bool useDefaultFramebufferTarget =
            desc.allowDefaultFramebufferTarget && desc.colorTarget == nullptr;
        if (!m_device || !packet.IsValid() || !desc.commandContext ||
            !desc.pipeline || (!desc.colorTarget && !useDefaultFramebufferTarget))
        {
            return false;
        }

        RHICommandContext& context = *desc.commandContext;
        ApplyTextureSetLayoutOverride(desc.textureSetLayout);
        if (!EnsureTextureDescriptorResources() ||
            !EnsureWhiteTexture(context) ||
            !EnsureFontAtlasTexture(context, packet.fontAtlas) ||
            !EnsureDynamicBuffers(packet))
        {
            return false;
        }

        std::vector<RHIDescriptorSet*> commandDescriptorSets;
        commandDescriptorSets.reserve(packet.commands.size());
        for (const UIRenderDrawCommand& command : packet.commands)
        {
            RHIDescriptorSet* descriptorSet = GetDescriptorSetForCommand(
                command,
                desc.resourceResolver);
            if (!descriptorSet)
            {
                return false;
            }
            commandDescriptorSets.push_back(descriptorSet);
        }

        const uint64 vertexBytes = static_cast<uint64>(
            packet.vertices.size() * sizeof(UIRenderVertex));
        const uint64 indexBytes = static_cast<uint64>(
            packet.indices.size() * sizeof(uint32));
        std::vector<UIRenderVertex> submitVertices = packet.vertices;
        const float invTargetWidth =
            1.0f / static_cast<float>(packet.targetWidth);
        const float invTargetHeight =
            1.0f / static_cast<float>(packet.targetHeight);
        for (UIRenderVertex& vertex : submitVertices)
        {
            vertex.position.x =
                vertex.position.x * invTargetWidth * 2.0f - 1.0f;
            vertex.position.y =
                1.0f - vertex.position.y * invTargetHeight * 2.0f;
        }
        if (!UploadBuffer(m_vertexBuffer.Get(),
                          submitVertices.data(),
                          vertexBytes) ||
            !UploadBuffer(m_indexBuffer.Get(),
                          packet.indices.data(),
                          indexBytes))
        {
            return false;
        }

        context.BufferBarrier(m_vertexBuffer.Get(),
                              RHIResourceState::Common,
                              RHIResourceState::VertexBuffer);
        context.BufferBarrier(m_indexBuffer.Get(),
                              RHIResourceState::Common,
                              RHIResourceState::IndexBuffer);

        if (desc.beginRenderPass)
        {
            RHIRenderPassDesc renderPass;
            if (!useDefaultFramebufferTarget)
            {
                renderPass.AddColorAttachment(desc.colorTarget,
                                              desc.colorLoadOp,
                                              desc.colorStoreOp,
                                              desc.clearColor);
            }
            renderPass.SetRenderArea(0,
                                     0,
                                     packet.targetWidth,
                                     packet.targetHeight);
            context.BeginRenderPass(renderPass);
        }

        RHIViewport viewport;
        viewport.width = static_cast<float>(packet.targetWidth);
        viewport.height = static_cast<float>(packet.targetHeight);
        context.SetViewport(viewport);
        RHIRect currentScissor{0,
                               0,
                               packet.targetWidth,
                               packet.targetHeight};
        context.SetScissor(currentScissor);
        context.SetPipeline(desc.pipeline);
        context.SetVertexBuffer(0, m_vertexBuffer.Get());
        context.SetIndexBuffer(m_indexBuffer.Get(), RHIFormat::R32_UINT);

        UIPushConstants pushConstants;
        pushConstants.targetSize[0] =
            static_cast<float>(packet.targetWidth);
        pushConstants.targetSize[1] =
            static_cast<float>(packet.targetHeight);
        pushConstants.inverseTargetSize[0] = invTargetWidth;
        pushConstants.inverseTargetSize[1] = invTargetHeight;
        context.SetPushConstants(&pushConstants, sizeof(pushConstants));

        RHIDescriptorSet* currentDescriptorSet = nullptr;
        for (size_t commandIndex = 0;
             commandIndex < packet.commands.size();
             ++commandIndex)
        {
            const UIRenderDrawCommand& command =
                packet.commands[commandIndex];
            const RHIRect commandScissor = ToScissorRect(
                command.clipRect,
                packet.targetWidth,
                packet.targetHeight);
            if (commandScissor.width == 0u || commandScissor.height == 0u)
            {
                continue;
            }
            if (!SameScissor(commandScissor, currentScissor))
            {
                context.SetScissor(commandScissor);
                currentScissor = commandScissor;
            }

            RHIDescriptorSet* descriptorSet =
                commandDescriptorSets[commandIndex];
            if (descriptorSet != currentDescriptorSet)
            {
                context.SetDescriptorSet(0, descriptorSet);
                currentDescriptorSet = descriptorSet;
            }
            context.DrawIndexed(command.indexCount,
                                1,
                                command.firstIndex,
                                0,
                                0);
            ++m_submitStats.drawCallCount;
        }

        if (desc.beginRenderPass)
        {
            context.EndRenderPass();
        }

        m_submitStats.submitted = m_submitStats.drawCallCount > 0;
        m_submitStats.defaultFramebufferTarget =
            useDefaultFramebufferTarget;
        m_submitStats.vertexBytesUploaded = vertexBytes;
        m_submitStats.indexBytesUploaded = indexBytes;
        return m_submitStats.submitted;
    }

    void UIPass::ResetGPUResources()
    {
        WaitForOwnerReplacement();
        m_vertexBuffer.Reset();
        m_indexBuffer.Reset();
        m_vertexBufferCapacity = 0;
        m_indexBufferCapacity = 0;
        m_fontAtlasTexture.Reset();
        m_fontAtlasTextureView.Reset();
        m_fontAtlasStagingBuffer.Reset();
        m_fontAtlasDescriptorSet.Reset();
        m_uploadedFontAtlasRevision = 0;
        m_uploadedFontAtlasWidth = 0;
        m_uploadedFontAtlasHeight = 0;
        m_whiteTexture.Reset();
        m_whiteTextureView.Reset();
        m_whiteTextureStagingBuffer.Reset();
        m_whiteDescriptorSet.Reset();
        m_textureSampler.Reset();
        m_fontSampler.Reset();
        m_textureSetLayout.Reset();
        m_textureSetLayoutOverride = nullptr;
        m_textureDescriptorSets.clear();
        m_submitStats = {};
    }

    void UIPass::WaitForOwnerReplacement()
    {
        if (m_device)
        {
            m_device->WaitIdle();
        }
    }

    bool UIPass::EnsureDynamicBuffers(const UIRenderPacket& packet)
    {
        const uint64 vertexBytes = static_cast<uint64>(
            packet.vertices.size() * sizeof(UIRenderVertex));
        const uint64 indexBytes = static_cast<uint64>(
            packet.indices.size() * sizeof(uint32));
        return EnsureBuffer(m_vertexBuffer,
                            m_vertexBufferCapacity,
                            vertexBytes,
                            RHIBufferUsage::Vertex,
                            "UI.VertexBuffer",
                            sizeof(UIRenderVertex)) &&
               EnsureBuffer(m_indexBuffer,
                            m_indexBufferCapacity,
                            indexBytes,
                            RHIBufferUsage::Index,
                            "UI.IndexBuffer",
                            0);
    }

    bool UIPass::EnsureBuffer(RHIBufferRef& buffer,
                              uint64& capacity,
                              uint64 requiredSize,
                              RHIBufferUsage usage,
                              const char* debugName,
                              uint32 stride)
    {
        if (requiredSize == 0)
        {
            return true;
        }
        if (buffer && capacity >= requiredSize)
        {
            return true;
        }
        const uint64 newCapacity = GrowCapacity(capacity, requiredSize);
        RHIBufferDesc desc;
        desc.size = newCapacity;
        desc.usage = usage;
        desc.memoryType = RHIMemoryType::Upload;
        desc.stride = stride;
        desc.debugName = debugName;
        RHIBufferRef newBuffer = m_device->CreateBuffer(desc);
        if (!newBuffer)
        {
            return false;
        }
        if (buffer)
        {
            WaitForOwnerReplacement();
        }
        buffer = std::move(newBuffer);
        capacity = newCapacity;
        return true;
    }

    bool UIPass::UploadBuffer(RHIBuffer* buffer,
                              const void* data,
                              uint64 size)
    {
        if (!buffer || !data || size == 0)
        {
            return false;
        }
        void* mapped = buffer->Map();
        if (!mapped)
        {
            return false;
        }
        std::memcpy(mapped, data, static_cast<size_t>(size));
        buffer->Unmap();
        return true;
    }

    bool UIPass::EnsureTextureDescriptorResources()
    {
        if (!m_textureSampler)
        {
            RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
            samplerDesc.mipFilter = RHIFilterMode::Nearest;
            samplerDesc.maxLod = 0.0f;
            samplerDesc.debugName = "UI.LinearClampSampler";
            m_textureSampler = m_device->CreateSampler(samplerDesc);
        }
        if (!m_fontSampler)
        {
            RHISamplerDesc samplerDesc = RHISamplerDesc::LinearClamp();
            samplerDesc.mipFilter = RHIFilterMode::Nearest;
            samplerDesc.maxLod = 0.0f;
            samplerDesc.debugName = "UI.FontAtlasLinearSampler";
            m_fontSampler = m_device->CreateSampler(samplerDesc);
        }
        if (!m_textureSetLayoutOverride && !m_textureSetLayout)
        {
            RHIDescriptorSetLayoutDesc layoutDesc;
            layoutDesc.debugName = "UI.TextureSetLayout";
            layoutDesc.AddBinding(0,
                                  RHIBindingType::SampledTexture,
                                  RHIShaderStage::Pixel);
            layoutDesc.AddBinding(1,
                                  RHIBindingType::Sampler,
                                  RHIShaderStage::Pixel);
            m_textureSetLayout =
                m_device->CreateDescriptorSetLayout(layoutDesc);
        }
        return m_textureSampler && m_fontSampler &&
               GetActiveTextureSetLayout();
    }

    bool UIPass::EnsureWhiteTexture(RHICommandContext& context)
    {
        if (m_whiteTexture && m_whiteTextureView && m_whiteDescriptorSet)
        {
            return true;
        }
        RHIDescriptorSetLayout* layout = GetActiveTextureSetLayout();
        if (!layout || !m_textureSampler)
        {
            return false;
        }
        constexpr std::array<uint8, 4> whitePixel = {255u, 255u, 255u, 255u};
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            1,
            1,
            RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
        textureDesc.debugName = "UI.WhiteTexture";
        m_whiteTexture = m_device->CreateTexture(textureDesc);
        if (!m_whiteTexture)
        {
            return false;
        }
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.debugName = "UI.WhiteTextureView";
        m_whiteTextureView =
            m_device->CreateTextureView(m_whiteTexture.Get(), viewDesc);
        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = whitePixel.size();
        stagingDesc.debugName = "UI.WhiteTextureUpload";
        m_whiteTextureStagingBuffer =
            m_device->CreateStagingBuffer(stagingDesc);
        if (!m_whiteTextureView || !m_whiteTextureStagingBuffer)
        {
            return false;
        }
        void* mapped = m_whiteTextureStagingBuffer->Map(0, whitePixel.size());
        if (!mapped)
        {
            return false;
        }
        std::memcpy(mapped, whitePixel.data(), whitePixel.size());
        m_whiteTextureStagingBuffer->Unmap();
        context.TextureBarrier(m_whiteTexture.Get(),
                               RHIResourceState::Common,
                               RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferRowPitch = 4;
        copyDesc.bufferImageHeight = 1;
        copyDesc.textureRegion = {0, 0, 1, 1};
        context.CopyBufferToTexture(m_whiteTextureStagingBuffer->GetBuffer(),
                                    m_whiteTexture.Get(),
                                    copyDesc);
        context.TextureBarrier(m_whiteTexture.Get(),
                               RHIResourceState::CopyDest,
                               RHIResourceState::ShaderResource);
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "UI.WhiteTextureSet";
        descriptorDesc.SetLayout(layout)
            .BindTexture(0, m_whiteTextureView.Get())
            .BindSampler(1, m_textureSampler.Get());
        m_whiteDescriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
        if (!m_whiteDescriptorSet)
        {
            return false;
        }
        m_submitStats.whiteTextureUploaded = true;
        m_submitStats.whiteTextureBytesUploaded = whitePixel.size();
        return true;
    }

    bool UIPass::EnsureFontAtlasTexture(RHICommandContext& context,
                                        const UIRenderFontAtlas& atlas)
    {
        if (!atlas.IsValid())
        {
            return true;
        }
        if (m_fontAtlasTexture && m_fontAtlasTextureView &&
            m_fontAtlasDescriptorSet &&
            m_uploadedFontAtlasRevision == atlas.revision &&
            m_uploadedFontAtlasWidth == atlas.width &&
            m_uploadedFontAtlasHeight == atlas.height)
        {
            return true;
        }
        RHIDescriptorSetLayout* layout = GetActiveTextureSetLayout();
        if (!layout || !m_fontSampler)
        {
            return false;
        }
        if (m_fontAtlasTexture || m_fontAtlasTextureView ||
            m_fontAtlasStagingBuffer || m_fontAtlasDescriptorSet)
        {
            WaitForOwnerReplacement();
        }
        m_fontAtlasTexture.Reset();
        m_fontAtlasTextureView.Reset();
        m_fontAtlasStagingBuffer.Reset();
        m_fontAtlasDescriptorSet.Reset();

        const std::vector<uint8> rgbaPixels = ExpandAlphaAtlasToRGBA(atlas);
        RHITextureDesc textureDesc = RHITextureDesc::Texture2D(
            atlas.width,
            atlas.height,
            RHIFormat::RGBA8_UNORM,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
        textureDesc.debugName = "UI.FontAtlasTexture";
        m_fontAtlasTexture = m_device->CreateTexture(textureDesc);
        if (!m_fontAtlasTexture)
        {
            return false;
        }
        RHITextureViewDesc viewDesc;
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.format = RHIFormat::RGBA8_UNORM;
        viewDesc.debugName = "UI.FontAtlasTextureView";
        m_fontAtlasTextureView =
            m_device->CreateTextureView(m_fontAtlasTexture.Get(), viewDesc);
        RHIStagingBufferDesc stagingDesc;
        stagingDesc.size = static_cast<uint64>(rgbaPixels.size());
        stagingDesc.debugName = "UI.FontAtlasUpload";
        m_fontAtlasStagingBuffer =
            m_device->CreateStagingBuffer(stagingDesc);
        if (!m_fontAtlasTextureView || !m_fontAtlasStagingBuffer)
        {
            return false;
        }
        void* mapped = m_fontAtlasStagingBuffer->Map(0, stagingDesc.size);
        if (!mapped)
        {
            return false;
        }
        std::memcpy(mapped, rgbaPixels.data(), rgbaPixels.size());
        m_fontAtlasStagingBuffer->Unmap();
        context.TextureBarrier(m_fontAtlasTexture.Get(),
                               RHIResourceState::Common,
                               RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc copyDesc;
        copyDesc.bufferRowPitch = atlas.width * 4u;
        copyDesc.bufferImageHeight = atlas.height;
        copyDesc.textureRegion = {0, 0, atlas.width, atlas.height};
        context.CopyBufferToTexture(m_fontAtlasStagingBuffer->GetBuffer(),
                                    m_fontAtlasTexture.Get(),
                                    copyDesc);
        context.TextureBarrier(m_fontAtlasTexture.Get(),
                               RHIResourceState::CopyDest,
                               RHIResourceState::ShaderResource);
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "UI.FontAtlasTextureSet";
        descriptorDesc.SetLayout(layout)
            .BindTexture(0, m_fontAtlasTextureView.Get())
            .BindSampler(1, m_fontSampler.Get());
        m_fontAtlasDescriptorSet = m_device->CreateDescriptorSet(descriptorDesc);
        if (!m_fontAtlasDescriptorSet)
        {
            return false;
        }
        m_uploadedFontAtlasRevision = atlas.revision;
        m_uploadedFontAtlasWidth = atlas.width;
        m_uploadedFontAtlasHeight = atlas.height;
        m_submitStats.atlasUploaded = true;
        m_submitStats.atlasBytesUploaded = rgbaPixels.size();
        return true;
    }

    RHIDescriptorSet* UIPass::GetOrCreateTextureDescriptor(
        RenderResourceHandle handle,
        IUIRenderResourceResolver* resolver)
    {
        RHIDescriptorSetLayout* layout = GetActiveTextureSetLayout();
        if (!handle.IsValid() || !resolver || !layout || !m_textureSampler)
        {
            return nullptr;
        }
        RHITextureView* resolvedView = resolver->ResolveTextureView(handle);
        if (!resolvedView)
        {
            return nullptr;
        }
        const auto it = std::find_if(
            m_textureDescriptorSets.begin(),
            m_textureDescriptorSets.end(),
            [handle](const TextureDescriptorEntry& entry) {
                return entry.handle == handle;
            });
        if (it != m_textureDescriptorSets.end() &&
            it->resolvedView == resolvedView)
        {
            return it->descriptorSet.Get();
        }
        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.debugName = "UI.ImageTextureSet";
        descriptorDesc.SetLayout(layout)
            .BindTexture(0, resolvedView)
            .BindSampler(1, m_textureSampler.Get());
        RHIDescriptorSetRef descriptorSet =
            m_device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            return nullptr;
        }
        if (it != m_textureDescriptorSets.end())
        {
            WaitForOwnerReplacement();
            it->resolvedView = resolvedView;
            it->descriptorSet = std::move(descriptorSet);
            return it->descriptorSet.Get();
        }
        m_textureDescriptorSets.push_back(
            {handle, resolvedView, std::move(descriptorSet)});
        return m_textureDescriptorSets.back().descriptorSet.Get();
    }

    RHIDescriptorSet* UIPass::GetDescriptorSetForCommand(
        const UIRenderDrawCommand& command,
        IUIRenderResourceResolver* resolver)
    {
        switch (command.resourceKind)
        {
            case UIRenderResourceKind::White:
                return m_whiteDescriptorSet.Get();
            case UIRenderResourceKind::FontAtlas:
                return m_fontAtlasDescriptorSet
                           ? m_fontAtlasDescriptorSet.Get()
                           : m_whiteDescriptorSet.Get();
            case UIRenderResourceKind::Texture:
                return GetOrCreateTextureDescriptor(command.texture, resolver);
        }
        return nullptr;
    }

    RHIDescriptorSetLayout* UIPass::GetActiveTextureSetLayout() const
    {
        return m_textureSetLayoutOverride ? m_textureSetLayoutOverride
                                          : m_textureSetLayout.Get();
    }

    void UIPass::ApplyTextureSetLayoutOverride(
        RHIDescriptorSetLayout* layout)
    {
        if (m_textureSetLayoutOverride == layout)
        {
            return;
        }
        if (m_whiteDescriptorSet || m_fontAtlasDescriptorSet ||
            !m_textureDescriptorSets.empty())
        {
            WaitForOwnerReplacement();
        }
        m_textureSetLayoutOverride = layout;
        m_whiteDescriptorSet.Reset();
        m_fontAtlasDescriptorSet.Reset();
        m_textureDescriptorSets.clear();
    }
} // namespace RVX
