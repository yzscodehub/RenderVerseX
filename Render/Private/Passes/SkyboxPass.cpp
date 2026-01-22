/**
 * @file SkyboxPass.cpp
 * @brief SkyboxPass implementation
 */

#include "Render/Passes/SkyboxPass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"

namespace RVX
{

SkyboxPass::SkyboxPass()
{
    // Skybox pass is enabled by default
}

void SkyboxPass::SetResources(PipelineCache* pipelineCache)
{
    m_pipelineCache = pipelineCache;
}

void SkyboxPass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void SkyboxPass::SetCubemap(RHITexture* cubemap)
{
    m_cubemap = cubemap;
    m_useProceduralSky = (cubemap == nullptr);
}

void SkyboxPass::SetProceduralSkyParams(const Vec3& sunDirection, const Vec3& skyColor, const Vec3& horizonColor)
{
    m_sunDirection = sunDirection;
    m_skyColor = skyColor;
    m_horizonColor = horizonColor;
}

void SkyboxPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    // Write to color target
    if (view.colorTarget.IsValid())
    {
        m_colorTargetHandle = builder.Write(view.colorTarget, RHIResourceState::RenderTarget);
    }

    // Read depth (skybox uses depth test to skip pixels covered by geometry)
    if (view.depthTarget.IsValid())
    {
        builder.SetDepthStencil(view.depthTarget, false, false);  // Read-only depth
        m_depthTargetHandle = view.depthTarget;
    }
}

void SkyboxPass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    if (!m_colorTargetView)
    {
        return;
    }

    // Begin render pass (load existing color, use existing depth)
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(m_colorTargetView, RHILoadOp::Load, RHIStoreOp::Store,
                              {0.0f, 0.0f, 0.0f, 0.0f});

    if (m_depthTargetView)
    {
        // Depth test enabled, write disabled
        rpDesc.SetDepthStencil(m_depthTargetView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
    }

    ctx.BeginRenderPass(rpDesc);

    // Set viewport and scissor
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());

    // TODO: Bind skybox pipeline and draw fullscreen quad
    // For now, just clear to a gradient sky color as placeholder
    // A proper implementation would:
    // 1. Bind skybox shader (vertex: fullscreen triangle, pixel: sample cubemap or compute procedural color)
    // 2. Bind cubemap texture or procedural sky parameters
    // 3. Draw fullscreen triangle

    // Placeholder: The skybox effect will be visible once we have:
    // - SkyboxPipeline in PipelineCache
    // - Skybox.hlsl shader

    ctx.EndRenderPass();
}

} // namespace RVX
