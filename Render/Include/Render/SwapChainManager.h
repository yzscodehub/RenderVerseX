#pragma once

#include "RHI/RHI.h"

#include <string>

namespace RVX
{
    struct SwapChainManagerDiagnostics
    {
        bool requested = false;
        bool initialized = false;
        bool hasDevice = false;
        bool hasWindowHandle = false;
        bool hasSwapChain = false;
        bool ownsSwapChain = false;
        bool resizeRequested = false;
        bool presentRequested = false;
        uint32 width = 0;
        uint32 height = 0;
        std::string reason;
    };

    /**
     * @brief Manages swap chain lifecycle and presentation
     */
    class SwapChainManager
    {
    public:
        /// Initialize with existing swap chain
        void Initialize(IRHIDevice* device, RHISwapChain* swapChain);

        /// Initialize by creating swap chain from window handle
        void Initialize(IRHIDevice* device, void* windowHandle, uint32_t width, uint32_t height);

        /// Resize swap chain
        void Resize(uint32_t width, uint32_t height);

        /// Present the current frame
        void Present();

        /// Get the underlying swap chain
        RHISwapChain* GetSwapChain() const { return m_swapChain; }
        bool OwnsSwapChain() const { return m_ownsSwapChain; }
        const SwapChainManagerDiagnostics& GetLastDiagnostics() const { return m_lastDiagnostics; }

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
        bool m_ownsSwapChain = false;
        SwapChainManagerDiagnostics m_lastDiagnostics;
    };
} // namespace RVX
