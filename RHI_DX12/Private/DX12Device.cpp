#include "DX12Device.h"
#include "DX12Resources.h"
#include "DX12SwapChain.h"
#include "DX12CommandContext.h"
#include "DX12Pipeline.h"
#include "Core/Log.h"

namespace RVX
{
    // =============================================================================
    // Factory Function
    // =============================================================================
    std::unique_ptr<IRHIDevice> CreateDX12Device(const RHIDeviceDesc& desc)
    {
        auto device = std::make_unique<DX12Device>();
        if (!device->Initialize(desc))
        {
            RVX_RHI_ERROR("Failed to create DX12 Device");
            return nullptr;
        }
        return device;
    }

    // =============================================================================
    // Constructor / Destructor
    // =============================================================================
    DX12Device::DX12Device()
    {
    }

    DX12Device::~DX12Device()
    {
        Shutdown();
    }

    // =============================================================================
    // Initialization
    // =============================================================================
    bool DX12Device::Initialize(const RHIDeviceDesc& desc)
    {
        RVX_RHI_INFO("Initializing DX12 Device...");

        if (desc.enableDebugLayer)
        {
            EnableDebugLayer(desc.enableGPUValidation);
        }

        if (!CreateFactory(desc.enableDebugLayer))
        {
            return false;
        }

        if (!SelectAdapter(desc.preferredAdapterIndex))
        {
            return false;
        }

        if (!CreateDevice(desc.enableGPUValidation))
        {
            return false;
        }

        if (!CreateCommandQueues())
        {
            return false;
        }

        // Initialize descriptor heap manager
        m_descriptorHeapManager.Initialize(m_device.Get());

        // Create frame fence
        DX12_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)));
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent)
        {
            RVX_RHI_ERROR("Failed to create fence event");
            return false;
        }

        // Initialize D3D12 Memory Allocator
        #ifdef RVX_USE_D3D12MA
        D3D12MA::ALLOCATOR_DESC allocatorDesc = {};
        allocatorDesc.pDevice = m_device.Get();
        allocatorDesc.pAdapter = m_adapter.Get();
        allocatorDesc.Flags = D3D12MA::ALLOCATOR_FLAG_NONE;

        if (FAILED(D3D12MA::CreateAllocator(&allocatorDesc, &m_memoryAllocator)))
        {
            RVX_RHI_WARN("Failed to create D3D12 Memory Allocator, using committed resources");
        }
        else
        {
            RVX_RHI_INFO("D3D12 Memory Allocator initialized");
        }
        #endif

        if (!InitializeCapabilities())
        {
            return false;
        }

        RVX_RHI_INFO("DX12 Device initialized successfully");
        RVX_RHI_INFO("  Adapter: {}", m_capabilities.adapterName);
        RVX_RHI_INFO("  VRAM: {} MB", m_capabilities.dedicatedVideoMemory / (1024 * 1024));

        return true;
    }

    void DX12Device::Shutdown()
    {
        WaitIdle();

        if (m_fenceEvent)
        {
            CloseHandle(m_fenceEvent);
            m_fenceEvent = nullptr;
        }

        m_descriptorHeapManager.Shutdown();

        #ifdef RVX_USE_D3D12MA
        m_memoryAllocator.Reset();
        #endif

        m_frameFence.Reset();
        m_copyQueue.Reset();
        m_computeQueue.Reset();
        m_graphicsQueue.Reset();
        m_device.Reset();
        m_adapter.Reset();
        m_factory.Reset();

        RVX_RHI_INFO("DX12 Device shutdown complete");
    }

    // =============================================================================
    // Debug Layer
    // =============================================================================
    void DX12Device::EnableDebugLayer(bool enableGPUValidation)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            m_debugLayerEnabled = true;
            RVX_RHI_INFO("DX12 Debug Layer enabled");

            if (enableGPUValidation)
            {
                ComPtr<ID3D12Debug1> debugController1;
                if (SUCCEEDED(debugController.As(&debugController1)))
                {
                    debugController1->SetEnableGPUBasedValidation(TRUE);
                    RVX_RHI_INFO("DX12 GPU-based validation enabled");
                }
            }
        }
        else
        {
            RVX_RHI_WARN("Failed to enable DX12 Debug Layer");
        }
    }

    // =============================================================================
    // Factory Creation
    // =============================================================================
    bool DX12Device::CreateFactory(bool enableDebugLayer)
    {
        UINT dxgiFlags = 0;
        if (enableDebugLayer)
        {
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }

        if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory))))
        {
            RVX_RHI_ERROR("Failed to create DXGI Factory");
            return false;
        }

        return true;
    }

    // =============================================================================
    // Adapter Selection
    // =============================================================================
    bool DX12Device::SelectAdapter(uint32 preferredIndex)
    {
        ComPtr<IDXGIAdapter1> adapter;

        // Enumerate adapters and select the best one
        std::vector<ComPtr<IDXGIAdapter4>> adapters;

        for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(
                i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);

            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            {
                continue;
            }

            // Check if adapter supports D3D12
            if (SUCCEEDED(D3D12CreateDevice(
                    adapter.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr)))
            {
                ComPtr<IDXGIAdapter4> adapter4;
                if (SUCCEEDED(adapter.As(&adapter4)))
                {
                    adapters.push_back(adapter4);

                    char name[256];
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                    RVX_RHI_DEBUG("Found GPU {}: {} (VRAM: {} MB)",
                        adapters.size() - 1, name, desc.DedicatedVideoMemory / (1024 * 1024));
                }
            }
        }

        if (adapters.empty())
        {
            RVX_RHI_ERROR("No suitable GPU found");
            return false;
        }

        // Select adapter
        uint32 selectedIndex = 0;
        if (preferredIndex > 0 && preferredIndex < adapters.size())
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
    bool DX12Device::CreateDevice(bool enableGPUValidation)
    {
        // Try to create device with highest feature level first
        D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_12_2,
            D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0,
        };

        for (auto featureLevel : featureLevels)
        {
            if (SUCCEEDED(D3D12CreateDevice(
                    m_adapter.Get(), featureLevel, IID_PPV_ARGS(&m_device))))
            {
                RVX_RHI_INFO("Created D3D12 device with feature level: 0x{:X}", static_cast<int>(featureLevel));
                return true;
            }
        }

        RVX_RHI_ERROR("Failed to create D3D12 device");
        return false;
    }

    // =============================================================================
    // Command Queue Creation
    // =============================================================================
    bool DX12Device::CreateCommandQueues()
    {
        // Graphics Queue
        D3D12_COMMAND_QUEUE_DESC graphicsQueueDesc = {};
        graphicsQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        graphicsQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        graphicsQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        graphicsQueueDesc.NodeMask = 0;

        if (FAILED(m_device->CreateCommandQueue(&graphicsQueueDesc, IID_PPV_ARGS(&m_graphicsQueue))))
        {
            RVX_RHI_ERROR("Failed to create graphics command queue");
            return false;
        }

        // Compute Queue
        D3D12_COMMAND_QUEUE_DESC computeQueueDesc = {};
        computeQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        computeQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        computeQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        computeQueueDesc.NodeMask = 0;

        if (FAILED(m_device->CreateCommandQueue(&computeQueueDesc, IID_PPV_ARGS(&m_computeQueue))))
        {
            RVX_RHI_ERROR("Failed to create compute command queue");
            return false;
        }

        // Copy Queue
        D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
        copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        copyQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        copyQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        copyQueueDesc.NodeMask = 0;

        if (FAILED(m_device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_copyQueue))))
        {
            RVX_RHI_ERROR("Failed to create copy command queue");
            return false;
        }

        RVX_RHI_DEBUG("Command queues created (Graphics, Compute, Copy)");
        return true;
    }

    ID3D12CommandQueue* DX12Device::GetQueue(RHICommandQueueType type) const
    {
        switch (type)
        {
            case RHICommandQueueType::Graphics: return m_graphicsQueue.Get();
            case RHICommandQueueType::Compute:  return m_computeQueue.Get();
            case RHICommandQueueType::Copy:     return m_copyQueue.Get();
            default: return m_graphicsQueue.Get();
        }
    }

    // =============================================================================
    // Capabilities
    // =============================================================================
    bool DX12Device::InitializeCapabilities()
    {
        m_capabilities.backendType = RHIBackendType::DX12;

        DXGI_ADAPTER_DESC1 adapterDesc;
        m_adapter->GetDesc1(&adapterDesc);

        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name), nullptr, nullptr);
        m_capabilities.adapterName = name;
        m_capabilities.dedicatedVideoMemory = adapterDesc.DedicatedVideoMemory;

        // Query feature support
        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));

        m_capabilities.dx12.resourceBindingTier = static_cast<uint32>(options.ResourceBindingTier);

        // Root signature version
        D3D12_FEATURE_DATA_ROOT_SIGNATURE rootSigFeature = {};
        rootSigFeature.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &rootSigFeature, sizeof(rootSigFeature))))
        {
            m_capabilities.dx12.supportsRootSignature1_1 = (rootSigFeature.HighestVersion >= D3D_ROOT_SIGNATURE_VERSION_1_1);
        }

        // Shader model
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_6 };
        m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));

        // Common limits
        m_capabilities.maxTextureSize = 16384;
        m_capabilities.maxTextureLayers = 2048;
        m_capabilities.maxColorAttachments = 8;
        m_capabilities.maxComputeWorkGroupSize[0] = 1024;
        m_capabilities.maxComputeWorkGroupSize[1] = 1024;
        m_capabilities.maxComputeWorkGroupSize[2] = 64;
        m_capabilities.maxPushConstantSize = 256;  // 64 DWORDs

        // Bindless support
        m_capabilities.supportsBindless = (options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2);
        if (m_capabilities.supportsBindless)
        {
            m_capabilities.maxBindlessTextures = 1000000;
            m_capabilities.maxBindlessBuffers = 500000;
        }

        // Feature support
        m_capabilities.supportsAsyncCompute = true;

        // Raytracing
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))))
        {
            m_capabilities.supportsRaytracing = (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
        }

        // Mesh shaders
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7))))
        {
            m_capabilities.supportsMeshShaders = (options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
        }

        // VRS
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))))
        {
            m_capabilities.supportsVariableRateShading = (options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_1);
        }

        return true;
    }

    // =============================================================================
    // Frame Management
    // =============================================================================
    void DX12Device::BeginFrame()
    {
        // Wait for the frame we're about to render to complete on GPU
        uint64 fenceValue = m_frameFenceValues[m_frameIndex];
        if (m_frameFence->GetCompletedValue() < fenceValue)
        {
            m_frameFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
            WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
        }

        // Reset transient descriptor heaps
        m_descriptorHeapManager.ResetTransientHeaps();
    }

    void DX12Device::EndFrame()
    {
        // Signal fence for this frame
        m_frameFenceValues[m_frameIndex] = m_frameFence->GetCompletedValue() + 1;
        m_graphicsQueue->Signal(m_frameFence.Get(), m_frameFenceValues[m_frameIndex]);

        // Advance frame index
        m_frameIndex = (m_frameIndex + 1) % RVX_MAX_FRAME_COUNT;
    }

    void DX12Device::WaitIdle()
    {
        // Signal and wait for all queues
        uint64 fenceValue = m_frameFence->GetCompletedValue() + 1;

        m_graphicsQueue->Signal(m_frameFence.Get(), fenceValue);
        m_frameFence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
    }

    // =============================================================================
    // Resource Creation - Placeholders (implemented in DX12Resources.cpp)
    // =============================================================================
    RHIBufferRef DX12Device::CreateBuffer(const RHIBufferDesc& desc)
    {
        return CreateDX12Buffer(this, desc);
    }

    RHITextureRef DX12Device::CreateTexture(const RHITextureDesc& desc)
    {
        return CreateDX12Texture(this, desc);
    }

    RHITextureViewRef DX12Device::CreateTextureView(RHITexture* texture, const RHITextureViewDesc& desc)
    {
        return CreateDX12TextureView(this, texture, desc);
    }

    RHISamplerRef DX12Device::CreateSampler(const RHISamplerDesc& desc)
    {
        return CreateDX12Sampler(this, desc);
    }

    RHIShaderRef DX12Device::CreateShader(const RHIShaderDesc& desc)
    {
        return CreateDX12Shader(this, desc);
    }

    // =============================================================================
    // Pipeline Creation
    // =============================================================================
    RHIDescriptorSetLayoutRef DX12Device::CreateDescriptorSetLayout(const RHIDescriptorSetLayoutDesc& desc)
    {
        return CreateDX12DescriptorSetLayout(this, desc);
    }

    RHIPipelineLayoutRef DX12Device::CreatePipelineLayout(const RHIPipelineLayoutDesc& desc)
    {
        return CreateDX12PipelineLayout(this, desc);
    }

    RHIPipelineRef DX12Device::CreateGraphicsPipeline(const RHIGraphicsPipelineDesc& desc)
    {
        return CreateDX12GraphicsPipeline(this, desc);
    }

    RHIPipelineRef DX12Device::CreateComputePipeline(const RHIComputePipelineDesc& desc)
    {
        return CreateDX12ComputePipeline(this, desc);
    }

    RHIDescriptorSetRef DX12Device::CreateDescriptorSet(const RHIDescriptorSetDesc& desc)
    {
        return CreateDX12DescriptorSet(this, desc);
    }

    // =============================================================================
    // Command Context
    // =============================================================================
    RHICommandContextRef DX12Device::CreateCommandContext(RHICommandQueueType type)
    {
        return CreateDX12CommandContext(this, type);
    }

    void DX12Device::SubmitCommandContext(RHICommandContext* context, RHIFence* signalFence)
    {
        SubmitDX12CommandContext(this, context, signalFence);
    }

    void DX12Device::SubmitCommandContexts(std::span<RHICommandContext* const> contexts, RHIFence* signalFence)
    {
        SubmitDX12CommandContexts(this, contexts, signalFence);
    }

    // =============================================================================
    // SwapChain
    // =============================================================================
    RHISwapChainRef DX12Device::CreateSwapChain(const RHISwapChainDesc& desc)
    {
        return CreateDX12SwapChain(this, desc);
    }

    // =============================================================================
    // Synchronization
    // =============================================================================
    RHIFenceRef DX12Device::CreateFence(uint64 initialValue)
    {
        return CreateDX12Fence(this, initialValue);
    }

    void DX12Device::WaitForFence(RHIFence* fence, uint64 value)
    {
        WaitForDX12Fence(this, fence, value);
    }

} // namespace RVX
