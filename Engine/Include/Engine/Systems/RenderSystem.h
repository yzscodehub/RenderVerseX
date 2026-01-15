#pragma once

#include "Core/ISystem.h"
#include "RHI/RHI.h"
#include "RenderGraph/RenderGraph.h"
#include <functional>
#include <vector>

namespace RVX
{
    class RenderSystem : public ISystem
    {
    public:
        using RenderCallback = std::function<void(
            RHICommandContext& ctx,
            RHISwapChain& swapChain,
            RenderGraph& graph,
            uint32 backBufferIndex)>;

        const char* GetName() const override { return "RenderSystem"; }

        void Initialize(IRHIDevice* device, RHISwapChain* swapChain, std::vector<RHICommandContextRef>* contexts)
        {
            m_device = device;
            m_swapChain = swapChain;
            m_contexts = contexts;
            m_graph.SetDevice(device);
            if (swapChain)
            {
                m_backBufferStates.assign(swapChain->GetBufferCount(), RHIResourceState::Undefined);
            }
        }

        void SetRenderCallback(RenderCallback callback) { m_renderCallback = std::move(callback); }

        void OnRender() override
        {
            if (!m_device || !m_swapChain || !m_contexts || m_contexts->empty())
                return;

            m_device->BeginFrame();
            uint32 backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
            const uint32 frameIndex = m_device->GetCurrentFrameIndex();
            if (frameIndex >= m_contexts->size())
            {
                m_device->EndFrame();
                return;
            }
            auto& cmdContext = (*m_contexts)[frameIndex];
            cmdContext->Begin();

            if (m_renderCallback)
            {
                m_renderCallback(*cmdContext, *m_swapChain, m_graph, backBufferIndex);
            }

            cmdContext->End();
            m_device->SubmitCommandContext(cmdContext.Get(), nullptr);
            m_swapChain->Present();
            m_device->EndFrame();
        }

        void ResetBackBufferStates()
        {
            if (m_swapChain)
            {
                m_backBufferStates.assign(m_swapChain->GetBufferCount(), RHIResourceState::Undefined);
            }
        }

        void HandleResize(uint32 width, uint32 height)
        {
            if (!m_device || !m_swapChain)
                return;
            // Release backbuffer references held by the graph before resizing.
            m_graph.Clear();
            m_device->WaitIdle();
            m_swapChain->Resize(width, height);
            ResetBackBufferStates();
        }

        std::vector<RHIResourceState>& GetBackBufferStates() { return m_backBufferStates; }

    private:
        IRHIDevice* m_device = nullptr;
        RHISwapChain* m_swapChain = nullptr;
        std::vector<RHICommandContextRef>* m_contexts = nullptr;
        RenderGraph m_graph;
        RenderCallback m_renderCallback;
        std::vector<RHIResourceState> m_backBufferStates;
    };
} // namespace RVX
