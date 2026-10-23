#include "Render/SwapChainManager.h"

namespace RVX
{

namespace
{
    constexpr const char* RVX_SWAPCHAIN_WINDOW_HANDLE_UNSUPPORTED_REASON =
        "SwapChainManager cannot create swap chains from raw window handles; provide a platform-created RHISwapChain";
} // namespace

void SwapChainManager::Initialize(IRHIDevice* device, RHISwapChain* swapChain)
{
    m_device = device;
    m_swapChain = swapChain;
    m_ownsSwapChain = false;

    m_lastDiagnostics = {};
    m_lastDiagnostics.requested = swapChain != nullptr;
    m_lastDiagnostics.initialized = m_device != nullptr && m_swapChain != nullptr;
    m_lastDiagnostics.hasDevice = m_device != nullptr;
    m_lastDiagnostics.hasSwapChain = m_swapChain != nullptr;
    m_lastDiagnostics.ownsSwapChain = false;
    if (!m_lastDiagnostics.initialized)
    {
        m_lastDiagnostics.reason = "External swap chain initialization requires both an RHI device and swap chain";
    }
}

void SwapChainManager::Initialize(IRHIDevice* device, void* windowHandle, uint32_t width, uint32_t height)
{
    m_device = device;
    m_swapChain = nullptr;
    m_ownsSwapChain = false;

    m_lastDiagnostics = {};
    m_lastDiagnostics.requested = true;
    m_lastDiagnostics.initialized = false;
    m_lastDiagnostics.hasDevice = m_device != nullptr;
    m_lastDiagnostics.hasWindowHandle = windowHandle != nullptr;
    m_lastDiagnostics.hasSwapChain = false;
    m_lastDiagnostics.ownsSwapChain = false;
    m_lastDiagnostics.width = width;
    m_lastDiagnostics.height = height;

    if (!m_device)
    {
        m_lastDiagnostics.reason = "Swap chain creation skipped because no RHI device is available";
        return;
    }

    if (!windowHandle)
    {
        m_lastDiagnostics.reason = "Swap chain creation skipped because no window handle is available";
        return;
    }

    m_lastDiagnostics.reason = RVX_SWAPCHAIN_WINDOW_HANDLE_UNSUPPORTED_REASON;
}

void SwapChainManager::Resize(uint32_t width, uint32_t height)
{
    m_lastDiagnostics.resizeRequested = true;
    m_lastDiagnostics.width = width;
    m_lastDiagnostics.height = height;
    m_lastDiagnostics.hasDevice = m_device != nullptr;
    m_lastDiagnostics.hasSwapChain = m_swapChain != nullptr;
    m_lastDiagnostics.ownsSwapChain = m_ownsSwapChain;

    if (!m_swapChain || !m_device)
    {
        m_lastDiagnostics.initialized = false;
        m_lastDiagnostics.reason = "Swap chain resize skipped because the manager is not initialized";
        return;
    }

    m_device->WaitIdle();
    m_swapChain->Resize(width, height);
    m_lastDiagnostics.initialized = true;
    m_lastDiagnostics.reason.clear();
}

void SwapChainManager::Present()
{
    m_lastDiagnostics.presentRequested = true;
    m_lastDiagnostics.hasDevice = m_device != nullptr;
    m_lastDiagnostics.hasSwapChain = m_swapChain != nullptr;
    m_lastDiagnostics.ownsSwapChain = m_ownsSwapChain;

    if (!m_swapChain)
    {
        m_lastDiagnostics.initialized = false;
        m_lastDiagnostics.reason = "Swap chain present skipped because no swap chain is available";
        return;
    }

    m_swapChain->Present();
    m_lastDiagnostics.initialized = true;
    m_lastDiagnostics.reason.clear();
}

} // namespace RVX
