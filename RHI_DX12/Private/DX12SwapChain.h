#pragma once

#include "DX12Common.h"
#include "DX12Resources.h"
#include "RHI/RHISwapChain.h"

namespace RVX
{
    class DX12Device;

    // =============================================================================
    // DX12 SwapChain Implementation
    // =============================================================================
    class DX12SwapChain : public RHISwapChain
    {
    public:
        DX12SwapChain(DX12Device* device, const RHISwapChainDesc& desc);
        ~DX12SwapChain() override;

        // RHISwapChain interface
        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;
        uint32 GetCurrentBackBufferIndex() const override { return m_currentBackBufferIndex; }

        void Present() override;
        void Resize(uint32 width, uint32 height) override;

        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return m_bufferCount; }

        // DX12 Specific
        IDXGISwapChain4* GetSwapChain() const { return m_swapChain.Get(); }

    private:
        void CreateBackBufferResources();
        void ReleaseBackBufferResources();

        DX12Device* m_device = nullptr;

        ComPtr<IDXGISwapChain4> m_swapChain;

        uint32 m_width = 0;
        uint32 m_height = 0;
        RHIFormat m_format = RHIFormat::BGRA8_UNORM;
        uint32 m_bufferCount = 3;
        bool m_vsync = true;

        uint32 m_currentBackBufferIndex = 0;

        std::vector<RHITextureRef> m_backBuffers;
        std::vector<RHITextureViewRef> m_backBufferViews;
    };

    // Factory function
    RHISwapChainRef CreateDX12SwapChain(DX12Device* device, const RHISwapChainDesc& desc);

} // namespace RVX
