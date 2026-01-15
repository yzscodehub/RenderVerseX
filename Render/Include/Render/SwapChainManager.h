#pragma once

#include "RHI/RHI.h"

namespace RVX
{
    class SwapChainManager
    {
    public:
        void Initialize(IRHIDevice* device, RHISwapChain* swapChain);
        void Resize(uint32 width, uint32 height);

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
    };
} // namespace RVX
