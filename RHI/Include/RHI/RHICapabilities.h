#pragma once

#include "RHI/RHIDefinitions.h"
#include <string>

namespace RVX
{
    // =============================================================================
    // DX11 Threading Mode
    // =============================================================================
    enum class DX11ThreadingMode : uint8
    {
        SingleThreaded,   // Always single-threaded (safest)
        DeferredContext,  // Use Deferred Context for multi-threading
        Adaptive,         // Auto-select based on DrawCall count
    };

    // =============================================================================
    // Device Capabilities
    // =============================================================================
    struct RHICapabilities
    {
        RHIBackendType backendType = RHIBackendType::None;
        std::string adapterName;
        std::string driverVersion;

        // Basic limits
        uint32 maxTextureSize2D = 16384;
        uint32 maxTextureSize3D = 2048;
        uint32 maxTextureSizeCube = 16384;
        uint32 maxTextureArrayLayers = 2048;
        uint32 maxColorAttachments = 8;
        uint32 maxComputeWorkGroupSizeX = 1024;
        uint32 maxComputeWorkGroupSizeY = 1024;
        uint32 maxComputeWorkGroupSizeZ = 64;
        uint32 maxComputeWorkGroupCount = 65535;

        // Bindless support
        bool supportsBindless = false;
        uint32 maxBindlessTextures = 0;
        uint32 maxBindlessBuffers = 0;
        uint32 maxBindlessSamplers = 0;

        // Advanced features
        bool supportsRaytracing = false;
        bool supportsMeshShaders = false;
        bool supportsVariableRateShading = false;
        bool supportsAsyncCompute = false;
        bool supportsConservativeRasterization = false;

        // DX11-specific
        struct DX11Specific
        {
            DX11ThreadingMode threadingMode = DX11ThreadingMode::Adaptive;
            uint32 minDrawCallsForMultithread = 500;
            bool supportsDeferredContext = false;
            uint32 featureLevel = 0;  // e.g., 0xB000 for 11.0
        } dx11;

        // DX12-specific
        struct DX12Specific
        {
            uint32 resourceBindingTier = 0;  // 1, 2, or 3
            bool supportsRootSignature1_1 = false;
            bool supportsSM6_0 = false;
            bool supportsSM6_6 = false;
        } dx12;

        // Vulkan-specific
        struct VulkanSpecific
        {
            bool supportsDescriptorIndexing = false;
            bool supportsBufferDeviceAddress = false;
            uint32 maxPushConstantSize = 128;
            uint32 apiVersion = 0;  // VK_MAKE_VERSION
        } vulkan;
    };

} // namespace RVX
