#pragma once

#include "MetalCommon.h"
#include "RHI/RHISwapChain.h"

namespace RVX
{
    class MetalDevice;

    // =============================================================================
    // Metal SwapChain
    // Wraps CAMetalLayer for window presentation
    // =============================================================================
    class MetalSwapChain : public RHISwapChain
    {
    public:
        MetalSwapChain(MetalDevice* device, const RHISwapChainDesc& desc);
        ~MetalSwapChain() override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return m_bufferCount; }
        uint32 GetCurrentBackBufferIndex() const override { return m_currentBackBufferIndex; }

        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;

        void Resize(uint32 width, uint32 height) override;
        void Present() override;

        // Get current drawable for command buffer presentation
        id<CAMetalDrawable> GetCurrentDrawable() const { return m_currentDrawable; }
        bool HasPendingDrawable() const { return m_currentDrawable != nil; }

        // Called after present to advance to next frame
        void AdvanceFrame();

    private:
        void CreateBackBuffers();

        MetalDevice* m_device = nullptr;
        CAMetalLayer* m_metalLayer = nil;

        uint32 m_width = 0;
        uint32 m_height = 0;
        RHIFormat m_format = RHIFormat::BGRA8_UNORM;
        uint32 m_bufferCount = 3;
        bool m_vsync = true;

        uint32 m_currentBackBufferIndex = 0;
        id<CAMetalDrawable> m_currentDrawable = nil;

        std::vector<RHITextureRef> m_backBuffers;
        std::vector<RHITextureViewRef> m_backBufferViews;
    };

} // namespace RVX
