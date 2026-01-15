#include "Render/SwapChainManager.h"

namespace RVX
{
    void SwapChainManager::Initialize(IRHIDevice* device, RHISwapChain* swapChain)
    {
        m_device = device;
        m_swapChain = swapChain;
    }

    void SwapChainManager::Resize(uint32 width, uint32 height)
    {
        if (!m_swapChain || !m_device)
            return;
        m_device->WaitIdle();
        m_swapChain->Resize(width, height);
    }
} // namespace RVX
