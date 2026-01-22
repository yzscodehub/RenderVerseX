/**
 * @file ShadowPass.cpp
 * @brief ShadowPass implementation
 */

#include "Render/Passes/ShadowPass.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"
#include <cmath>

namespace RVX
{

ShadowPass::ShadowPass()
{
    m_cascades.resize(4);  // Default 4 cascades
}

void ShadowPass::SetResources(GPUResourceManager* gpuResources, PipelineCache* pipelineCache)
{
    m_gpuResources = gpuResources;
    m_pipelineCache = pipelineCache;
}

void ShadowPass::SetRenderScene(const RenderScene* scene)
{
    m_renderScene = scene;
}

void ShadowPass::SetConfig(const ShadowPassConfig& config)
{
    m_config = config;
    m_cascades.resize(config.numCascades);
}

void ShadowPass::SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity)
{
    m_lightDirection = direction;
    m_lightColor = color;
    m_lightIntensity = intensity;
    m_enabled = true;  // Enable shadow pass when light is configured
}

void ShadowPass::CalculateCascades(const ViewData& view)
{
    if (m_cascades.empty())
        return;

    float nearClip = view.nearPlane;
    float farClip = view.farPlane;
    float range = farClip - nearClip;
    float ratio = farClip / nearClip;

    // Calculate cascade split depths using PSSM scheme
    for (uint32_t i = 0; i < m_cascades.size(); ++i)
    {
        float p = static_cast<float>(i + 1) / static_cast<float>(m_cascades.size());
        float logSplit = nearClip * std::pow(ratio, p);
        float uniformSplit = nearClip + range * p;
        float splitDepth = m_config.cascadeSplitLambda * logSplit + 
                          (1.0f - m_config.cascadeSplitLambda) * uniformSplit;
        m_cascades[i].splitDepth = (splitDepth - nearClip) / range;
    }

    // Calculate light view-projection matrix for each cascade
    // Note: This is a simplified implementation. A full implementation would:
    // 1. Calculate the frustum corners for each cascade split
    // 2. Transform corners to world space
    // 3. Fit an orthographic projection to encompass the frustum
    // 4. Stabilize the shadow map to prevent shimmer

    Vec3 lightDir = m_lightDirection;
    // Normalize
    float len = std::sqrt(lightDir.x * lightDir.x + lightDir.y * lightDir.y + lightDir.z * lightDir.z);
    if (len > 0.0f)
    {
        lightDir.x /= len;
        lightDir.y /= len;
        lightDir.z /= len;
    }

    // Simplified: Create a basic orthographic light projection centered on view
    for (uint32_t i = 0; i < m_cascades.size(); ++i)
    {
        float cascadeExtent = (i + 1) * 50.0f;  // Increase extent per cascade

        // Light view matrix (looking down light direction)
        Vec3 up = (std::abs(lightDir.y) < 0.99f) ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        
        // Simple orthographic projection for directional light
        // A full implementation would calculate proper bounds from frustum corners
        m_cascades[i].viewProjection = Mat4Identity();  // Placeholder
    }
}

void ShadowPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    // Shadow pass creates its own render targets (shadow maps)
    // These would be transient textures in a full RenderGraph implementation
    CalculateCascades(view);
}

void ShadowPass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    if (!m_pipelineCache || !m_renderScene || !m_gpuResources)
    {
        return;
    }

    // Get depth-only pipeline for shadow rendering
    RHIPipeline* shadowPipeline = m_pipelineCache->GetDepthOnlyPipeline();
    if (!shadowPipeline)
    {
        // Shadow pipeline not available yet
        return;
    }

    // Render each cascade
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RenderCascade(ctx, i);
    }
}

void ShadowPass::RenderCascade(RHICommandContext& ctx, uint32_t cascadeIndex)
{
    if (cascadeIndex >= m_cascadeViews.size() || !m_cascadeViews[cascadeIndex])
    {
        return;  // Cascade view not created
    }

    // Begin shadow render pass for this cascade
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(m_cascadeViews[cascadeIndex].Get(), 
                           RHILoadOp::Clear, RHIStoreOp::Store, 1.0f, 0);

    ctx.BeginRenderPass(rpDesc);

    // Set viewport for this cascade slice
    uint32_t size = m_config.shadowMapSize;
    RHIViewport viewport{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f};
    ctx.SetViewport(viewport);

    RHIRect scissor{0, 0, size, size};
    ctx.SetScissor(scissor);

    // Bind shadow pipeline
    RHIPipeline* pipeline = m_pipelineCache->GetDepthOnlyPipeline();
    if (pipeline)
    {
        ctx.SetPipeline(pipeline);
    }

    // Draw all shadow-casting objects
    for (size_t i = 0; i < m_renderScene->GetObjectCount(); ++i)
    {
        const RenderObject& obj = m_renderScene->GetObject(i);
        
        if (!obj.castsShadow)
            continue;

        MeshGPUBuffers buffers = m_gpuResources->GetMeshBuffers(obj.meshId);
        if (!buffers.IsValid())
            continue;

        // Update object constants with light-space matrix
        // In a full implementation: lightViewProj * worldMatrix
        if (m_pipelineCache)
        {
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix);
        }

        // Bind vertex buffers
        ctx.SetVertexBuffer(0, buffers.positionBuffer);
        ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

        // Draw
        for (const SubmeshGPUInfo& submesh : buffers.submeshes)
        {
            ctx.DrawIndexed(submesh.indexCount, 1, submesh.indexOffset, submesh.baseVertex, 0);
        }
    }

    ctx.EndRenderPass();
}

void ShadowPass::CreateShadowMap()
{
    // Shadow map creation would typically be done through RenderGraph
    // This is a placeholder for when direct resource creation is needed
}

} // namespace RVX
