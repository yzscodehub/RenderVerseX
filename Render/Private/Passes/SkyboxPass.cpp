/**
 * @file SkyboxPass.cpp
 * @brief SkyboxPass implementation
 */

#include "Render/Passes/SkyboxPass.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cstring>

namespace RVX
{

namespace
{
    constexpr uint64 RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT = 256;

    uint64 AlignSkyboxConstantBufferSize(uint64 size)
    {
        return (size + RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    struct SkyboxGPUConstants
    {
        float zenithColor[4] = {0.4f, 0.6f, 1.0f, 1.0f};
        float horizonColor[4] = {0.8f, 0.85f, 0.9f, 1.0f};
        float groundColor[4] = {0.3f, 0.25f, 0.2f, 0.999f};
        float sunDirection[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        float sunColor[4] = {1.0f, 0.95f, 0.9f, 0.0f};
        float textureParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float cameraPosition[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Mat4 inverseViewProjection = Mat4Identity();
    };
} // namespace

SkyboxPass::SkyboxPass()
{
    // Skybox pass is enabled by default
}

void SkyboxPass::SetResources(PipelineCache* pipelineCache)
{
    m_pipelineCache = pipelineCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_retainedDescriptorSets.clear();
        m_retainedCubemapViews.clear();
        m_constantBuffer.Reset();
        m_fallbackCubemap.Reset();
        m_fallbackCubemapView.Reset();
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    RefreshSupport();
}

void SkyboxPass::SetRenderTargets(RHITextureView* colorTargetView, RHITextureView* depthTargetView)
{
    m_colorTargetView = colorTargetView;
    m_depthTargetView = depthTargetView;
}

void SkyboxPass::SetCubemap(RHITexture* cubemap, float exposure, float rotation, float blurLevel)
{
    m_cubemap = cubemap;
    m_exposure = exposure;
    m_rotation = rotation;
    m_blurLevel = blurLevel;
    if (!cubemap)
    {
        ClearSkybox("SkyboxCubemapMissing");
        return;
    }

    m_drawMode = SkyboxDrawMode::Cubemap;
    m_skySelected = true;
    RefreshSupport();
}

void SkyboxPass::SetProceduralSkyParams(const Vec3& sunDirection,
                                        const Vec3& skyColor,
                                        const Vec3& horizonColor,
                                        const Vec3& groundColor,
                                        const Vec3& sunColor,
                                        float exposure,
                                        float scatteringIntensity)
{
    m_sunDirection = sunDirection;
    m_skyColor = skyColor;
    m_horizonColor = horizonColor;
    m_groundColor = groundColor;
    m_sunColor = sunColor;
    m_exposure = exposure;
    m_scatteringIntensity = scatteringIntensity;
    m_rotation = 0.0f;
    m_blurLevel = 0.0f;
    m_cubemap = nullptr;
    m_drawMode = SkyboxDrawMode::Procedural;
    m_skySelected = true;
    RefreshSupport();
}

void SkyboxPass::SetSolidColor(const Vec3& color, float exposure)
{
    SetProceduralSkyParams(m_sunDirection, color, color, color, Vec3{0.0f, 0.0f, 0.0f}, exposure, 0.0f);
}

void SkyboxPass::ClearSkybox(const char* reason)
{
    m_skySelected = false;
    m_drawReady = false;
    m_cubemap = nullptr;
    m_drawMode = SkyboxDrawMode::None;
    m_unsupportedReason = reason ? reason : "No supported SkyboxComponent selected";
}

void SkyboxPass::RefreshSupport()
{
    m_drawReady = false;

    if (!m_enabled)
    {
        m_unsupportedReason = "Skybox pass is disabled";
        return;
    }

    if (!m_skySelected)
    {
        if (m_unsupportedReason.empty())
        {
            m_unsupportedReason = "No supported SkyboxComponent selected";
        }
        return;
    }

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        m_unsupportedReason = "Skybox requires an initialized PipelineCache";
        return;
    }

    if (!m_pipelineCache->GetSkyboxPipeline() ||
        !m_pipelineCache->GetSkyboxLayout() ||
        !m_pipelineCache->GetSkyboxSetLayout())
    {
        m_unsupportedReason = "Skybox pipeline resources are not available";
        return;
    }

    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_drawReady = true;
    m_unsupportedReason.clear();
}

void SkyboxPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    m_colorTargetHandle = {};
    m_depthTargetHandle = {};

    if (!IsEnabled())
    {
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("SkyboxPass: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

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
    if (!IsEnabled())
    {
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("SkyboxPass: unsupported execute skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    RHITextureView* colorTargetView = m_colorTargetView;
    RHITextureView* depthTargetView = m_depthTargetView;
    RHITexture* colorTarget = nullptr;
    if (view.renderGraph && view.viewCache && m_colorTargetHandle.IsValid())
    {
        colorTarget = view.renderGraph->GetTexture(m_colorTargetHandle);
        if (colorTarget)
        {
            colorTargetView = view.viewCache->GetDefaultRTV(colorTarget);
        }
    }

    if (view.renderGraph && view.viewCache && m_depthTargetHandle.IsValid())
    {
        if (RHITexture* depthTarget = view.renderGraph->GetTexture(m_depthTargetHandle))
        {
            depthTargetView = view.viewCache->GetDefaultDSV(depthTarget);
        }
    }

    if (!colorTargetView)
    {
        return;
    }

    if (!colorTarget)
    {
        colorTarget = colorTargetView->GetTexture();
    }

    const RHIFormat outputFormat = colorTargetView->GetFormat();
    RHIPipeline* pipeline = m_pipelineCache ? m_pipelineCache->GetSkyboxPipeline(outputFormat, depthTargetView != nullptr) : nullptr;
    RHIDescriptorSetLayout* setLayout = m_pipelineCache ? m_pipelineCache->GetSkyboxSetLayout() : nullptr;
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!pipeline || !setLayout || !device)
    {
        RVX_CORE_WARN("SkyboxPass: fullscreen pipeline resources are unavailable");
        return;
    }

    if (!EnsureRuntimeResources() || !UpdateConstants(view))
    {
        return;
    }

    RHITextureView* cubemapView = ResolveCubemapView(view);
    if (!cubemapView || !m_sampler)
    {
        RVX_CORE_WARN("SkyboxPass: texture descriptor resources are unavailable");
        return;
    }

    RHIDescriptorSetDesc descriptorDesc;
    descriptorDesc.layout = setLayout;
    descriptorDesc.debugName = "SkyboxDescriptorSet";
    descriptorDesc.BindBuffer(0,
                              m_constantBuffer.Get(),
                              0,
                              AlignSkyboxConstantBufferSize(sizeof(SkyboxGPUConstants)));
    descriptorDesc.BindTexture(1, cubemapView);
    descriptorDesc.BindSampler(2, m_sampler.Get());

    RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
    if (!descriptorSet)
    {
        RVX_CORE_WARN("SkyboxPass: failed to create descriptor set");
        return;
    }

    m_retainedDescriptorSets.push_back(descriptorSet);
    while (m_retainedDescriptorSets.size() > RVX_MAX_FRAME_COUNT + 1)
    {
        m_retainedDescriptorSets.pop_front();
    }

    // Begin render pass (load existing color, use existing depth)
    RHIRenderPassDesc rpDesc;
    rpDesc.AddColorAttachment(colorTargetView, RHILoadOp::Load, RHIStoreOp::Store,
                              {0.0f, 0.0f, 0.0f, 0.0f});

    if (depthTargetView)
    {
        // Depth test enabled, write disabled
        rpDesc.SetDepthStencil(depthTargetView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
    }

    if (colorTarget)
    {
        rpDesc.SetRenderArea(0, 0, colorTarget->GetWidth(), colorTarget->GetHeight());
    }

    ctx.BeginRenderPass(rpDesc);
    ctx.SetPipeline(pipeline);
    ctx.SetDescriptorSet(0, descriptorSet.Get());
    ctx.SetViewport(view.GetRHIViewport());
    ctx.SetScissor(view.GetRHIScissor());
    ctx.Draw(3, 1, 0, 0);
    ctx.EndRenderPass();
}

bool SkyboxPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        m_unsupportedReason = "Skybox requires an RHI device";
        return false;
    }

    if (!m_constantBuffer)
    {
        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignSkyboxConstantBufferSize(sizeof(SkyboxGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "SkyboxConstants";

        m_constantBuffer = device->CreateBuffer(bufferDesc);
        if (!m_constantBuffer)
        {
            m_unsupportedReason = "Skybox constant buffer creation failed";
            return false;
        }
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearWrap();
        samplerDesc.debugName = "SkyboxLinearWrapSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            m_unsupportedReason = "Skybox sampler creation failed";
            return false;
        }
    }

    if (!m_fallbackCubemap)
    {
        RHITextureDesc fallbackDesc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
        fallbackDesc.dimension = RHITextureDimension::TextureCube;
        fallbackDesc.arraySize = 1;
        fallbackDesc.usage = RHITextureUsage::ShaderResource;
        fallbackDesc.debugName = "SkyboxFallbackCubemap";
        m_fallbackCubemap = device->CreateTexture(fallbackDesc);
        if (!m_fallbackCubemap)
        {
            m_unsupportedReason = "Skybox fallback cubemap creation failed";
            return false;
        }
    }

    if (!m_fallbackCubemapView)
    {
        RHITextureViewDesc fallbackViewDesc;
        fallbackViewDesc.format = m_fallbackCubemap->GetFormat();
        fallbackViewDesc.dimension = RHITextureDimension::TextureCube;
        fallbackViewDesc.subresourceRange = RHISubresourceRange::All();
        fallbackViewDesc.debugName = "SkyboxFallbackCubemapSRV";
        m_fallbackCubemapView = device->CreateTextureView(m_fallbackCubemap.Get(), fallbackViewDesc);
        if (!m_fallbackCubemapView)
        {
            m_unsupportedReason = "Skybox fallback cubemap view creation failed";
            return false;
        }
    }

    return true;
}

RHITextureView* SkyboxPass::ResolveCubemapView(const ViewData& view)
{
    if (m_drawMode != SkyboxDrawMode::Cubemap)
    {
        return m_fallbackCubemapView.Get();
    }

    if (!m_cubemap)
        return nullptr;

    if (view.viewCache)
    {
        return view.viewCache->GetDefaultSRV(m_cubemap);
    }

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
        return nullptr;

    RHITextureViewDesc viewDesc;
    viewDesc.format = m_cubemap->GetFormat();
    viewDesc.dimension = RHITextureDimension::TextureCube;
    viewDesc.subresourceRange = RHISubresourceRange::All();
    viewDesc.debugName = "SkyboxCubemapSRV";
    RHITextureViewRef viewRef = device->CreateTextureView(m_cubemap, viewDesc);
    if (!viewRef)
        return nullptr;

    RHITextureView* result = viewRef.Get();
    m_retainedCubemapViews.push_back(std::move(viewRef));
    while (m_retainedCubemapViews.size() > RVX_MAX_FRAME_COUNT + 1)
    {
        m_retainedCubemapViews.pop_front();
    }
    return result;
}

bool SkyboxPass::UpdateConstants(const ViewData& view)
{
    if (!m_constantBuffer)
        return false;

    const bool reverseZ = m_pipelineCache && m_pipelineCache->GetConfig().reverseZ;
    const float farDepth = reverseZ ? 0.001f : 0.999f;
    const float exposure = std::max(0.0f, m_exposure);

    SkyboxGPUConstants constants;
    constants.zenithColor[0] = m_skyColor.x;
    constants.zenithColor[1] = m_skyColor.y;
    constants.zenithColor[2] = m_skyColor.z;
    constants.zenithColor[3] = exposure;
    constants.horizonColor[0] = m_horizonColor.x;
    constants.horizonColor[1] = m_horizonColor.y;
    constants.horizonColor[2] = m_horizonColor.z;
    constants.horizonColor[3] = std::max(0.0f, m_scatteringIntensity);
    constants.groundColor[0] = m_groundColor.x;
    constants.groundColor[1] = m_groundColor.y;
    constants.groundColor[2] = m_groundColor.z;
    constants.groundColor[3] = farDepth;
    constants.sunDirection[0] = m_sunDirection.x;
    constants.sunDirection[1] = m_sunDirection.y;
    constants.sunDirection[2] = m_sunDirection.z;
    constants.sunDirection[3] = m_drawMode == SkyboxDrawMode::Procedural ? 1.0f : 0.0f;
    constants.sunColor[0] = m_sunColor.x;
    constants.sunColor[1] = m_sunColor.y;
    constants.sunColor[2] = m_sunColor.z;
    constants.textureParams[0] = m_drawMode == SkyboxDrawMode::Cubemap ? 1.0f : 0.0f;
    constants.textureParams[1] = std::max(0.0f, m_blurLevel);
    constants.textureParams[2] = m_rotation;
    constants.cameraPosition[0] = view.cameraPosition.x;
    constants.cameraPosition[1] = view.cameraPosition.y;
    constants.cameraPosition[2] = view.cameraPosition.z;
    constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;

    void* mapped = m_constantBuffer->Map();
    if (!mapped)
    {
        RVX_CORE_WARN("SkyboxPass: failed to map constants buffer");
        return false;
    }

    std::memcpy(mapped, &constants, sizeof(constants));
    m_constantBuffer->Unmap();
    return true;
}

} // namespace RVX
