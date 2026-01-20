#include "DX11Device.h"
#include "DX11Resources.h"
#include "DX11Pipeline.h"
#include "DX11CommandContext.h"
#include "DX11SwapChain.h"
#include "DX11/DX11Device.h"

namespace RVX
{
    // =============================================================================
    // Factory Function
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateDX11Device(const RHIDeviceDesc& desc)
    {
        auto device = std::make_unique<DX11Device>();
        if (!device->Initialize(desc))
        {
            RVX_RHI_ERROR("Failed to create DX11 Device");
            return nullptr;
        }
        return device;
    }

    bool IsDX11Available()
    {
        // Try to create a minimal DXGI factory to check DX11 availability
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            return false;
        }

        // Try to find a DX11-capable adapter
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapter.Reset();
                continue;
            }

            // Check if adapter supports D3D11
            ComPtr<ID3D11Device> testDevice;
            D3D_FEATURE_LEVEL featureLevel;
            if (SUCCEEDED(D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION,
                &testDevice, &featureLevel, nullptr)))
            {
                if (featureLevel >= D3D_FEATURE_LEVEL_11_0)
                {
                    return true;
                }
            }

            adapter.Reset();
        }

        return false;
    }

    // =============================================================================
    // Constructor / Destructor
    // =============================================================================
    DX11Device::DX11Device()
    {
    }

    DX11Device::~DX11Device()
    {
        Shutdown();
    }

    // =============================================================================
    // Initialization
    // =============================================================================
    bool DX11Device::Initialize(const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Initializing DX11 Device...");

        if (!CreateFactory())
        {
            return false;
        }

        if (!SelectAdapter(desc.preferredAdapterIndex))
        {
            return false;
        }

        if (!CreateDevice(desc.enableDebugLayer))
        {
            return false;
        }

        // Initialize debug system
        DX11Debug::Get().Initialize(m_device.Get(), desc.enableDebugLayer);

        QueryCapabilities();

        m_initialized = true;
        RVX_RHI_INFO("DX11 Device initialized successfully");
        RVX_RHI_INFO("  Adapter: {}", m_capabilities.adapterName);
        RVX_RHI_INFO("  Feature Level: 0x{:X}", static_cast<uint32>(m_featureLevel));
        RVX_RHI_INFO("  VRAM: {} MB", m_capabilities.dedicatedVideoMemory / (1024 * 1024));

        return true;
    }

    void DX11Device::Shutdown()
    {
        if (!m_initialized) return;

        WaitIdle();

        DX11Debug::Get().Shutdown();

        m_immediateContext1.Reset();
        m_immediateContext.Reset();
        m_device5.Reset();
        m_device1.Reset();
        m_device.Reset();
        m_adapter.Reset();
        m_factory.Reset();

        m_initialized = false;
        RVX_RHI_INFO("DX11 Device shutdown complete");
    }

    // =============================================================================
    // Factory Creation
    // =============================================================================
    bool DX11Device::CreateFactory()
    {
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&m_factory));
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create DXGI Factory: {}", HRESULTToString(hr));
            return false;
        }
        return true;
    }

    // =============================================================================
    // Adapter Selection
    // =============================================================================
    bool DX11Device::SelectAdapter(uint32 preferredIndex)
    {
        std::vector<ComPtr<IDXGIAdapter1>> adapters;

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                adapter.Reset();
                continue;
            }

            // Check if adapter supports D3D11
            ComPtr<ID3D11Device> testDevice;
            D3D_FEATURE_LEVEL featureLevel;
            if (SUCCEEDED(D3D11CreateDevice(
                adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION,
                &testDevice, &featureLevel, nullptr)))
            {
                if (featureLevel >= D3D_FEATURE_LEVEL_11_0)
                {
                    adapters.push_back(adapter);

                    char name[256];
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                    RVX_RHI_DEBUG("Found GPU {}: {} (VRAM: {} MB)",
                        adapters.size() - 1, name, desc.DedicatedVideoMemory / (1024 * 1024));
                }
            }

            adapter.Reset();
        }

        if (adapters.empty())
        {
            RVX_RHI_ERROR("No DX11-capable GPU found");
            return false;
        }

        // Select adapter
        uint32 selectedIndex = 0;
        if (preferredIndex > 0 && preferredIndex < static_cast<uint32>(adapters.size()))
        {
            selectedIndex = preferredIndex;
        }

        m_adapter = adapters[selectedIndex];

        DXGI_ADAPTER_DESC1 selectedDesc;
        m_adapter->GetDesc1(&selectedDesc);

        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, selectedDesc.Description, -1, name, sizeof(name), nullptr, nullptr);
        RVX_RHI_INFO("Selected GPU: {}", name);

        return true;
    }

    // =============================================================================
    // Device Creation
    // =============================================================================
    bool DX11Device::CreateDevice(bool enableDebugLayer)
    {
        UINT createDeviceFlags = 0;

        if (enableDebugLayer)
        {
            createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
            m_debugLayerEnabled = true;
            RVX_RHI_INFO("DX11 Debug Layer requested");
        }

        D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        HRESULT hr = D3D11CreateDevice(
            m_adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            createDeviceFlags,
            featureLevels,
            _countof(featureLevels),
            D3D11_SDK_VERSION,
            &m_device,
            &m_featureLevel,
            &m_immediateContext
        );

        if (FAILED(hr) && (createDeviceFlags & D3D11_CREATE_DEVICE_DEBUG))
        {
            // Debug layer might not be installed, retry without it
            RVX_RHI_WARN("Failed to create device with debug layer, retrying without...");
            createDeviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            m_debugLayerEnabled = false;

            hr = D3D11CreateDevice(
                m_adapter.Get(),
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                createDeviceFlags,
                featureLevels,
                _countof(featureLevels),
                D3D11_SDK_VERSION,
                &m_device,
                &m_featureLevel,
                &m_immediateContext
            );
        }

        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create D3D11 device: {}", HRESULTToString(hr));
            return false;
        }

        // Get optional interfaces
        m_device->QueryInterface(IID_PPV_ARGS(&m_device1));
        m_device->QueryInterface(IID_PPV_ARGS(&m_device5));
        m_immediateContext->QueryInterface(IID_PPV_ARGS(&m_immediateContext1));

        RVX_RHI_INFO("Created D3D11 device with feature level: 0x{:X}", static_cast<uint32>(m_featureLevel));

        return true;
    }

    // =============================================================================
    // Capabilities Query
    // =============================================================================
    void DX11Device::QueryCapabilities()
    {
        m_capabilities.backendType = RHIBackendType::DX11;

        // Adapter info
        DXGI_ADAPTER_DESC1 adapterDesc;
        m_adapter->GetDesc1(&adapterDesc);

        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name), nullptr, nullptr);
        m_capabilities.adapterName = name;
        m_capabilities.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;
        m_capabilities.sharedSystemMemory = adapterDesc.SharedSystemMemory;

        // Feature level
        m_capabilities.dx11.featureLevel = static_cast<uint32>(m_featureLevel);

        // Threading support
        D3D11_FEATURE_DATA_THREADING threadingSupport = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D11_FEATURE_THREADING, &threadingSupport, sizeof(threadingSupport))))
        {
            m_capabilities.dx11.supportsDeferredContext = threadingSupport.DriverCommandLists == TRUE;
            RVX_RHI_DEBUG("DX11 Deferred Context support: {}",
                m_capabilities.dx11.supportsDeferredContext ? "yes" : "no");
        }

        // Common limits
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxTextureSize2D = 16384;
        m_capabilities.maxTextureSize3D = 2048;
        m_capabilities.maxTextureSizeCube = 16384;
        m_capabilities.maxTextureArrayLayers = 2048;
        m_capabilities.maxTextureLayers = 2048;
        m_capabilities.maxColorAttachments = 8;
        m_capabilities.maxPushConstantSize = 256;

        // Compute shader limits (DX11.0)
        m_capabilities.maxComputeWorkGroupSize[0] = 1024;
        m_capabilities.maxComputeWorkGroupSize[1] = 1024;
        m_capabilities.maxComputeWorkGroupSize[2] = 64;
        m_capabilities.maxComputeWorkGroupSizeX = 1024;
        m_capabilities.maxComputeWorkGroupSizeY = 1024;
        m_capabilities.maxComputeWorkGroupSizeZ = 64;
        m_capabilities.maxComputeWorkGroupCount = 65535;

        // DX11 doesn't support these advanced features
        m_capabilities.supportsBindless = false;
        m_capabilities.supportsRaytracing = false;
        m_capabilities.supportsMeshShaders = false;
        m_capabilities.supportsVariableRateShading = false;
        m_capabilities.supportsAsyncCompute = false;  // Single queue

        // Set threading mode
        m_capabilities.dx11.threadingMode = DX11ThreadingMode::SingleThreaded;
        m_capabilities.dx11.minDrawCallsForMultithread = 500;
        m_threadingMode = DX11ThreadingMode::SingleThreaded;
    }

    // =============================================================================
    // Frame Management
    // =============================================================================
    void DX11Device::BeginFrame()
    {
        DX11Debug::Get().BeginFrame(m_totalFrameCount);
    }

    void DX11Device::EndFrame()
    {
        DX11Debug::Get().EndFrame();

        m_frameIndex = (m_frameIndex + 1) % DX11_MAX_FRAME_COUNT;
        m_totalFrameCount++;
    }

    void DX11Device::WaitIdle()
    {
        if (m_immediateContext)
        {
            m_immediateContext->Flush();
        }
    }

    // =============================================================================
    // Deferred Context
    // =============================================================================
    ComPtr<ID3D11DeviceContext> DX11Device::CreateDeferredContext()
    {
        ComPtr<ID3D11DeviceContext> deferredContext;
        HRESULT hr = m_device->CreateDeferredContext(0, &deferredContext);
        if (FAILED(hr))
        {
            RVX_RHI_ERROR("Failed to create deferred context: {}", HRESULTToString(hr));
            return nullptr;
        }
        return deferredContext;
    }

    // =============================================================================
    // Resource Creation
    // =============================================================================
    RHIBufferRef DX11Device::CreateBuffer(const RHIBufferDesc& desc)
    {
        auto buffer = MakeRef<DX11Buffer>(this, desc);
        if (!buffer->GetBuffer())
        {
            return nullptr;
        }
        return buffer;
    }

    RHITextureRef DX11Device::CreateTexture(const RHITextureDesc& desc)
    {
        auto texture = MakeRef<DX11Texture>(this, desc);
        if (!texture->GetResource())
        {
            return nullptr;
        }
        return texture;
    }

    RHITextureViewRef DX11Device::CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
    {
        if (!texture)
        {
            RVX_RHI_ERROR("DX11: Cannot create texture view from null texture");
            return nullptr;
        }
        return MakeRef<DX11TextureView>(this, texture, desc);
    }

    RHISamplerRef DX11Device::CreateSampler(const RHISamplerDesc& desc)
    {
        auto sampler = MakeRef<DX11Sampler>(this, desc);
        if (!sampler->GetSampler())
        {
            return nullptr;
        }
        return sampler;
    }

    RHIShaderRef DX11Device::CreateShader(const RHIShaderDesc& desc)
    {
        auto shader = MakeRef<DX11Shader>(this, desc);
        // Check if shader was created based on stage
        bool valid = false;
        switch (desc.stage)
        {
            case RHIShaderStage::Vertex:   valid = shader->GetVertexShader() != nullptr; break;
            case RHIShaderStage::Pixel:    valid = shader->GetPixelShader() != nullptr; break;
            case RHIShaderStage::Geometry: valid = shader->GetGeometryShader() != nullptr; break;
            case RHIShaderStage::Hull:     valid = shader->GetHullShader() != nullptr; break;
            case RHIShaderStage::Domain:   valid = shader->GetDomainShader() != nullptr; break;
            case RHIShaderStage::Compute:  valid = shader->GetComputeShader() != nullptr; break;
            default: break;
        }
        if (!valid)
        {
            return nullptr;
        }
        return shader;
    }

    // =============================================================================
    // Heap Management - DX11 doesn't support this
    // =============================================================================
    RHIHeapRef DX11Device::CreateHeap(const RHIHeapDesc& desc)
    {
        RVX_RHI_WARN("DX11 does not support explicit heap management");
        (void)desc;
        return nullptr;
    }

    RHITextureRef DX11Device::CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        RVX_RHI_WARN("DX11 does not support placed textures, creating standalone texture");
        (void)heap;
        (void)offset;
        return CreateTexture(desc);
    }

    RHIBufferRef DX11Device::CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        RVX_RHI_WARN("DX11 does not support placed buffers, creating standalone buffer");
        (void)heap;
        (void)offset;
        return CreateBuffer(desc);
    }

    IRHIDevice::MemoryRequirements DX11Device::GetTextureMemoryRequirements(const RHITextureDesc& desc)
    {
        // DX11 doesn't expose memory requirements, estimate based on format
        uint64 size = desc.width * desc.height * GetFormatBytesPerPixel(desc.format);
        size *= desc.arraySize * desc.mipLevels;
        return { size, 256 };
    }

    IRHIDevice::MemoryRequirements DX11Device::GetBufferMemoryRequirements(const RHIBufferDesc& desc)
    {
        return { desc.size, 16 };
    }

    // =============================================================================
    // Pipeline Creation
    // =============================================================================
    RHIDescriptorSetLayoutRef DX11Device::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
    {
        return MakeRef<DX11DescriptorSetLayout>(this, desc);
    }

    RHIPipelineLayoutRef DX11Device::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
    {
        return MakeRef<DX11PipelineLayout>(this, desc);
    }

    RHIPipelineRef DX11Device::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        return MakeRef<DX11GraphicsPipeline>(this, desc);
    }

    RHIPipelineRef DX11Device::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        return MakeRef<DX11ComputePipeline>(this, desc);
    }

    RHIDescriptorSetRef DX11Device::CreateDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        return MakeRef<DX11DescriptorSet>(this, desc);
    }

    // =============================================================================
    // Command Context
    // =============================================================================
    RHICommandContextRef DX11Device::CreateCommandContext(RHICommandQueueType type)
    {
        // DX11 only has one queue, all work goes through immediate/deferred context
        return MakeRef<DX11CommandContext>(this, type);
    }

    void DX11Device::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
    {
        if (!context) return;

        auto* dx11Context = static_cast<DX11CommandContext*>(context);
        dx11Context->Submit();

        if (signalFence)
        {
            auto* dx11Fence = static_cast<DX11Fence*>(signalFence);
            dx11Fence->Signal(dx11Fence->GetCompletedValue() + 1);
        }
    }

    void DX11Device::SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        for (auto* context : contexts)
        {
            if (context)
            {
                auto* dx11Context = static_cast<DX11CommandContext*>(context);
                dx11Context->Submit();
            }
        }

        if (signalFence)
        {
            auto* dx11Fence = static_cast<DX11Fence*>(signalFence);
            dx11Fence->Signal(dx11Fence->GetCompletedValue() + 1);
        }
    }

    // =============================================================================
    // SwapChain
    // =============================================================================
    RHISwapChainRef DX11Device::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        auto swapChain = MakeRef<DX11SwapChain>(this, desc);
        if (!swapChain->GetSwapChain())
        {
            return nullptr;
        }
        return swapChain;
    }

    // =============================================================================
    // Synchronization
    // =============================================================================
    RHIFenceRef DX11Device::CreateFence(uint64 initialValue)
    {
        return MakeRef<DX11Fence>(this, initialValue);
    }

    void DX11Device::WaitForFence(RHIFence* fence, uint64 value)
    {
        if (fence)
        {
            fence->Wait(value, UINT64_MAX);
        }
    }

} // namespace RVX
