#pragma once

#include "DX11Common.h"
#include "RHI/RHISwapChain.h"

#include <memory>
#include <vector>

namespace RVX
{
    class DX11Device;
    class DX11Texture;
    class DX11TextureView;

    // =============================================================================
    // DX11 SwapChain (Phase 5)
    // =============================================================================
    class DX11SwapChain : public RHISwapChain
    {
    public:
        DX11SwapChain(DX11Device* device, const RHISwapChainDesc& desc);
        ~DX11SwapChain() override;

        // RHISwapChain interface
        RHITexture* GetCurrentBackBuffer() override;
        RHITextureView* GetCurrentBackBufferView() override;
        // For FLIP model, we only have access to buffer 0 via GetBuffer(0)
        // The internal m_currentBackBuffer tracks the logical rotation for Present timing
        // but externally we always return 0 since that's the only accessible buffer
        uint32 GetCurrentBackBufferIndex() const override { return 0; }
        void Present() override;
        void Resize(uint32 width, uint32 height) override;
        uint32 GetWidth() const override { return m_width; }
        uint32 GetHeight() const override { return m_height; }
        RHIFormat GetFormat() const override { return m_format; }
        uint32 GetBufferCount() const override { return m_backBufferCount; }

        // DX11 Specific
        IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }

    private:
        bool CreateSwapChain(const RHISwapChainDesc& desc);
        void CreateBackBufferViews();
        void ReleaseBackBuffers();
        bool CheckTearingSupport();

        DX11Device* m_device = nullptr;

        ComPtr<IDXGISwapChain1> m_swapChain;
        std::vector<std::unique_ptr<DX11Texture>> m_backBuffers;
        std::vector<std::unique_ptr<DX11TextureView>> m_backBufferViews;

        uint32 m_width = 0;
        uint32 m_height = 0;
        uint32 m_backBufferCount = 0;
        uint32 m_currentBackBuffer = 0;
        RHIFormat m_format = RHIFormat::RGBA8_UNORM;
        RHIFormat m_requestedFormat = RHIFormat::RGBA8_UNORM;  // Original requested format (may be SRGB)
        bool m_vsyncEnabled = true;
        bool m_isFlipModel = false;        // Whether using FLIP swap effect
        bool m_tearingSupported = false;   // Whether tearing (variable refresh rate) is supported
    };

} // namespace RVX
