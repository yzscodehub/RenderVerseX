#include "Render/Passes/GBufferPass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

namespace RVX
{
    void GBufferPass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
        RVX_CORE_INFO("GBufferPass: Initialized");
    }

    void GBufferPass::OnRemove()
    {
        m_device = nullptr;
    }

    void GBufferPass::SetResources(GPUResourceManager* gpuMgr, PipelineCache* pipelines)
    {
        m_gpuResources = gpuMgr;
        m_pipelineCache = pipelines;
    }

    void GBufferPass::SetRenderScene(const RenderScene* scene, const std::vector<uint32_t>* visibleIndices)
    {
        m_renderScene = scene;
        m_visibleIndices = visibleIndices;
    }

    void GBufferPass::SetRenderTargets(
        RHITextureView* albedoView,
        RHITextureView* normalView,
        RHITextureView* roughnessView,
        RHITextureView* depthView)
    {
        m_albedoView = albedoView;
        m_normalView = normalView;
        m_roughnessView = roughnessView;
        m_depthView = depthView;
    }

    void GBufferPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        // The deferred path is not part of the default frame pipeline yet.
        // Until SceneRenderer owns GBuffer render targets, declare only the external
        // targets provided by the current view so RenderGraph can still validate this pass.
        if (view.colorTarget.IsValid())
        {
            m_albedoHandle = builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
        }

        if (view.depthTarget.IsValid())
        {
            builder.SetDepthStencil(view.depthTarget, true, false);
            m_depthHandle = view.depthTarget;
        }
    }

    void GBufferPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;

        // Deferred rendering is intentionally not wired into the default renderer yet.
        // Keep this pass compile-safe and graph-valid until dedicated GBuffer targets
        // and pipelines are owned by SceneRenderer.
    }

} // namespace RVX
