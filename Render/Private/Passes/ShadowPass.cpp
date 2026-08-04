/**
 * @file ShadowPass.cpp
 * @brief ShadowPass implementation
 */

#include "Render/Passes/ShadowPass.h"
#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace RVX
{
namespace
{
    constexpr float RVX_SHADOW_EPSILON = 0.0001f;

    std::span<const Mat4> ResolveSkinningMatrices(const RenderObject& object, const MeshGPUBuffers& buffers)
    {
        if (!object.HasSkinningData() || !buffers.HasSkinningVertexData())
        {
            return {};
        }

        return std::span<const Mat4>(object.skinningMatrices.data(), object.skinningMatrices.size());
    }

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

    ShadowDepthBiasState MakeShadowDepthBiasState(const ShadowPassConfig& config)
    {
        ShadowDepthBiasState biasState;
        biasState.constantBias = config.casterDepthBias;
        biasState.slopeScaledBias = config.casterSlopeScaledDepthBias;
        biasState.biasClamp = config.casterDepthBiasClamp;
        return biasState;
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

    DirectionalShadowRecordOutput MakeDirectionalShadowRecordOutput(
        const RenderPassRecordIdentity& identity,
        const ShadowPass& recorder)
    {
        DirectionalShadowRecordOutput output;
        output.identity = identity;

        const ShadowPassStats& stats = recorder.GetStats();
        const ShadowPassConfig& config = recorder.GetConfig();
        const std::vector<ShadowCascade>& cascades = recorder.GetCascades();
        const std::vector<RGTextureHandle>& cascadeHandles =
            recorder.GetCascadeTextureHandles();
        const RGTextureHandle shadowMap = recorder.GetShadowMapTextureHandle();
        const uint32 cascadeCount = static_cast<uint32>(cascades.size());
        const bool cascadeHandlesHaveCurrentProvenance = std::all_of(
            cascadeHandles.begin(),
            cascadeHandles.end(),
            [&identity](const RGTextureHandle& cascadeHandle)
            {
                return HasCurrentGraphProvenance(cascadeHandle, identity);
            });
        if (!shadowMap.IsValid() ||
            !HasCurrentGraphProvenance(shadowMap, identity) ||
            config.shadowMapSize == 0 ||
            cascadeCount == 0 ||
            cascadeCount != config.numCascades ||
            cascadeCount != stats.configuredCascadeCount ||
            cascadeCount != stats.declaredCascadeResourceCount ||
            cascadeCount != cascadeHandles.size() ||
            !cascadeHandlesHaveCurrentProvenance ||
            cascadeCount > RVX_MAX_DIRECTIONAL_SHADOW_CASCADES)
        {
            return output;
        }

        output.enabled = true;
        output.shadowMap = shadowMap;
        output.shadowMapSize = config.shadowMapSize;
        output.cascadeBlendRatio = config.cascadeBlendRatio;
        output.shadowBias = config.shadowBias;
        output.normalBias = config.normalBias;
        output.filterRadiusTexels = config.filterRadiusTexels;
        output.cascadeViewProjections.reserve(cascadeCount);
        output.cascadeSplitDepths.reserve(cascadeCount);
        for (const ShadowCascade& cascade : cascades)
        {
            output.cascadeViewProjections.push_back(cascade.viewProjection);
            output.cascadeSplitDepths.push_back(cascade.splitDepth);
        }
        if (!output.IsCompatibleWith(identity))
        {
            output = {};
            output.identity = identity;
        }
        return output;
    }

} // namespace

struct ShadowPass::PlannedShadowDraw
{
    MeshGPUBuffers buffers;
    ObjectConstantBinding objectBinding;
};

ShadowPass::ShadowPass()
{
    m_cascades.resize(4);  // Default 4 cascades
}

ShadowPass::~ShadowPass() = default;

void ShadowPass::SetResources(PipelineCache* pipelineCache)
{
    m_pipelineCache = pipelineCache;
}

void ShadowPass::InitializeGraphRecorder(const RenderScene* scene)
{
    m_renderScene = scene;
}

void ShadowPass::SetConfig(const ShadowPassConfig& config)
{
    m_config = config;
    m_cascades.resize(std::max(1u, config.numCascades));
}

void ShadowPass::Setup(RenderGraphBuilder&, const ViewData&)
{
    // ShadowPass accepts only the typed recording contract.  This retained
    // IRenderPass override makes a legacy base-class adapter a fail-closed
    // no-op instead of exposing a standalone recording path.
}

void ShadowPass::Execute(RHICommandContext&, const ViewData&)
{
    // See Setup(RenderGraphBuilder&, const ViewData&).
}

void ShadowPass::AddToGraph(
    RenderGraph& graph,
    const RenderPassRecordContext& context)
{
    struct GraphPassData
    {
        RenderPassExecutionData execution{};
        std::unique_ptr<ShadowPass> recorder;
        bool contextValid = false;
    };

    const bool sourcePlanValid = context.executionPlan != nullptr &&
        context.executionPlan->frameSequence == context.identity.frameSequence &&
        context.executionPlan->viewOrdinal == context.identity.viewOrdinal;
    const bool suppliedResultsValid = context.results != nullptr &&
        context.results->identity == context.identity &&
        (context.results->executionReport.frameSequence == 0 ||
         context.results->executionReport.frameSequence ==
             context.identity.frameSequence);
    const bool suppliedSnapshotValid = suppliedResultsValid &&
        context.frameSnapshot != nullptr &&
        context.frameSnapshot->identity == context.identity &&
        context.frameSnapshot->executionPlan.frameSequence ==
            context.identity.frameSequence &&
        context.frameSnapshot->executionPlan.viewOrdinal ==
            context.identity.viewOrdinal &&
        context.frameSnapshot->view.renderGraph == &graph &&
        context.frameSnapshot->view.renderFrameExecutionPlan ==
            &context.frameSnapshot->executionPlan &&
        context.frameSnapshot->view.meshPassPreparation ==
            &context.frameSnapshot->meshPassPreparation &&
        context.frameSnapshot->view.renderVisibility ==
            &context.frameSnapshot->visibility &&
        context.frameSnapshot->view.renderFrameExecutionReport ==
            &context.results->executionReport;
    const bool sourceContextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        sourcePlanValid && suppliedResultsValid && suppliedSnapshotValid &&
        context.view.renderGraph == &graph &&
        context.view.renderFrameExecutionPlan == context.executionPlan &&
        context.view.meshPassPreparation == context.meshPassPreparation &&
        context.view.renderVisibility == context.visibility &&
        context.view.renderFrameExecutionReport == context.executionReport;

    RenderPassExecutionData execution;
    if (sourceContextValid)
    {
        execution = MakeRenderPassExecutionData(context);
    }
    else
    {
        execution.view = context.view;
        execution.identity = context.identity;
    }
    const RenderFrameExecutionPlan* executionPlan =
        execution.GetExecutionPlan();
    const bool hasPlan = executionPlan != nullptr;
    const bool contextValid = sourceContextValid && hasPlan &&
        execution.MatchesTargetGraph(graph) &&
        execution.IsFrameIdentityValid() &&
        execution.frameSnapshot != nullptr && execution.results != nullptr;
    const bool resultOwnershipValid = sourceContextValid &&
        execution.identity.Matches(graph) && execution.results != nullptr &&
        execution.results->identity == execution.identity &&
        execution.frameSnapshot != nullptr &&
        execution.frameSnapshot->identity == execution.identity;

    const ShadowPassConfig config = m_config;
    const PrimaryDirectionalLightRecordInput primaryLight = execution.frameSnapshot
        ? execution.frameSnapshot->primaryDirectionalLight
        : PrimaryDirectionalLightRecordInput{};
    const bool requestedEnabled = m_enabled;
    PipelineCache* const pipelineCache = m_pipelineCache;
    const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
    const RenderScene* const renderScene = execution.frameSnapshot
        ? &execution.frameSnapshot->scene : nullptr;
    const std::shared_ptr<RenderPassRecordResults> results =
        resultOwnershipValid ? execution.results : nullptr;

    graph.AddPass<GraphPassData>(
        GetName(),
        GetPassType(),
        [execution,
         contextValid,
         config,
         primaryLight,
         requestedEnabled,
         pipelineCache,
         resourceRegistry,
         renderScene,
         results](RenderGraphBuilder& builder, GraphPassData& data)
        {
            data.execution = execution;
            data.contextValid = contextValid;
            if (!data.contextValid || !results)
            {
                return;
            }
            results->directionalShadowOutput = {};
            results->directionalShadowOutput.identity = results->identity;
            results->shadowStats = {};

            data.recorder = std::make_unique<ShadowPass>();
            data.recorder->SetResources(pipelineCache);
            data.recorder->SetResourceRegistry(resourceRegistry);
            data.recorder->InitializeGraphRecorder(renderScene);
            data.recorder->SetConfig(config);
            data.recorder->SetEnabled(requestedEnabled);
            if (!primaryLight.IsShadowEligible())
            {
                return;
            }
            data.recorder->Setup(builder, data.execution.view, primaryLight);

            results->shadowStats = data.recorder->GetStats();
            results->directionalShadowOutput =
                MakeDirectionalShadowRecordOutput(
                    data.execution.identity, *data.recorder);
        },
        [results, primaryLight](const GraphPassData& data, RHICommandContext& ctx)
        {
            if (!data.contextValid || !data.recorder || !results)
            {
                return;
            }
            data.recorder->Execute(ctx, data.execution.view, primaryLight);
            results->shadowStats = data.recorder->GetStats();
        });
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

    if (!m_pipelineCache->GetShadowDepthPipeline(MakeShadowDepthBiasState(m_config)))
    {
        m_unsupportedReason = "Shadow depth pipeline is not available";
        return false;
    }

    if (m_resourceRegistry == nullptr)
    {
        m_unsupportedReason = "RenderResourceRegistry is not available";
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

void ShadowPass::CalculateCascades(
    const ViewData& view,
    const PrimaryDirectionalLightRecordInput& primaryLight)
{
    if (m_cascades.empty())
        return;

    const float nearClip = std::max(0.001f, view.nearPlane);
    const float farClip = std::max(nearClip + 1.0f, view.farPlane);
    const float range = farClip - nearClip;
    const float ratio = farClip / nearClip;
    const float lambda = clamp(m_config.cascadeSplitLambda, 0.0f, 1.0f);
    const Vec3 lightDir = NormalizeOr(primaryLight.direction, Vec3(0.0f, -1.0f, 0.0f));
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

void ShadowPass::Setup(
    RenderGraphBuilder& builder,
    const ViewData& view,
    const PrimaryDirectionalLightRecordInput& primaryLight)
{
    if (!primaryLight.IsShadowEligible() || !IsEnabled())
        return;

    m_stats = {};
    m_shadowMapTextureHandle = {};
    m_cascadeTextureHandles.clear();
    m_cascadeViews.clear();
    m_plannedShadowDraws.clear();
    m_shadowDrawPreflightValid = false;
    m_cascades.resize(std::max(1u, m_config.numCascades));

    if (!view.renderGraph)
    {
        m_unsupportedReason = "RenderGraph is not available during ShadowPass setup";
        return;
    }

    CalculateCascades(view, primaryLight);
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

    // All caster constant pages and set-1 descriptors are fixed while the
    // graph is built. Execute() therefore has no allocation path after an
    // attachment is bound.
    m_shadowDrawPreflightValid = BuildPlannedShadowDraws(view);
    if (!m_shadowDrawPreflightValid)
    {
        RVX_RENDER_ERROR("ShadowPass: caster/page preflight failed; no shadow attachment will be recorded");
        m_shadowMapTextureHandle = {};
        m_cascadeTextureHandles.clear();
        m_cascadeViews.clear();
        m_stats.declaredCascadeResourceCount = 0;
    }
}

bool ShadowPass::BuildPlannedShadowDraws(const ViewData& view)
{
    m_plannedShadowDraws.clear();
    if (!m_pipelineCache || !m_renderScene || m_resourceRegistry == nullptr ||
        m_pipelineCache->GetShadowDepthPipeline(MakeShadowDepthBiasState(m_config)) == nullptr)
    {
        return false;
    }

    m_plannedShadowDraws.reserve(m_renderScene->GetObjectCount());
    for (size_t index = 0; index < m_renderScene->GetObjectCount(); ++index)
    {
        const RenderObject& object = m_renderScene->GetObject(index);
        if (!object.castsShadow)
        {
            continue;
        }

        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(m_resourceRegistry, object.mesh);
        if (!buffers.IsValid() || buffers.positionBuffer == nullptr ||
            buffers.indexBuffer == nullptr || buffers.submeshes.empty())
        {
            return false;
        }

        PlannedShadowDraw planned;
        planned.buffers = std::move(buffers);
        if (!m_pipelineCache->CreateObjectConstantBinding(
                object.worldMatrix,
                object.normalMatrix,
                object.previousWorldMatrix,
                view.previousViewProjectionMatrix,
                object.previousWorldMatrixValid != 0 &&
                    view.previousViewProjectionValid != 0 &&
                    !view.resetTemporalHistory,
                true,
                ResolveSkinningMatrices(object, planned.buffers),
                nullptr,
                planned.objectBinding) ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch,
                Ref<RefCounted>(planned.objectBinding.constantBuffer)) ||
            (planned.objectBinding.instanceBuffer &&
             !RetainRenderSubmissionResource(
                 view.submissionResourceBatch,
                 Ref<RefCounted>(planned.objectBinding.instanceBuffer))) ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch,
                Ref<RefCounted>(planned.objectBinding.descriptorSet)))
        {
            return false;
        }
        m_plannedShadowDraws.emplace_back(std::move(planned));
    }
    return true;
}

void ShadowPass::Execute(
    RHICommandContext& ctx,
    const ViewData& view,
    const PrimaryDirectionalLightRecordInput& primaryLight)
{
    if (!primaryLight.IsShadowEligible())
    {
        return;
    }

    if (!IsEnabled())
    {
        if (IsRequestedEnabled())
        {
            RVX_CORE_WARN("ShadowPass: unsupported pass skipped: {}", GetUnsupportedReason());
        }
        return;
    }

    if (!m_pipelineCache || !m_renderScene ||
        m_resourceRegistry == nullptr || !m_shadowDrawPreflightValid)
    {
        return;
    }

    // Get shadow depth-only pipeline for shadow rendering
    RHIPipeline* shadowPipeline = m_pipelineCache->GetShadowDepthPipeline(MakeShadowDepthBiasState(m_config));
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

    for (const RHITextureViewRef& cascadeView : m_cascadeViews)
    {
        RHITextureView* const viewHandle = cascadeView.Get();
        if (viewHandle == nullptr || viewHandle->GetTexture() == nullptr ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch, Ref<RefCounted>(viewHandle)) ||
            !RetainRenderSubmissionResource(
                view.submissionResourceBatch,
                Ref<RefCounted>(viewHandle->GetTexture())))
        {
            RVX_CORE_WARN("ShadowPass: submission ownership rejected cascade attachment");
            return;
        }
    }

    // Render each cascade
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RenderCascade(ctx, view, i, primaryLight);
    }

    m_pipelineCache->UpdateViewConstants(view);
}

bool ShadowPass::ResolveCascadeViews(const ViewData& view)
{
    m_cascadeViews.assign(m_cascadeTextureHandles.size(), RHITextureViewRef{});
    m_stats.resolvedCascadeViewCount = 0;

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

        m_cascadeViews[i] = RHITextureViewRef(viewHandle);
        ++m_stats.resolvedCascadeViewCount;
    }

    return m_stats.resolvedCascadeViewCount == m_cascadeTextureHandles.size();
}

void ShadowPass::RenderCascade(
    RHICommandContext& ctx,
    const ViewData& view,
    uint32_t cascadeIndex,
    const PrimaryDirectionalLightRecordInput& primaryLight)
{
    if (cascadeIndex >= m_cascadeViews.size() || !m_cascadeViews[cascadeIndex])
    {
        return;  // Cascade view not created
    }

    ViewData shadowView = view;
    shadowView.viewProjectionMatrix = m_cascades[cascadeIndex].viewProjection;
    shadowView.cameraForward = NormalizeOr(primaryLight.direction, Vec3(0.0f, -1.0f, 0.0f));
    m_pipelineCache->UpdateViewConstants(shadowView);

    // Begin shadow render pass for this cascade
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(m_cascadeViews[cascadeIndex].Get(),
                           RHILoadOp::Clear, RHIStoreOp::Store, m_pipelineCache->GetDepthClearValue(), 0);

    ctx.BeginRenderPass(rpDesc);

    // Set viewport for this cascade slice
    uint32_t size = m_config.shadowMapSize;
    RHIViewport viewport{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f};
    ctx.SetViewport(viewport);

    RHIRect scissor{0, 0, size, size};
    ctx.SetScissor(scissor);

    // Bind shadow pipeline
    RHIPipeline* pipeline = m_pipelineCache->GetShadowDepthPipeline(MakeShadowDepthBiasState(m_config));
    if (pipeline)
    {
        ctx.SetPipeline(pipeline);
    }

    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();
    if (frameSet)
    {
        ctx.SetDescriptorSet(0, frameSet);
    }

    // All page constants/descriptors were preflighted during graph setup.
    for (const PlannedShadowDraw& planned : m_plannedShadowDraws)
    {
        ++m_stats.shadowCasterCount;
        ctx.SetDescriptorSet(1,
                             planned.objectBinding.descriptorSet.Get(),
                             planned.objectBinding.dynamicOffsets);

        // Bind vertex buffers
        ctx.SetVertexBuffer(0, planned.buffers.positionBuffer);
        if (planned.buffers.boneIndicesBuffer)
        {
            ctx.SetVertexBuffer(4, planned.buffers.boneIndicesBuffer);
        }
        if (planned.buffers.boneWeightsBuffer)
        {
            ctx.SetVertexBuffer(5, planned.buffers.boneWeightsBuffer);
        }
        ctx.SetIndexBuffer(planned.buffers.indexBuffer, RHIFormat::R32_UINT);

        // Draw
        for (const SubmeshGPUInfo& submesh : planned.buffers.submeshes)
        {
            ctx.DrawIndexed(submesh.indexCount, 1, submesh.indexOffset, submesh.baseVertex, 0);
            ++m_stats.drawCount;
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
