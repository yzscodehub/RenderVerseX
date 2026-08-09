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
    DirectDrawPacket packet;
    MeshGPUBuffers buffers;
    SubmeshGPUInfo submesh;
    RHIPipeline* pipeline = nullptr;
    ObjectConstantBinding objectBinding;
    RHIBufferRef instanceIndexBuffer;
    uint32 representedPacketCount = 1;
    bool instanced = false;
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
        [results, primaryLight](const GraphPassData& data,
                                RenderGraphPassContext& context)
        {
            if (!data.contextValid || !data.recorder || !results)
            {
                return;
            }
            data.recorder->Execute(
                context, data.execution.view, primaryLight);
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

    const ShadowDepthBiasState biasState = MakeShadowDepthBiasState(m_config);
    if (!m_pipelineCache->GetShadowDepthPipeline(
            biasState, DefaultLitDirectVertexInputMode::Rigid) ||
        !m_pipelineCache->GetShadowDepthPipeline(
            biasState, DefaultLitDirectVertexInputMode::Skinned))
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
    m_cascadeViewHandles.clear();
    m_plannedShadowDraws.clear();
    m_directInstanceHandle = {};
    m_directInstanceIndexHandle = {};
    m_directInstancePlan = {};
    m_directInstanceStream = {};
    m_directInstancingPreflightFailed = false;
    m_shadowDrawPreflightValid = false;
    m_cascades.resize(std::max(1u, m_config.numCascades));

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

    m_shadowMapTextureHandle = builder.CreateTexture(shadowDesc);
    builder.SetExportState(
        m_shadowMapTextureHandle, RHIResourceState::ShaderResource);

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RGTextureHandle shadowLayer = m_shadowMapTextureHandle;
        shadowLayer.hasSubresourceRange = true;
        shadowLayer.subresourceRange = RHISubresourceRange{0, 1, i, 1, RHITextureAspect::Depth};
        m_cascadeTextureHandles.push_back(shadowLayer);

        RHITextureViewDesc viewDesc;
        viewDesc.format = depthFormat;
        viewDesc.dimension = shadowDesc.dimension;
        viewDesc.subresourceRange = shadowLayer.subresourceRange;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "ShadowCascadeLayerDSV";
        RGTextureViewHandle cascadeView =
            builder.CreateTextureView(m_shadowMapTextureHandle, viewDesc);
        cascadeView = builder.Write(
            cascadeView,
            MakeRGAccessDesc(
                RHIResourceState::DepthWrite,
                RHIShaderStage::None,
                RHIDiscardIntent::Discard));
        m_cascadeViewHandles.push_back(cascadeView);
    }

    m_stats.declaredCascadeResourceCount = static_cast<uint32_t>(m_cascadeTextureHandles.size());

    if (!PrepareDirectInstanceStream(builder, view))
    {
        m_directInstancingPreflightFailed = true;
    }

    // All caster constant pages and set-1 descriptors are fixed while the
    // graph is built. Execute() therefore has no allocation path after an
    // attachment is bound.
    m_shadowDrawPreflightValid = BuildPlannedShadowDraws(builder, view);
    if (!m_shadowDrawPreflightValid)
    {
        RVX_RENDER_ERROR("ShadowPass: caster/page preflight failed; no shadow attachment will be recorded");
        m_shadowMapTextureHandle = {};
        m_cascadeTextureHandles.clear();
        m_cascadeViewHandles.clear();
        m_stats.declaredCascadeResourceCount = 0;
    }
}

bool ShadowPass::BuildPlannedShadowDraws(RenderGraphBuilder& builder,
                                         const ViewData& view)
{
    m_plannedShadowDraws.clear();
    const ShadowDepthBiasState biasState = MakeShadowDepthBiasState(m_config);
    if (!m_pipelineCache || !m_renderScene || m_resourceRegistry == nullptr ||
        m_pipelineCache->GetShadowDepthPipeline(
            biasState, DefaultLitDirectVertexInputMode::Rigid) == nullptr ||
        m_pipelineCache->GetShadowDepthPipeline(
            biasState, DefaultLitDirectVertexInputMode::Skinned) == nullptr)
    {
        RVX_RENDER_ERROR("ShadowPass: planned draw preflight is missing renderer resources or a shadow pipeline");
        return false;
    }

    if (view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr)
    {
        RVX_RENDER_ERROR("ShadowPass: planned draw preflight is missing the frame plan or mesh-pass preparation");
        return false;
    }
    const DirectDrawPacketBatchBuildResult direct = BuildDirectDrawPacketBatch(
        *view.renderFrameExecutionPlan,
        RenderPassKind::Shadow,
        view.meshPassPreparation->shadow,
        nullptr);
    if (!direct.succeeded)
    {
        const MeshPassPacketStream& stream = view.meshPassPreparation->shadow;
        const RenderPassExecutionPlan* shadowPlan = nullptr;
        for (const RenderPassExecutionPlan& passPlan :
             view.renderFrameExecutionPlan->passes)
        {
            if (passPlan.pass == RenderPassKind::Shadow)
            {
                shadowPlan = &passPlan;
                break;
            }
        }
        RVX_RENDER_ERROR(
            "ShadowPass: direct shadow packet batch failed validation "
            "(stream input={}, relevant={}, candidates={}, direct={}, skipped={}, "
            "plan direct={}, gpu={}, skipped={})",
            stream.stats.inputPacketCount,
            stream.stats.relevantPacketCount,
            stream.stats.gpuCandidatePacketCount,
            stream.stats.directPacketCount,
            stream.stats.skippedPacketCount,
            shadowPlan != nullptr ? shadowPlan->partition.directPacketCount : 0u,
            shadowPlan != nullptr ? shadowPlan->partition.gpuDrivenPacketCount : 0u,
            shadowPlan != nullptr ? shadowPlan->partition.skippedPacketCount : 0u);
        return false;
    }

    m_plannedShadowDraws.reserve(direct.batch.packets.size());
    for (const DirectDrawPacket& draw : direct.batch.packets)
    {
        const RenderDrawPacket& packet = draw.packet;
        if (packet.pass != RenderPassKind::Shadow ||
            packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
            packet.primitiveData >= m_renderScene->GetObjectCount() ||
            packet.arguments.indexCount == 0 ||
            packet.arguments.instanceCount != 1 ||
            packet.arguments.firstInstance != 0)
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow packet violates the canonical packet contract");
            return false;
        }
        const RenderObject& object =
            m_renderScene->GetObject(packet.primitiveData);
        if (!object.castsShadow || packet.objectId == 0 ||
            object.entityId != packet.objectId ||
            object.mesh != packet.geometryKey.mesh)
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow packet does not match its RenderObject identity");
            return false;
        }

        MeshGPUBuffers buffers = ResolveRenderMeshBuffers(m_resourceRegistry, object.mesh);
        if (!buffers.IsValid() || buffers.positionBuffer == nullptr ||
            buffers.indexBuffer == nullptr ||
            packet.geometryKey.submeshIndex >= buffers.submeshes.size())
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow packet references unavailable geometry");
            return false;
        }
        const SubmeshGPUInfo submesh =
            buffers.submeshes[packet.geometryKey.submeshIndex];
        if (packet.arguments.indexCount != submesh.indexCount ||
            packet.arguments.firstIndex != submesh.indexOffset ||
            packet.arguments.vertexOffset != submesh.baseVertex)
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow packet index arguments do not match the resolved submesh");
            return false;
        }

        PlannedShadowDraw planned;
        planned.packet = draw;
        planned.buffers = std::move(buffers);
        planned.submesh = submesh;
        planned.pipeline = m_pipelineCache->GetShadowDepthPipeline(
            biasState,
            packet.pipelineKey.skinned
                ? DefaultLitDirectVertexInputMode::Skinned
                : DefaultLitDirectVertexInputMode::Rigid);
        if (planned.pipeline == nullptr)
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow packet has no compatible rigid/skinned pipeline");
            return false;
        }
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
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.constantBuffer)) ||
            (planned.objectBinding.instanceBuffer &&
             !builder.RetainSubmissionResource(
                 Ref<RefCounted>(planned.objectBinding.instanceBuffer))) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(planned.objectBinding.descriptorSet)))
        {
            RVX_RENDER_ERROR("ShadowPass: direct shadow object constants or retained bindings failed preflight");
            return false;
        }
        m_plannedShadowDraws.emplace_back(std::move(planned));
    }
    ApplyDirectInstancePlan(builder, view);
    return true;
}

bool ShadowPass::PrepareDirectInstanceStream(RenderGraphBuilder& builder,
                                             const ViewData& view)
{
    if (view.instancingMode == RenderInstancingMode::Disabled)
    {
        return true;
    }
    if (m_renderScene == nullptr || view.instanceBatchPlans == nullptr ||
        !view.instanceBatchPlans->shadowValid ||
        view.renderFrameExecutionPlan == nullptr ||
        view.meshPassPreparation == nullptr ||
        m_pipelineCache == nullptr)
    {
        return false;
    }
    m_directInstancePlan = view.instanceBatchPlans->shadow;
    if (m_directInstancePlan.instancedBatchCount == 0)
    {
        return true;
    }
    const DirectDrawPacketBatchBuildResult direct = BuildDirectDrawPacketBatch(
        *view.renderFrameExecutionPlan,
        RenderPassKind::Shadow,
        view.meshPassPreparation->shadow,
        nullptr);
    std::vector<GPUInstanceData> instances;
    if (!direct.succeeded ||
        !BuildRasterInstanceData(m_directInstancePlan,
                                 direct.batch,
                                 *m_renderScene,
                                 instances))
    {
        return false;
    }
    IRHIDevice* device = m_pipelineCache->GetDevice();
    if (device == nullptr ||
        !CreateRasterInstanceStream(*device,
                                    instances,
                                    "ShadowDirectInstancing",
                                    m_directInstanceStream) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instances)) ||
        !builder.RetainSubmissionResource(
            Ref<RefCounted>(m_directInstanceStream.instanceIndices)))
    {
        m_directInstanceStream = {};
        return false;
    }
    m_directInstanceHandle = builder.ImportBuffer(
        m_directInstanceStream.instances,
        RHIResourceState::ShaderResource);
    m_directInstanceIndexHandle = builder.ImportBuffer(
        m_directInstanceStream.instanceIndices,
        RHIResourceState::VertexBuffer);
    builder.Read(m_directInstanceHandle, RHIShaderStage::Vertex);
    builder.Read(m_directInstanceIndexHandle,
                 RHIResourceState::VertexBuffer,
                 RHIShaderStage::Vertex);
    return true;
}

void ShadowPass::ApplyDirectInstancePlan(RenderGraphBuilder& builder,
                                         const ViewData& view)
{
    if (view.instancingMode == RenderInstancingMode::Disabled ||
        m_directInstancePlan.instancedBatchCount == 0)
    {
        return;
    }
    if (m_directInstancingPreflightFailed ||
        !m_directInstanceStream.IsValid() ||
        m_directInstancePlan.executedPacketCount !=
            m_plannedShadowDraws.size())
    {
        m_stats.instancingFallbackBatchCount +=
            m_directInstancePlan.instancedBatchCount;
        return;
    }

    struct InstancedBinding
    {
        uint32 leaderIndex = 0;
        ObjectConstantBinding objectBinding;
    };
    std::vector<InstancedBinding> bindings;
    bindings.reserve(m_directInstancePlan.instancedBatchCount);
    std::vector<bool> consumed(m_plannedShadowDraws.size(), false);
    RHIPipeline* instancedPipeline =
        m_pipelineCache->GetInstancedShadowDepthPipeline(
            MakeShadowDepthBiasState(m_config));
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        if (batch.members.empty())
        {
            m_stats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        for (const RenderInstanceBatchMember& member : batch.members)
        {
            if (member.directPacketIndex >= m_plannedShadowDraws.size() ||
                consumed[member.directPacketIndex])
            {
                m_stats.instancingFallbackBatchCount +=
                    m_directInstancePlan.instancedBatchCount;
                return;
            }
            consumed[member.directPacketIndex] = true;
        }
        if (!batch.instanced)
        {
            continue;
        }
        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        const PlannedShadowDraw& leader = m_plannedShadowDraws[leaderIndex];
        ObjectConstantBinding binding;
        if (instancedPipeline == nullptr ||
            leader.packet.packet.pipelineKey.skinned ||
            !m_pipelineCache->CreateObjectConstantBinding(
                Mat4Identity(),
                Mat4Identity(),
                Mat4Identity(),
                view.previousViewProjectionMatrix,
                false,
                true,
                {},
                m_directInstanceStream.instances.Get(),
                binding) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(binding.constantBuffer)) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(binding.instanceBuffer)) ||
            !builder.RetainSubmissionResource(
                Ref<RefCounted>(binding.descriptorSet)))
        {
            m_stats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        bindings.push_back({leaderIndex, std::move(binding)});
    }
    if (!std::all_of(consumed.begin(), consumed.end(),
                     [](bool value) { return value; }))
    {
        m_stats.instancingFallbackBatchCount +=
            m_directInstancePlan.instancedBatchCount;
        return;
    }

    std::vector<PlannedShadowDraw> batched;
    batched.reserve(m_directInstancePlan.batches.size());
    size_t bindingIndex = 0;
    for (const RenderInstanceBatch& batch : m_directInstancePlan.batches)
    {
        const uint32 leaderIndex = batch.members.front().directPacketIndex;
        if (!batch.instanced)
        {
            batched.push_back(std::move(m_plannedShadowDraws[leaderIndex]));
            continue;
        }
        if (bindingIndex >= bindings.size() ||
            bindings[bindingIndex].leaderIndex != leaderIndex)
        {
            m_stats.instancingFallbackBatchCount +=
                m_directInstancePlan.instancedBatchCount;
            return;
        }
        PlannedShadowDraw leader =
            std::move(m_plannedShadowDraws[leaderIndex]);
        leader.pipeline = instancedPipeline;
        leader.objectBinding =
            std::move(bindings[bindingIndex].objectBinding);
        leader.instanceIndexBuffer = m_directInstanceStream.instanceIndices;
        leader.packet.packet.arguments.instanceCount =
            static_cast<uint32>(batch.members.size());
        leader.packet.packet.arguments.firstInstance = batch.firstInstance;
        leader.representedPacketCount =
            static_cast<uint32>(batch.members.size());
        leader.instanced = true;
        batched.push_back(std::move(leader));
        ++bindingIndex;
    }
    m_plannedShadowDraws = std::move(batched);
}

void ShadowPass::Execute(
    RenderGraphPassContext& context,
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

    m_stats.resolvedCascadeViewCount = 0;
    for (RGTextureViewHandle cascadeView : m_cascadeViewHandles)
    {
        RHITextureView* const viewHandle =
            context.GetTextureView(cascadeView);
        if (viewHandle == nullptr || viewHandle->GetTexture() == nullptr)
        {
            RVX_CORE_WARN(
                "ShadowPass: explicit cascade view was not realized");
            return;
        }
        ++m_stats.resolvedCascadeViewCount;
    }

    // Render each cascade
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_cascades.size()); ++i)
    {
        RenderCascade(context, view, i, primaryLight);
    }

    m_pipelineCache->UpdateViewConstants(view);
}

void ShadowPass::RenderCascade(
    RenderGraphPassContext& context,
    const ViewData& view,
    uint32_t cascadeIndex,
    const PrimaryDirectionalLightRecordInput& primaryLight)
{
    if (cascadeIndex >= m_cascadeViewHandles.size())
    {
        return;  // Cascade view not created
    }
    RHITextureView* const cascadeView =
        context.GetTextureView(m_cascadeViewHandles[cascadeIndex]);
    if (!cascadeView)
        return;
    RHICommandContext& ctx = context.Commands();

    ViewData shadowView = view;
    shadowView.viewProjectionMatrix = m_cascades[cascadeIndex].viewProjection;
    shadowView.cameraForward = NormalizeOr(primaryLight.direction, Vec3(0.0f, -1.0f, 0.0f));
    m_pipelineCache->UpdateViewConstants(shadowView);

    // Begin shadow render pass for this cascade
    RHIRenderPassDesc rpDesc;
    rpDesc.SetDepthStencil(cascadeView,
                           RHILoadOp::Clear, RHIStoreOp::Store, m_pipelineCache->GetDepthClearValue(), 0);

    ctx.BeginRenderPass(rpDesc);

    // Set viewport for this cascade slice
    uint32_t size = m_config.shadowMapSize;
    RHIViewport viewport{0.0f, 0.0f, static_cast<float>(size), static_cast<float>(size), 0.0f, 1.0f};
    ctx.SetViewport(viewport);

    RHIRect scissor{0, 0, size, size};
    ctx.SetScissor(scissor);

    RHIDescriptorSet* frameSet = m_pipelineCache->GetFrameDescriptorSet();

    // All page constants/descriptors were preflighted during graph setup.
    for (const PlannedShadowDraw& planned : m_plannedShadowDraws)
    {
        m_stats.shadowCasterCount += planned.representedPacketCount;
        ctx.SetPipeline(planned.pipeline);
        if (frameSet)
        {
            ctx.SetDescriptorSet(0, frameSet);
        }
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
        if (planned.instanced)
        {
            ctx.SetVertexBuffer(6, planned.instanceIndexBuffer.Get());
        }
        ctx.SetIndexBuffer(planned.buffers.indexBuffer, RHIFormat::R32_UINT);

        const RenderDrawArguments& arguments = planned.packet.packet.arguments;
        ctx.DrawIndexed(arguments.indexCount,
                        arguments.instanceCount,
                        arguments.firstIndex,
                        arguments.vertexOffset,
                        arguments.firstInstance);
        ++m_stats.drawCount;
        m_stats.submittedInstanceCount += arguments.instanceCount;
        if (planned.instanced)
        {
            ++m_stats.instancedBatchCount;
        }
    }

    ctx.EndRenderPass();
}

} // namespace RVX
