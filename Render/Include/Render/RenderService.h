#pragma once

#include "Runtime/Camera/Camera.h"
#include "RHI/RHI.h"

// Forward declaration
namespace RVX { class RenderGraph; }

namespace RVX
{
    /// @brief Low-level render service for frame management (renamed from RenderSystem
    ///        to avoid confusion with Engine/Systems/RenderSystem which is an ISystem)
    class RenderService
    {
    public:
        void Initialize(IRHIDevice* device, RHISwapChain* swapChain);
        void SetRenderGraph(RenderGraph* graph) { m_graph = graph; }

        IRHIDevice* GetDevice() const { return m_device; }
        RHISwapChain* GetSwapChain() const { return m_swapChain; }
        RenderGraph* GetRenderGraph() const { return m_graph; }

        void BeginFrame();
        void Render(const Camera& camera);
        void EndFrame();

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
        RenderGraph* m_graph = nullptr;
    };
} // namespace RVX
