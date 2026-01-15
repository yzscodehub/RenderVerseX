#pragma once

#include "Camera/Camera.h"
#include "RHI/RHI.h"
#include "RenderGraph/RenderGraph.h"

namespace RVX
{
    class RenderSystem
    {
    public:
        void Initialize(IRHIDevice* device, RHISwapChain* swapChain);
        void SetRenderGraph(RenderGraph* graph) { m_graph = graph; }

        void BeginFrame();
        void Render(const Camera& camera);
        void EndFrame();

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
        RenderGraph* m_graph = nullptr;
    };
} // namespace RVX
