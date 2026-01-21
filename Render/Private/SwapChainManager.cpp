#include "Render/SwapChainManager.h"

namespace RVX
{

void SwapChainManager::Initialize(IRHIDevice* device, RHISwapChain* swapChain)
{
    m_device = device;
    m_swapChain = swapChain;
    m_ownsSwapChain = false;
}

void SwapChainManager::Initialize(IRHIDevice* device, void* windowHandle, uint32_t width, uint32_t height)
{
    m_device = device;
    
    if (m_device && windowHandle)
    {
        // TODO: Create swap chain from window handle
        // m_swapChain = m_device->CreateSwapChain(windowHandle, width, height);
        // m_ownsSwapChain = true;
    }
    
    (void)windowHandle;
    (void)width;
    (void)height;
}

void SwapChainManager::Resize(uint32_t width, uint32_t height)
{
    if (!m_swapChain || !m_device)
        return;
    m_device->WaitIdle();
    m_swapChain->Resize(width, height);
}

void SwapChainManager::Present()
{
    if (m_swapChain)
    {
        m_swapChain->Present();
    }
}

} // namespace RVX
