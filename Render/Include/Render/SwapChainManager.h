#pragma once

#include "RHI/RHI.h"

namespace RVX
{
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

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
        bool m_ownsSwapChain = false;
    };
} // namespace RVX
