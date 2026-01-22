#include "DX12Device.h"
#include "DX12Resources.h"
#include "DX12SwapChain.h"
#include "DX12CommandContext.h"
#include "DX12Pipeline.h"
#include "DX12Query.h"
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

    bool IsDX12Available()
    {
        // Try to create a minimal DXGI factory to check DX12 availability
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        {
            return false;
        }

        // Try to find a DX12-capable adapter
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

            // Check if adapter supports D3D12
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, 
                __uuidof(ID3D12Device), nullptr)))
            {
                return true;
            }

            adapter.Reset();
        }

        return false;
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
            // Enable DRED for better crash diagnostics (must be before device creation)
            EnableDRED();
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

        // Initialize pipeline cache (PSO disk caching)
        m_pipelineCache.Initialize(this, "Cache/PipelineCache.bin");

        // Initialize command allocator pool
        m_allocatorPool.Initialize(this);

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

        // Shutdown pipeline cache (saves to disk)
        m_pipelineCache.Shutdown();

        // Shutdown allocator pool
        m_allocatorPool.Shutdown();

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
    // Device Lost Handling
    // =============================================================================
    HRESULT DX12Device::GetDeviceRemovedReason() const
    {
        if (m_device)
        {
            return m_device->GetDeviceRemovedReason();
        }
        return S_OK;
    }

    void DX12Device::HandleDeviceLost(HRESULT reason)
    {
        // Prevent multiple notifications
        bool expected = false;
        if (!m_deviceLost.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        {
            return; // Already handled
        }

        RVX_RHI_ERROR("=== Device Lost Detected ===");
        RVX_RHI_ERROR("Reason HRESULT: 0x{:08X}", static_cast<uint32>(reason));

        // Get detailed reason if available
        if (m_device)
        {
            HRESULT removedReason = m_device->GetDeviceRemovedReason();
            RVX_RHI_ERROR("Device Removed Reason: 0x{:08X}", static_cast<uint32>(removedReason));

            switch (removedReason)
            {
                case DXGI_ERROR_DEVICE_HUNG:
                    RVX_RHI_ERROR("  -> DXGI_ERROR_DEVICE_HUNG: GPU took too long to execute commands");
                    break;
                case DXGI_ERROR_DEVICE_REMOVED:
                    RVX_RHI_ERROR("  -> DXGI_ERROR_DEVICE_REMOVED: GPU was physically removed or driver update");
                    break;
                case DXGI_ERROR_DEVICE_RESET:
                    RVX_RHI_ERROR("  -> DXGI_ERROR_DEVICE_RESET: GPU reset due to bad commands");
                    break;
                case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
                    RVX_RHI_ERROR("  -> DXGI_ERROR_DRIVER_INTERNAL_ERROR: Driver encountered internal error");
                    break;
                case DXGI_ERROR_INVALID_CALL:
                    RVX_RHI_ERROR("  -> DXGI_ERROR_INVALID_CALL: Invalid API usage");
                    break;
                case S_OK:
                    RVX_RHI_ERROR("  -> S_OK: Device is still valid (unexpected)");
                    break;
                default:
                    RVX_RHI_ERROR("  -> Unknown reason code");
                    break;
            }
        }

        // Log DRED info if available
        LogDREDInfo();

        // Invoke user callback
        if (m_deviceLostCallback)
        {
            m_deviceLostCallback(reason);
        }
    }

    void DX12Device::EnableDRED()
    {
        // DRED (Device Removed Extended Data) provides detailed GPU crash information
        // Requires Windows 10 1903+ and appropriate SDK
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
        {
            // Enable auto-breadcrumbs to track GPU progress
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            // Enable page fault reporting
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            RVX_RHI_INFO("DRED (Device Removed Extended Data) enabled");
        }
        else
        {
            RVX_RHI_DEBUG("DRED not available (requires Windows 10 1903+)");
        }
    }

    void DX12Device::LogDREDInfo()
    {
        if (!m_device)
            return;

        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&dred))))
        {
            RVX_RHI_DEBUG("DRED interface not available");
            return;
        }

        // Get auto-breadcrumb data
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
        {
            RVX_RHI_ERROR("=== DRED Auto-Breadcrumbs ===");
            const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
            while (node)
            {
                if (node->pCommandListDebugNameW)
                {
                    char name[256];
                    WideCharToMultiByte(CP_UTF8, 0, node->pCommandListDebugNameW, -1, name, sizeof(name), nullptr, nullptr);
                    RVX_RHI_ERROR("  CommandList: {}", name);
                }

                if (node->pCommandQueueDebugNameW)
                {
                    char name[256];
                    WideCharToMultiByte(CP_UTF8, 0, node->pCommandQueueDebugNameW, -1, name, sizeof(name), nullptr, nullptr);
                    RVX_RHI_ERROR("  CommandQueue: {}", name);
                }

                // Log breadcrumb operations
                if (node->pLastBreadcrumbValue && node->pCommandHistory && node->BreadcrumbCount > 0)
                {
                    uint32 lastCompleted = *node->pLastBreadcrumbValue;
                    RVX_RHI_ERROR("  Last completed breadcrumb: {}/{}", lastCompleted, node->BreadcrumbCount);
                    
                    // Log operations around the crash point
                    uint32 start = (lastCompleted > 3) ? lastCompleted - 3 : 0;
                    uint32 end = std::min(lastCompleted + 3, node->BreadcrumbCount);
                    for (uint32 i = start; i < end; ++i)
                    {
                        const char* opName = "Unknown";
                        switch (node->pCommandHistory[i])
                        {
                            case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: opName = "SetMarker"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: opName = "BeginEvent"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: opName = "EndEvent"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: opName = "DrawInstanced"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: opName = "DrawIndexedInstanced"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: opName = "Dispatch"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: opName = "CopyBufferRegion"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: opName = "CopyTextureRegion"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: opName = "ResolveSubresource"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW: opName = "ClearRTV"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW: opName = "ClearDSV"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: opName = "ResourceBarrier"; break;
                            case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: opName = "ExecuteIndirect"; break;
                            default: break;
                        }
                        const char* marker = (i == lastCompleted) ? " <-- LAST COMPLETED" : "";
                        RVX_RHI_ERROR("    [{}] {}{}", i, opName, marker);
                    }
                }

                node = node->pNext;
            }
        }

        // Get page fault data
        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
        {
            if (pageFault.PageFaultVA != 0)
            {
                RVX_RHI_ERROR("=== DRED Page Fault ===");
                RVX_RHI_ERROR("  Faulting VA: 0x{:016X}", pageFault.PageFaultVA);

                // Log allocations that existed at the faulting address
                const D3D12_DRED_ALLOCATION_NODE* allocNode = pageFault.pHeadExistingAllocationNode;
                if (allocNode)
                {
                    RVX_RHI_ERROR("  Existing allocations at address:");
                    while (allocNode)
                    {
                        if (allocNode->ObjectNameW)
                        {
                            char name[256];
                            WideCharToMultiByte(CP_UTF8, 0, allocNode->ObjectNameW, -1, name, sizeof(name), nullptr, nullptr);
                            RVX_RHI_ERROR("    - {}", name);
                        }
                        allocNode = allocNode->pNext;
                    }
                }

                // Log recently freed allocations
                allocNode = pageFault.pHeadRecentFreedAllocationNode;
                if (allocNode)
                {
                    RVX_RHI_ERROR("  Recently freed allocations:");
                    while (allocNode)
                    {
                        if (allocNode->ObjectNameW)
                        {
                            char name[256];
                            WideCharToMultiByte(CP_UTF8, 0, allocNode->ObjectNameW, -1, name, sizeof(name), nullptr, nullptr);
                            RVX_RHI_ERROR("    - {}", name);
                        }
                        allocNode = allocNode->pNext;
                    }
                }
            }
        }
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
    // Root Signature Cache
    // =============================================================================
    ComPtr<ID3D12RootSignature> DX12Device::GetOrCreateRootSignature(
        const RootSignatureCacheKey& key,
        const std::function<ComPtr<ID3D12RootSignature>()>& createFunc)
    {
        std::lock_guard<std::mutex> lock(m_rootSignatureCacheMutex);
        
        auto it = m_rootSignatureCache.find(key);
        if (it != m_rootSignatureCache.end())
        {
            RVX_RHI_DEBUG("Root Signature cache hit");
            return it->second;
        }
        
        // Create new root signature
        RVX_RHI_DEBUG("Root Signature cache miss, creating new");
        ComPtr<ID3D12RootSignature> rootSig = createFunc();
        if (rootSig)
        {
            m_rootSignatureCache[key] = rootSig;
        }
        return rootSig;
    }

    RootSignatureCacheKey DX12Device::BuildRootSignatureKey(const RHIPipelineLayoutDesc& desc)
    {
        RootSignatureCacheKey key;
        key.pushConstantSize = desc.pushConstantSize;
        key.setCount = static_cast<uint32>(desc.setLayouts.size());
        
        for (uint32 setIndex = 0; setIndex < desc.setLayouts.size(); ++setIndex)
        {
            auto* layout = desc.setLayouts[setIndex];
            if (!layout)
                continue;
            
            // Access the entries through the layout
            // We need to cast to DX12DescriptorSetLayout to get entries
            // For now, use a simple approach: store setIndex as marker
            key.bindings.push_back({setIndex, 0xFF});
        }
        
        return key;
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
        (void)enableGPUValidation;  // GPU validation is configured in EnableDebugLayer

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

    ID3D12CommandSignature* DX12Device::GetDrawCommandSignature()
    {
        if (!m_drawCommandSignature)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

            D3D12_COMMAND_SIGNATURE_DESC desc = {};
            desc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &arg;

            DX12_CHECK(m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_drawCommandSignature)));
        }

        return m_drawCommandSignature.Get();
    }

    ID3D12CommandSignature* DX12Device::GetDrawIndexedCommandSignature()
    {
        if (!m_drawIndexedCommandSignature)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;

            D3D12_COMMAND_SIGNATURE_DESC desc = {};
            desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &arg;

            DX12_CHECK(m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_drawIndexedCommandSignature)));
        }

        return m_drawIndexedCommandSignature.Get();
    }

    ID3D12CommandSignature* DX12Device::GetDispatchCommandSignature()
    {
        if (!m_dispatchCommandSignature)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

            D3D12_COMMAND_SIGNATURE_DESC desc = {};
            desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &arg;

            DX12_CHECK(m_device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_dispatchCommandSignature)));
        }

        return m_dispatchCommandSignature.Get();
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

        // Dynamic state and advanced features
        m_capabilities.supportsDepthBounds = true;              // DX12 supports depth bounds
        m_capabilities.supportsDynamicLineWidth = false;        // DX12 doesn't support dynamic line width
        m_capabilities.supportsSeparateStencilRef = false;      // DX12 doesn't support separate stencil refs
        m_capabilities.supportsSplitBarrier = true;             // DX12 supports split barriers
        m_capabilities.supportsSecondaryCommandBuffer = true;   // DX12 supports bundles
        m_capabilities.supportsMemoryBudgetQuery = true;        // DXGI supports memory budget
        m_capabilities.supportsPersistentMapping = true;        // DX12 supports persistent mapping

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

        // Recycle completed command allocators
        uint64 completedValue = m_frameFence->GetCompletedValue();
        m_allocatorPool.Tick(completedValue);

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
    // Memory Heap Management
    // =============================================================================
    RHIHeapRef DX12Device::CreateHeap(const RHIHeapDesc& desc)
    {
        return CreateDX12Heap(this, desc);
    }

    RHITextureRef DX12Device::CreatePlacedTexture(RHIHeap* heap, uint64 offset, const RHITextureDesc& desc)
    {
        return CreateDX12PlacedTexture(this, heap, offset, desc);
    }

    RHIBufferRef DX12Device::CreatePlacedBuffer(RHIHeap* heap, uint64 offset, const RHIBufferDesc& desc)
    {
        return CreateDX12PlacedBuffer(this, heap, offset, desc);
    }

    IRHIDevice::MemoryRequirements DX12Device::GetTextureMemoryRequirements(const RHITextureDesc& desc)
    {
        D3D12_RESOURCE_DESC resourceDesc = {};
        
        switch (desc.dimension)
        {
            case RHITextureDimension::Texture1D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture2D:
            case RHITextureDimension::TextureCube:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.arraySize);
                break;
            case RHITextureDimension::Texture3D:
                resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
                resourceDesc.DepthOrArraySize = static_cast<UINT16>(desc.depth);
                break;
        }
        
        resourceDesc.Width = desc.width;
        resourceDesc.Height = desc.height;
        resourceDesc.MipLevels = static_cast<UINT16>(desc.mipLevels);
        resourceDesc.Format = ToDXGIFormat(desc.format);
        resourceDesc.SampleDesc.Count = static_cast<UINT>(desc.sampleCount);
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        if (HasFlag(desc.usage, RHITextureUsage::RenderTarget))
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (HasFlag(desc.usage, RHITextureUsage::DepthStencil))
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (HasFlag(desc.usage, RHITextureUsage::UnorderedAccess))
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        
        return {allocInfo.SizeInBytes, allocInfo.Alignment};
    }

    IRHIDevice::MemoryRequirements DX12Device::GetBufferMemoryRequirements(const RHIBufferDesc& desc)
    {
        D3D12_RESOURCE_DESC resourceDesc = {};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = desc.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        if (HasFlag(desc.usage, RHIBufferUsage::UnorderedAccess))
            resourceDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        D3D12_RESOURCE_ALLOCATION_INFO allocInfo = m_device->GetResourceAllocationInfo(0, 1, &resourceDesc);
        
        return {allocInfo.SizeInBytes, allocInfo.Alignment};
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

    RHIQueryPoolRef DX12Device::CreateQueryPool(const RHIQueryPoolDesc& desc)
    {
        return CreateDX12QueryPool(this, desc);
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

    // =============================================================================
    // Upload Resources
    // =============================================================================
    RHIStagingBufferRef DX12Device::CreateStagingBuffer(const RHIStagingBufferDesc& desc)
    {
        // Create an upload buffer for staging
        RHIBufferDesc bufferDesc;
        bufferDesc.size = desc.size;
        bufferDesc.usage = RHIBufferUsage::CopySrc;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = desc.debugName;

        // TODO: Create proper DX12StagingBuffer wrapper
        // For now, return nullptr (not yet implemented)
        RVX_RHI_WARN("DX12: CreateStagingBuffer not yet fully implemented");
        return nullptr;
    }

    RHIRingBufferRef DX12Device::CreateRingBuffer(const RHIRingBufferDesc& desc)
    {
        // TODO: Create proper DX12RingBuffer implementation
        RVX_RHI_WARN("DX12: CreateRingBuffer not yet fully implemented");
        return nullptr;
    }

    // =============================================================================
    // Memory Statistics
    // =============================================================================
    RHIMemoryStats DX12Device::GetMemoryStats() const
    {
        RHIMemoryStats stats = {};

        // Query DXGI adapter memory info
        DXGI_QUERY_VIDEO_MEMORY_INFO localMemoryInfo = {};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonLocalMemoryInfo = {};

        ComPtr<IDXGIAdapter3> adapter3;
        if (m_adapter && SUCCEEDED(m_adapter.As(&adapter3)))
        {
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &localMemoryInfo);
            adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocalMemoryInfo);
        }

        stats.budgetBytes = localMemoryInfo.Budget;
        stats.currentUsageBytes = localMemoryInfo.CurrentUsage;
        stats.totalAllocated = localMemoryInfo.CurrentUsage + nonLocalMemoryInfo.CurrentUsage;
        stats.totalUsed = stats.totalAllocated;

        return stats;
    }

    // =============================================================================
    // Debug Resource Groups
    // =============================================================================
    void DX12Device::BeginResourceGroup(const char* name)
    {
        // PIX markers for resource grouping
        // Note: This is primarily useful during resource creation capture
        (void)name;
    }

    void DX12Device::EndResourceGroup()
    {
        // End PIX resource group
    }

} // namespace RVX
