/**
 * @file UIPass.h
 * @brief Render-owned consumer for backend-neutral UI packets
 */

#pragma once

#include "RenderContracts/UIRenderPacket.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHIDescriptor.h"
#include "RHI/RHIRenderPass.h"
#include "RHI/RHISampler.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIUpload.h"

#include <utility>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RHIPipeline;

    /** @brief Render-side resolver for exact-generation UI texture handles. */
    class IUIRenderResourceResolver
    {
    public:
        virtual ~IUIRenderResourceResolver() = default;
        virtual RHITextureView* ResolveTextureView(
            RenderResourceHandle handle) = 0;
    };

    /** @brief RHI submission inputs owned by the Render layer. */
    struct UIPassSubmitDesc
    {
        RHICommandContext* commandContext = nullptr;
        RHIPipeline* pipeline = nullptr;
        RHIDescriptorSetLayout* textureSetLayout = nullptr;
        RHITextureView* colorTarget = nullptr;
        IUIRenderResourceResolver* resourceResolver = nullptr;
        bool allowDefaultFramebufferTarget = false;
        bool beginRenderPass = true;
        RHILoadOp colorLoadOp = RHILoadOp::Load;
        RHIStoreOp colorStoreOp = RHIStoreOp::Store;
        RHIClearColor clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
    };

    /** @brief Observable submission evidence for tests and diagnostics. */
    struct UIPassSubmitStats
    {
        bool submitted = false;
        bool defaultFramebufferTarget = false;
        bool atlasUploaded = false;
        bool whiteTextureUploaded = false;
        uint32 drawCallCount = 0;
        uint64 vertexBytesUploaded = 0;
        uint64 indexBytesUploaded = 0;
        uint64 atlasBytesUploaded = 0;
        uint64 whiteTextureBytesUploaded = 0;
    };

    /** @brief Sole RHI owner for UI packet realization and drawing. */
    class UIPass final
    {
    public:
        bool Initialize(IRHIDevice* device);
        void Shutdown();

        [[nodiscard]] bool IsInitialized() const noexcept
        {
            return m_device != nullptr;
        }
        bool Submit(const UIRenderPacket& packet,
                    const UIPassSubmitDesc& desc);

        [[nodiscard]] const UIPassSubmitStats& GetSubmitStats() const
        {
            return m_submitStats;
        }
        [[nodiscard]] RHITextureView* GetFontAtlasTextureView() const
        {
            return m_fontAtlasTextureView.Get();
        }
        [[nodiscard]] RHITextureView* GetWhiteTextureView() const
        {
            return m_whiteTextureView.Get();
        }
        [[nodiscard]] RHIBuffer* GetVertexBuffer() const
        {
            return m_vertexBuffer.Get();
        }
        [[nodiscard]] RHIBuffer* GetIndexBuffer() const
        {
            return m_indexBuffer.Get();
        }

    private:
        struct TextureDescriptorEntry
        {
            RenderResourceHandle handle{};
            RHITextureView* resolvedView = nullptr;
            RHIDescriptorSetRef descriptorSet;
        };

        void ResetGPUResources();
        void WaitForOwnerReplacement();
        bool EnsureDynamicBuffers(const UIRenderPacket& packet);
        bool EnsureBuffer(RHIBufferRef& buffer,
                          uint64& capacity,
                          uint64 requiredSize,
                          RHIBufferUsage usage,
                          const char* debugName,
                          uint32 stride);
        bool UploadBuffer(RHIBuffer* buffer, const void* data, uint64 size);
        bool EnsureTextureDescriptorResources();
        bool EnsureWhiteTexture(RHICommandContext& context);
        bool EnsureFontAtlasTexture(RHICommandContext& context,
                                    const UIRenderFontAtlas& atlas);
        RHIDescriptorSet* GetOrCreateTextureDescriptor(
            RenderResourceHandle handle,
            IUIRenderResourceResolver* resolver);
        RHIDescriptorSet* GetDescriptorSetForCommand(
            const UIRenderDrawCommand& command,
            IUIRenderResourceResolver* resolver);
        RHIDescriptorSetLayout* GetActiveTextureSetLayout() const;
        void ApplyTextureSetLayoutOverride(RHIDescriptorSetLayout* layout);

        IRHIDevice* m_device = nullptr;
        RHIBufferRef m_vertexBuffer;
        RHIBufferRef m_indexBuffer;
        uint64 m_vertexBufferCapacity = 0;
        uint64 m_indexBufferCapacity = 0;

        RHITextureRef m_fontAtlasTexture;
        RHITextureViewRef m_fontAtlasTextureView;
        RHIStagingBufferRef m_fontAtlasStagingBuffer;
        RHIDescriptorSetRef m_fontAtlasDescriptorSet;
        uint64 m_uploadedFontAtlasRevision = 0;
        uint32 m_uploadedFontAtlasWidth = 0;
        uint32 m_uploadedFontAtlasHeight = 0;

        RHITextureRef m_whiteTexture;
        RHITextureViewRef m_whiteTextureView;
        RHIStagingBufferRef m_whiteTextureStagingBuffer;
        RHIDescriptorSetRef m_whiteDescriptorSet;

        RHISamplerRef m_textureSampler;
        RHISamplerRef m_fontSampler;
        RHIDescriptorSetLayoutRef m_textureSetLayout;
        RHIDescriptorSetLayout* m_textureSetLayoutOverride = nullptr;
        std::vector<TextureDescriptorEntry> m_textureDescriptorSets;
        UIPassSubmitStats m_submitStats;
    };
} // namespace RVX
