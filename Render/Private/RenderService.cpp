#include "Render/RenderService.h"

namespace RVX
{
    void RenderService::Initialize(IRHIDevice* device, RHISwapChain* swapChain)
    {
        m_device = device;
        m_swapChain = swapChain;
    }

    void RenderService::BeginFrame()
    {
        if (m_device)
            m_device->BeginFrame();
    }

    void RenderService::Render(const Camera& camera)
    {
        (void)camera;
        // MVP: RenderGraph build/execute managed by upper layer
    }

    void RenderService::EndFrame()
    {
        if (m_device)
            m_device->EndFrame();
    }
} // namespace RVX
