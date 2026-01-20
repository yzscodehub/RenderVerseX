#include "DX11SwapChain.h"
#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Conversions.h"

namespace RVX
{
    DX11SwapChain::DX11SwapChain(DX11Device* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_backBufferCount(desc.bufferCount)
        , m_format(desc.format)
        , m_vsyncEnabled(desc.vsync)
    {
        if (!CreateSwapChain(desc))
        {
            RVX_RHI_ERROR("Failed to create DX11 swap chain");
        }
    }

    DX11SwapChain::~DX11SwapChain()
    {
        ReleaseBackBuffers();

        // Before releasing swap chain, exit fullscreen if needed
        BOOL isFullscreen = FALSE;
        if (m_swapChain && SUCCEEDED(m_swapChain->GetFullscreenState(&isFullscreen, nullptr)))
        {
            if (isFullscreen)
            {
                m_swapChain->SetFullscreenState(FALSE, nullptr);
            }
        }
    }

    // Helper to get non-SRGB format for swap chain buffer (Flip model doesn't support SRGB)
    static DXGI_FORMAT GetSwapChainBufferFormat(RHIFormat format)
    {
        DXGI_FORMAT dxgiFormat = ToDXGIFormat(format);
        // Flip model swap chains don't support SRGB formats directly
        // Use the non-SRGB format for the buffer, SRGB for the RTV
        switch (dxgiFormat)
        {
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
            default: return dxgiFormat;
        }
    }

    bool DX11SwapChain::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        // Store the requested format for RTV creation
        m_requestedFormat = desc.format;
        
        // For DISCARD mode, we can use SRGB format directly
        // For FLIP model, we would need non-SRGB format for the buffer
        DXGI_FORMAT bufferFormat = ToDXGIFormat(desc.format);
        
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = desc.width;
        swapChainDesc.Height = desc.height;
        swapChainDesc.Format = bufferFormat;
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        // DISCARD mode requires BufferCount = 1, FLIP mode requires >= 2
        swapChainDesc.BufferCount = 1;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        // Use legacy DISCARD mode instead of FLIP_DISCARD for better compatibility
        // FLIP model has issues with buffer management in some scenarios
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        HWND hwnd = reinterpret_cast<HWND>(desc.windowHandle);

        HRESULT hr = m_device->GetDXGIFactory()->CreateSwapChainForHwnd(
            m_device->GetD3DDevice(),
            hwnd,
            &swapChainDesc,
            nullptr,  // No fullscreen desc
            nullptr,  // No restrict output
            &m_swapChain
        );

        // Using legacy DISCARD mode with single buffer
        m_isFlipModel = false;
        m_backBufferCount = 1;  // DISCARD mode only uses 1 buffer
        
        if (SUCCEEDED(hr))
        {
            RVX_RHI_DEBUG("DX11 SwapChain using DISCARD model");
        }

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create swap chain: {}", HRESULTToString(hr));
            return false;
        }

        // Disable Alt+Enter fullscreen toggle (handle it ourselves)
        m_device->GetDXGIFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        CreateBackBufferViews();

        RVX_RHI_INFO("DX11 SwapChain created: {}x{}, {} buffers",
            m_width, m_height, m_backBufferCount);

        return true;
    }

    void DX11SwapChain::CreateBackBufferViews()
    {
        m_backBuffers.clear();
        m_backBufferViews.clear();
        
        // For DX11 FLIP swap chains, we can only access buffer 0
        // The buffer rotates automatically after Present
        m_backBuffers.resize(1);
        m_backBufferViews.resize(1);

        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to get back buffer: {}", HRESULTToString(hr));
            return;
        }

        // Get the actual buffer format from the swap chain
        D3D11_TEXTURE2D_DESC texDesc2D;
        backBuffer->GetDesc(&texDesc2D);

        RHITextureDesc texDesc = {};
        texDesc.width = m_width;
        texDesc.height = m_height;
        texDesc.depth = 1;
        texDesc.mipLevels = 1;
        texDesc.arraySize = 1;
        texDesc.format = m_format;  // Use the original format
        texDesc.usage = RHITextureUsage::RenderTarget;
        texDesc.dimension = RHITextureDimension::Texture2D;
        texDesc.debugName = "SwapChainBackBuffer";

        m_backBuffers[0] = std::make_unique<DX11Texture>(m_device, backBuffer, texDesc);

        // Create view with the originally requested format (may be SRGB)
        // This allows SRGB gamma correction when rendering to the swap chain
        RHITextureViewDesc viewDesc = {};
        viewDesc.format = m_requestedFormat;  // Use requested format (SRGB if requested)
        m_backBufferViews[0] = std::make_unique<DX11TextureView>(m_device, m_backBuffers[0].get(), viewDesc);
    }

    void DX11SwapChain::ReleaseBackBuffers()
    {
        m_backBufferViews.clear();
        m_backBuffers.clear();
    }

    void DX11SwapChain::Resize(uint32 width, uint32 height)
    {
        if (width == 0 || height == 0) return;
        if (width == m_width && height == m_height) return;

        RVX_RHI_INFO("DX11 SwapChain resize: {}x{} -> {}x{}",
            m_width, m_height, width, height);

        // Release back buffers before resize
        ReleaseBackBuffers();

        // Resize swap chain (use 0 for buffer count to keep existing count)
        HRESULT hr = m_swapChain->ResizeBuffers(
            0,
            width,
            height,
            DXGI_FORMAT_UNKNOWN,  // Keep existing format
            DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
        );

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to resize swap chain: {}", HRESULTToString(hr));
            return;
        }

        m_width = width;
        m_height = height;
        m_currentBackBuffer = 0;

        CreateBackBufferViews();
    }

    RHITexture* DX11SwapChain::GetCurrentBackBuffer()
    {
        // DX11 FLIP swap chains only expose buffer 0
        if (!m_backBuffers.empty())
        {
            return m_backBuffers[0].get();
        }
        return nullptr;
    }

    RHITextureView* DX11SwapChain::GetCurrentBackBufferView()
    {
        // DX11 FLIP swap chains only expose buffer 0
        if (!m_backBufferViews.empty())
        {
            return m_backBufferViews[0].get();
        }
        return nullptr;
    }

    void DX11SwapChain::Present()
    {
        UINT syncInterval = m_vsyncEnabled ? 1 : 0;
        UINT presentFlags = 0;

        // Note: DXGI_PRESENT_ALLOW_TEARING requires DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        // which is not set during swap chain creation, so we don't use tearing for now

        HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            DX11Debug::Get().DiagnoseDeviceRemoved(m_device->GetD3DDevice());
            return;
        }

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("SwapChain present failed: {}", HRESULTToString(hr));
            return;
        }

        // Note: For DX11 FLIP model, we always access buffer 0 via GetBuffer(0).
        // The buffer rotation is handled internally by DXGI.
        // m_currentBackBuffer is not used externally (GetCurrentBackBufferIndex always returns 0).
    }

} // namespace RVX
