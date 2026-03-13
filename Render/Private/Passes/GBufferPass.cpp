#include "Render/Passes/GBufferPass.h"
#include "Render/SceneRenderer.h"
#include "Render/PipelineCache.h"
#include "Resource/GPUResourceManager.h"
#include "Scene/RenderScene.h"
#include "Core/Logger.h"

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
        // Get resolution from view
        uint32_t width = view.width;
        uint32_t height = view.height;

        // Create GBuffer render targets
        RHITextureDesc albedoDesc;
        albedoDesc.width = width;
        albedoDesc.height = height;
        albedoDesc.format = RHIFormat::RGBA8_UNORM;
        albedoDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        albedoDesc.debugName = "GBuffer_Albedo";
        m_albedoHandle = builder.CreateTexture(albedoDesc);

        RHITextureDesc normalDesc;
        normalDesc.width = width;
        normalDesc.height = height;
        normalDesc.format = RHIFormat::RGBA16_FLOAT;
        normalDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        normalDesc.debugName = "GBuffer_Normal";
        m_normalHandle = builder.CreateTexture(normalDesc);

        RHITextureDesc roughnessDesc;
        roughnessDesc.width = width;
        roughnessDesc.height = height;
        roughnessDesc.format = RHIFormat::RGBA16_FLOAT;
        roughnessDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        roughnessDesc.debugName = "GBuffer_Roughness";
        m_roughnessHandle = builder.CreateTexture(roughnessDesc);

        // Velocity (optional, for TAA)
        if (m_velocityEnabled)
        {
            RHITextureDesc velocityDesc;
            velocityDesc.width = width;
            velocityDesc.height = height;
            velocityDesc.format = RHIFormat::RG16_FLOAT;
            velocityDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            velocityDesc.debugName = "GBuffer_Velocity";
            m_velocityHandle = builder.CreateTexture(velocityDesc);
        }

        // Use existing depth or create new one
        if (view.depthTarget.IsValid())
        {
            m_depthHandle = view.depthTarget;
        }
        else
        {
            RHITextureDesc depthDesc;
            depthDesc.width = width;
            depthDesc.height = height;
            depthDesc.format = RHIFormat::D32_FLOAT;
            depthDesc.usage = RHITextureUsage::DepthStencil;
            depthDesc.debugName = "GBuffer_Depth";
            m_depthHandle = builder.CreateTexture(depthDesc);
        }

        // Declare write access
        builder.Write(m_albedoHandle, RHIResourceState::RenderTarget);
        builder.Write(m_normalHandle, RHIResourceState::RenderTarget);
        builder.Write(m_roughnessHandle, RHIResourceState::RenderTarget);
        if (m_velocityEnabled)
        {
            builder.Write(m_velocityHandle, RHIResourceState::RenderTarget);
        }
        builder.Write(m_depthHandle, RHIResourceState::DepthWrite);
    }

    void GBufferPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        // This is a simplified implementation
        // In a full implementation, this would:
        // 1. Set up the GBuffer pipeline
        // 2. Bind render targets
        // 3. Render all opaque meshes

        RVX_CORE_DEBUG("GBufferPass: Executing");

        // Set up render pass with multiple render targets
        RHIRenderPassDesc passDesc;
        passDesc.renderTargets.push_back({m_albedoHandle.get(), RHIResourceState::RenderTarget});
        passDesc.renderTargets.push_back({m_normalHandle.get(), RHIResourceState::RenderTarget});
        passDesc.renderTargets.push_back({m_roughnessHandle.get(), RHIResourceState::RenderTarget});
        if (m_velocityEnabled && m_velocityHandle.IsValid())
        {
            passDesc.renderTargets.push_back({m_velocityHandle.get(), RHIResourceState::RenderTarget});
        }
        passDesc.depthStencil = {m_depthHandle.get(), RHIResourceState::DepthWrite};

        ctx.BeginRenderPass(passDesc);

        // TODO: Set GBuffer pipeline and render opaque meshes
        // This requires:
        // 1. Loading GBuffer shaders (gbuffer_vertex.hlsl, gbuffer_pixel.hlsl)
        // 2. Creating/deferred pipeline state
        // 3. Iterating over visible meshes and rendering them

        // For now, just clear the targets
        ctx.ClearRenderTarget(0, {0.0f, 0.0f, 0.0f, 1.0f}); // Albedo - black
        ctx.ClearRenderTarget(1, {0.0f, 0.0f, 0.0f, 0.0f}); // Normal - (0,0,0) + no metallic
        ctx.ClearRenderTarget(2, {1.0f, 0.0f, 0.0f, 0.0f}); // Roughness - 1.0 (fully rough) + no metallic
        if (m_velocityEnabled && m_velocityHandle.IsValid())
        {
            ctx.ClearRenderTarget(3, {0.0f, 0.0f, 0.0f, 0.0f}); // Velocity - no motion
        }

        ctx.EndRenderPass();
    }

} // namespace RVX
