#include "Render/RenderSystem.h"

namespace RVX
{
    void RenderSystem::Initialize(IRHIDevice* device, RHISwapChain* swapChain)
    {
        m_device = device;
        m_swapChain = swapChain;
    }

    void RenderSystem::BeginFrame()
    {
        if (m_device)
            m_device->BeginFrame();
    }

    void RenderSystem::Render(const Camera& camera)
    {
        (void)camera;
        // MVP: RenderGraph build/execute由上层负责，RenderSystem 暂不生成 Pass
    }

    void RenderSystem::EndFrame()
    {
        if (m_device)
            m_device->EndFrame();
    }
} // namespace RVX
