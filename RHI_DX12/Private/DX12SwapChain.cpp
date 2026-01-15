#include "DX12SwapChain.h"
#include "DX12Device.h"

namespace RVX
{
    DX12SwapChain::DX12SwapChain(DX12Device* device, const RHISwapChainDesc& desc)
        : m_device(device)
        , m_width(desc.width)
        , m_height(desc.height)
        , m_format(desc.format)
        , m_bufferCount(desc.bufferCount)
        , m_vsync(desc.vsync)
    {
        if (desc.debugName)
        {
            SetDebugName(desc.debugName);
        }

        HWND hwnd = static_cast<HWND>(desc.windowHandle);
        if (!hwnd)
        {
            RVX_RHI_ERROR("Invalid window handle for swap chain");
            return;
        }

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.Width = desc.width;
        swapChainDesc.Height = desc.height;
        swapChainDesc.Format = ToDXGIFormat(desc.format);
        swapChainDesc.Stereo = FALSE;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.BufferCount = desc.bufferCount;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr = device->GetDXGIFactory()->CreateSwapChainForHwnd(
            device->GetGraphicsQueue(),
            hwnd,
            &swapChainDesc,
            nullptr,
            nullptr,
            &swapChain1);
        if (FAILED(hr))
        {
            // Some drivers reject sRGB swapchains; fall back to UNORM.
            if (desc.format == RHIFormat::BGRA8_UNORM_SRGB)
            {
                RVX_RHI_WARN("DX12 swapchain sRGB not supported (0x{:08X}), falling back to UNORM",
                    static_cast<uint32>(hr));
                swapChainDesc.Format = ToDXGIFormat(RHIFormat::BGRA8_UNORM);
                hr = device->GetDXGIFactory()->CreateSwapChainForHwnd(
                    device->GetGraphicsQueue(),
                    hwnd,
                    &swapChainDesc,
                    nullptr,
                    nullptr,
                    &swapChain1);
                if (SUCCEEDED(hr))
                {
                    m_format = RHIFormat::BGRA8_UNORM;
                }
            }
        }
        DX12_CHECK(hr);

        // Disable Alt+Enter fullscreen toggle
        device->GetDXGIFactory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

        DX12_CHECK(swapChain1.As(&m_swapChain));

        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

        CreateBackBufferResources();

        RVX_RHI_INFO("DX12 SwapChain created: {}x{}, {} buffers, format {}",
            desc.width, desc.height, desc.bufferCount, static_cast<int>(desc.format));
    }

    DX12SwapChain::~DX12SwapChain()
    {
        ReleaseBackBufferResources();
    }

    void DX12SwapChain::CreateBackBufferResources()
    {
        m_backBuffers.resize(m_bufferCount);
        m_backBufferViews.resize(m_bufferCount);

        for (uint32 i = 0; i < m_bufferCount; ++i)
        {
            ComPtr<ID3D12Resource> backBuffer;
            DX12_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

            RHITextureDesc textureDesc;
            textureDesc.width = m_width;
            textureDesc.height = m_height;
            textureDesc.depth = 1;
            textureDesc.mipLevels = 1;
            textureDesc.arraySize = 1;
            textureDesc.format = m_format;
            textureDesc.usage = RHITextureUsage::RenderTarget;
            textureDesc.dimension = RHITextureDimension::Texture2D;
            textureDesc.sampleCount = RHISampleCount::Count1;

            char debugName[64];
            snprintf(debugName, sizeof(debugName), "SwapChain BackBuffer %u", i);
            textureDesc.debugName = debugName;

            m_backBuffers[i] = CreateDX12TextureFromResource(m_device, backBuffer, textureDesc);

            // Create view for the back buffer
            RHITextureViewDesc viewDesc;
            viewDesc.format = m_format;
            m_backBufferViews[i] = m_device->CreateTextureView(m_backBuffers[i].Get(), viewDesc);
        }
    }

    void DX12SwapChain::ReleaseBackBufferResources()
    {
        m_backBufferViews.clear();
        m_backBuffers.clear();
    }

    RHITexture* DX12SwapChain::GetCurrentBackBuffer()
    {
        return m_backBuffers[m_currentBackBufferIndex].Get();
    }

    RHITextureView* DX12SwapChain::GetCurrentBackBufferView()
    {
        return m_backBufferViews[m_currentBackBufferIndex].Get();
    }

    void DX12SwapChain::Present()
    {
        UINT syncInterval = m_vsync ? 1 : 0;
        UINT presentFlags = 0;

        if (!m_vsync)
        {
            // Allow tearing for variable refresh rate displays
            ComPtr<IDXGIFactory5> factory5;
            if (SUCCEEDED(m_device->GetDXGIFactory()->QueryInterface(IID_PPV_ARGS(&factory5))))
            {
                BOOL allowTearing = FALSE;
                if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing))))
                {
                    if (allowTearing)
                    {
                        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
                    }
                }
            }
        }

        HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);

        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        {
            RVX_RHI_ERROR("Device lost during Present! HRESULT: 0x{:08X}", static_cast<uint32>(hr));
            // TODO: Handle device lost
        }

        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    void DX12SwapChain::Resize(uint32 width, uint32 height)
    {
        if (width == m_width && height == m_height)
            return;

        if (width == 0 || height == 0)
        {
            RVX_RHI_WARN("Ignoring resize to 0x0");
            return;
        }

        RVX_RHI_DEBUG("Resizing swap chain: {}x{} -> {}x{}", m_width, m_height, width, height);

        // Wait for GPU to finish using the buffers
        m_device->WaitIdle();

        // Release old resources
        ReleaseBackBufferResources();

        // Resize buffers
        DXGI_SWAP_CHAIN_DESC1 desc;
        m_swapChain->GetDesc1(&desc);

        DX12_CHECK(m_swapChain->ResizeBuffers(
            m_bufferCount,
            width,
            height,
            desc.Format,
            desc.Flags));

        m_width = width;
        m_height = height;
        m_currentBackBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

        // Recreate resources
        CreateBackBufferResources();
    }

    // =============================================================================
    // Factory Function
    // =============================================================================
    RHISwapChainRef CreateDX12SwapChain(DX12Device* device, const RHISwapChainDesc& desc)
    {
        return Ref<DX12SwapChain>(new DX12SwapChain(device, desc));
    }

} // namespace RVX
