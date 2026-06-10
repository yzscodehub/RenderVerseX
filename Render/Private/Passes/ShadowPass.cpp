/**
 * @file ShadowPass.cpp
 * @brief ShadowPass implementation
 */

#include "Render/Passes/ShadowPass.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"
#include "Core/Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace RVX
{
namespace
{
    constexpr float RVX_SHADOW_EPSILON = 0.0001f;

    Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback)
    {
        const float valueLength = length(value);
        if (valueLength > RVX_SHADOW_EPSILON)
        {
            return value / valueLength;
        }
        return fallback;
    }

    bool IsFinitePositive(float value)
    {
        return std::isfinite(value) && value > RVX_SHADOW_EPSILON;
    }

    float RoundToTexel(float value, float texelSize)
    {
        if (!IsFinitePositive(texelSize))
        {
            return value;
        }
        return std::floor(value / texelSize + 0.5f) * texelSize;
    }

    struct FrustumSlice
    {
        std::array<Vec3, 8> corners{};
        Vec3 center{0.0f, 0.0f, 0.0f};
        float radius = 1.0f;
    };

    FrustumSlice BuildFrustumSlice(const ViewData& view, float nearDistance, float farDistance)
    {
        FrustumSlice slice;

        const float aspect = IsFinitePositive(view.aspectRatio) ? view.aspectRatio : 1.0f;
        const float fov = IsFinitePositive(view.fieldOfView) ? view.fieldOfView : 1.0472f;
        const float tanHalfFov = std::tan(fov * 0.5f);

        const Vec3 forward = NormalizeOr(view.cameraForward, Vec3(0.0f, 0.0f, -1.0f));
        Vec3 right = NormalizeOr(GetRightFromMatrix(view.inverseViewMatrix), Vec3(1.0f, 0.0f, 0.0f));
        Vec3 up = NormalizeOr(GetUpFromMatrix(view.inverseViewMatrix), Vec3(0.0f, 1.0f, 0.0f));

        if (std::abs(dot(right, forward)) > 0.95f)
        {
            right = NormalizeOr(cross(forward, Vec3(0.0f, 1.0f, 0.0f)), Vec3(1.0f, 0.0f, 0.0f));
        }
        if (std::abs(dot(up, forward)) > 0.95f)
        {
            up = NormalizeOr(cross(right, forward), Vec3(0.0f, 1.0f, 0.0f));
        }

        const float nearHeight = tanHalfFov * nearDistance;
        const float nearWidth = nearHeight * aspect;
        const float farHeight = tanHalfFov * farDistance;
        const float farWidth = farHeight * aspect;

        const Vec3 nearCenter = view.cameraPosition + forward * nearDistance;
        const Vec3 farCenter = view.cameraPosition + forward * farDistance;

        const auto writePlane = [&](size_t offset, const Vec3& center, float halfWidth, float halfHeight)
        {
            slice.corners[offset + 0] = center - right * halfWidth - up * halfHeight;
            slice.corners[offset + 1] = center + right * halfWidth - up * halfHeight;
            slice.corners[offset + 2] = center + right * halfWidth + up * halfHeight;
            slice.corners[offset + 3] = center - right * halfWidth + up * halfHeight;
        };

        writePlane(0, nearCenter, nearWidth, nearHeight);
        writePlane(4, farCenter, farWidth, farHeight);

        for (const Vec3& corner : slice.corners)
        {
            slice.center += corner;
        }
        slice.center /= static_cast<float>(slice.corners.size());

        for (const Vec3& corner : slice.corners)
        {
            slice.radius = std::max(slice.radius, length(corner - slice.center));
        }

        return slice;
    }

} // namespace

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
    m_cascades.resize(std::max(1u, config.numCascades));
}

void ShadowPass::SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity)
{
    m_lightDirection = direction;
    m_lightColor = color;
    m_lightIntensity = intensity;
    m_enabled = true;  // Enable shadow pass when light is configured
}

bool ShadowPass::IsSupported() const
{
    if (!m_pipelineCache)
    {
        m_unsupportedReason = "PipelineCache is not available";
        return false;
    }

    if (!m_pipelineCache->IsInitialized())
    {
        m_unsupportedReason = "PipelineCache is not initialized";
        return false;
    }

    if (!m_pipelineCache->GetDepthOnlyPipeline())
    {
        m_unsupportedReason = "Depth-only pipeline is not available";
        return false;
    }

    if (!m_gpuResources)
    {
        m_unsupportedReason = "GPUResourceManager is not available";
        return false;
    }

    if (m_config.numCascades == 0)
    {
        m_unsupportedReason = "ShadowPass requires at least one cascade";
        return false;
    }

    if (m_config.numCascades > RVX_MAX_DIRECTIONAL_SHADOW_CASCADES)
    {
        m_unsupportedReason = "ShadowPass exceeds the supported directional cascade count";
        return false;
    }

    if (m_config.shadowMapSize == 0)
    {
        m_unsupportedReason = "ShadowPass requires a non-zero shadow map size";
        return false;
    }

    m_unsupportedReason.clear();
    return true;
}

void ShadowPass::CalculateCascades(const ViewData& view)
{
    if (m_cascades.empty())
        return;

    const float nearClip = std::max(0.001f, view.nearPlane);
    const float farClip = std::max(nearClip + 1.0f, view.farPlane);
    const float range = farClip - nearClip;
    const float ratio = farClip / nearClip;
    const float lambda = clamp(m_config.cascadeSplitLambda, 0.0f, 1.0f);
    const Vec3 lightDir = NormalizeOr(m_lightDirection, Vec3(0.0f, -1.0f, 0.0f));
    const Vec3 worldUp(0.0f, 1.0f, 0.0f);
    const Vec3 lightUp = std::abs(dot(lightDir, worldUp)) > 0.95f ? Vec3(1.0f, 0.0f, 0.0f) : worldUp;
    const Vec3 lightRight = NormalizeOr(cross(lightDir, lightUp), Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 lightOrthoUp = NormalizeOr(cross(lightRight, lightDir), lightUp);

    float previousSplitDistance = nearClip;
    for (uint32_t i = 0; i < m_cascades.size(); ++i)
    {
        const float p = static_cast<float>(i + 1) / static_cast<float>(m_cascades.size());
        const float logSplit = nearClip * std::pow(ratio, p);
        const float uniformSplit = nearClip + range * p;
        const float splitDistance = lambda * logSplit + (1.0f - lambda) * uniformSplit;
        m_cascades[i].splitDepth = (splitDistance - nearClip) / range;

        const FrustumSlice slice = BuildFrustumSlice(view, previousSplitDistance, splitDistance);
        previousSplitDistance = splitDistance;

        Vec3 minLight(std::numeric_limits<float>::max());
        Vec3 maxLight(std::numeric_limits<float>::lowest());
        for (const Vec3& corner : slice.corners)
        {
            const Vec3 lightSpaceCorner(dot(lightRight, corner),
                                        dot(lightOrthoUp, corner),
                                        dot(-lightDir, corner));
            minLight = min(minLight, Vec3(lightSpaceCorner));
            maxLight = max(maxLight, Vec3(lightSpaceCorner));
        }

        const float padding = std::max(1.0f, slice.radius * 0.1f);
        const float fittedWidth = maxLight.x - minLight.x;
        const float fittedHeight = maxLight.y - minLight.y;
        const float stableExtent = std::max(1.0f, std::max(fittedWidth, fittedHeight) + padding * 2.0f);
        const float texelWorldSize = stableExtent / static_cast<float>(std::max(1u, m_config.shadowMapSize));
        Vec2 lightSpaceCenter((minLight.x + maxLight.x) * 0.5f,
                              (minLight.y + maxLight.y) * 0.5f);

        if (m_config.stabilizeCascades)
        {
            lightSpaceCenter.x = RoundToTexel(lightSpaceCenter.x, texelWorldSize);
            lightSpaceCenter.y = RoundToTexel(lightSpaceCenter.y, texelWorldSize);
        }

        const float halfExtent = stableExtent * 0.5f;
        const float lightEyeZ = maxLight.z + padding * 2.0f;
        const float localMinZ = minLight.z - lightEyeZ;
        const float localMaxZ = maxLight.z - lightEyeZ;
        const float zNear = std::max(0.001f, -localMaxZ - padding);
        const float zFar = std::max(zNear + 1.0f, -localMinZ + padding);
        const Vec3 lightPosition = lightRight * lightSpaceCenter.x +
                                   lightOrthoUp * lightSpaceCenter.y -
                                   lightDir * lightEyeZ;
        const Mat4 lightView = lookAt(lightPosition, lightPosition + lightDir, lightOrthoUp);
        const Mat4 lightProjection = ortho(-halfExtent,
                                           halfExtent,
                                           -halfExtent,
                                           halfExtent,
                                           zNear,
                                           zFar);
        m_cascades[i].viewProjection = lightProjection * lightView;
        m_cascades[i].lightSpaceCenter = lightSpaceCenter;
        m_cascades[i].stableExtent = stableExtent;
        m_cascades[i].texelWorldSize = texelWorldSize;
    }
}

void ShadowPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    if (!IsEnabled())
        return;

    m_stats = {};
    m_shadowMapTextureHandle = {};
    m_shadowMapTexture = nullptr;
    m_cascadeTextureHandles.clear();
    m_cascadeViews.clear();
    m_cascades.resize(std::max(1u, m_config.numCascades));

    if (!view.renderGraph)
    {
        m_unsupportedReason = "RenderGraph is not available during ShadowPass setup";
        return;
    }

    CalculateCascades(view);
    m_stats.configuredCascadeCount = static_cast<uint32_t>(m_cascades.size());

    const RHIFormat depthFormat = m_pipelineCache ? m_pipelineCache->GetConfig().depthStencilFormat
                                                  : PipelineCache::GetDefaultDepthStencilFormat();
    RHITextureDesc shadowDesc = RHITextureDesc::DepthStencil(m_config.shadowMapSize,
                                                             m_config.shadowMapSize,
                                                             depthFormat);
    shadowDesc.arraySize = std::max(RVX_MIN_DIRECTIONAL_SHADOW_ARRAY_LAYERS,
                                    static_cast<uint32>(m_cascades.size()));
    shadowDesc.debugName = "DirectionalShadowCascadeArray";

    m_shadowMapTextureHandle = view.renderGraph->CreateTexture(shadowDesc);
    view.renderGraph->SetExportState(m_shadowMapTextureHandle, RHIResourceState::ShaderResource);

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RGTextureHandle shadowLayer = m_shadowMapTextureHandle;
        shadowLayer.hasSubresourceRange = true;
        shadowLayer.subresourceRange = RHISubresourceRange{0, 1, i, 1, RHITextureAspect::Depth};
        builder.SetDepthStencil(shadowLayer, true, false);
        m_cascadeTextureHandles.push_back(shadowLayer);
    }

    m_stats.declaredCascadeResourceCount = static_cast<uint32_t>(m_cascadeTextureHandles.size());
}

void ShadowPass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    (void)view;

    if (!IsEnabled())
    {
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("ShadowPass: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

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

    if (!ResolveCascadeViews(view))
    {
        RVX_CORE_WARN("ShadowPass: cascade resources were not resolved; skipping shadow rendering");
        return;
    }

    // Render each cascade
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RenderCascade(ctx, view, i);
    }

    m_pipelineCache->UpdateViewConstants(view);
}

bool ShadowPass::ResolveCascadeViews(const ViewData& view)
{
    m_cascadeViews.assign(m_cascadeTextureHandles.size(), nullptr);
    m_stats.resolvedCascadeViewCount = 0;
    m_shadowMapTexture = nullptr;

    if (!view.renderGraph || !view.viewCache)
    {
        return false;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascadeTextureHandles.size()); ++i)
    {
        RHITexture* texture = view.renderGraph->GetTexture(m_cascadeTextureHandles[i]);
        if (!texture)
            continue;

        RHITextureViewDesc viewDesc;
        viewDesc.format = texture->GetFormat();
        viewDesc.dimension = texture->GetDimension();
        viewDesc.subresourceRange = RHISubresourceRange{0, 1, i, 1, RHITextureAspect::Depth};
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "ShadowCascadeLayerDSV";

        RHITextureView* viewHandle = view.viewCache->GetTextureView(texture, viewDesc);
        if (!viewHandle)
            continue;

        if (!m_shadowMapTexture)
        {
            m_shadowMapTexture = texture;
        }
        m_cascadeViews[i] = viewHandle;
        ++m_stats.resolvedCascadeViewCount;
    }

    return m_stats.resolvedCascadeViewCount == m_cascadeTextureHandles.size();
}

void ShadowPass::RenderCascade(RHICommandContext& ctx, const ViewData& view, uint32_t cascadeIndex)
{
    if (cascadeIndex >= m_cascadeViews.size() || !m_cascadeViews[cascadeIndex])
    {
        return;  // Cascade view not created
    }

    ViewData shadowView = view;
    shadowView.viewProjectionMatrix = m_cascades[cascadeIndex].viewProjection;
    shadowView.cameraForward = NormalizeOr(m_lightDirection, Vec3(0.0f, -1.0f, 0.0f));
    m_pipelineCache->UpdateViewConstants(shadowView);

    // Begin shadow render pass for this cascade
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(m_cascadeViews[cascadeIndex],
                           RHILoadOp::Clear, RHIStoreOp::Store, m_pipelineCache->GetDepthClearValue(), 0);

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

    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
    if (frameSet)
    {
        ctx.SetDescriptorSet(0, frameSet);
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

        ++m_stats.shadowCasterCount;

        // Update per-object constants
        if (m_pipelineCache)
        {
            m_pipelineCache->UpdateObjectConstants(obj.worldMatrix, obj.normalMatrix);
        }

        RHIDescriptorSet* objectSet = m_pipelineCache->GetObjectDescriptorSet();
        if (objectSet)
        {
            const auto objectDynamicOffsets = m_pipelineCache->GetCurrentObjectDynamicOffset();
            ctx.SetDescriptorSet(1, objectSet, objectDynamicOffsets);
        }

        // Bind vertex buffers
        ctx.SetVertexBuffer(0, buffers.positionBuffer);
        ctx.SetIndexBuffer(buffers.indexBuffer, RHIFormat::R32_UINT);

        // Draw
        for (const SubmeshGPUInfo& submesh : buffers.submeshes)
        {
            ctx.DrawIndexed(submesh.indexCount, 1, submesh.indexOffset, submesh.baseVertex, 0);
            ++m_stats.drawCount;
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
