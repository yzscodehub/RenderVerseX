#pragma once

#include "RHI/RHIResources.h"

namespace RVX
{
    // =============================================================================
    // SwapChain Description
    // =============================================================================
    struct RHISwapChainDesc
    {
        void* windowHandle = nullptr;  // HWND on Windows
        uint32 width = 0;
        uint32 height = 0;
        RHIFormat format = RHIFormat::BGRA8_UNORM;
        uint32 bufferCount = 3;
        bool vsync = true;
        const char* debugName = nullptr;
    };

    // =============================================================================
    // SwapChain Interface
    // =============================================================================
    class RHISwapChain : public RHIResource
    {
    public:
        virtual ~RHISwapChain() = default;

        // Get current back buffer
        virtual RHITexture* GetCurrentBackBuffer() = 0;
        virtual RHITextureView* GetCurrentBackBufferView() = 0;
        virtual uint32 GetCurrentBackBufferIndex() const = 0;

        // Present and acquire next image
        virtual void Present() = 0;

        // Resize
        virtual void Resize(uint32 width, uint32 height) = 0;

        // Properties
        virtual uint32 GetWidth() const = 0;
        virtual uint32 GetHeight() const = 0;
        virtual RHIFormat GetFormat() const = 0;
        virtual uint32 GetBufferCount() const = 0;
    };

} // namespace RVX
