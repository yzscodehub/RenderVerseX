/**
 * @file SceneRenderer.cpp
 * @brief SceneRenderer implementation
 */

#include "Render/Renderer/SceneRenderer.h"
#include "Context/RenderContextInternal.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Core/Assert.h"
#include "Core/Log.h"
#include "Core/PathUtils.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/DepthPrepass.h"
#include "Render/Passes/DirectDrawPacketBatch.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Render/Passes/RenderPassClearValues.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/Passes/RayTracedShadowPass.h"
#include "Render/Passes/ShadowPass.h"
#include "Render/Passes/SkyboxPass.h"
#include "Render/Passes/TransparentPass.h"
#include "Render/PostProcess/Bloom.h"
#include "Render/PostProcess/ChromaticAberration.h"
#include "Render/PostProcess/ColorGrading.h"
#include "Render/PostProcess/FilmGrain.h"
#include "Render/PostProcess/FXAA.h"
#include "Render/PostProcess/SSAO.h"
#include "Render/PostProcess/ToneMapping.h"
#include "Render/PostProcess/Vignette.h"
#include "Render/RayTracing/RayTracingScene.h"
#include "Renderer/RenderFrameResourceBinder.h"
#include "Renderer/RenderPassRegistry.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderResourceResolver.h"
#include "Resources/RenderRetirementQueue.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    MeshPassResourceAvailability ResolvePassResourceAvailability(
        const RenderResourceRegistry* registry,
        RenderResourceHandle handle,
        bool gpuDataReady)
    {
        if (registry == nullptr || !handle.IsValid())
        {
            return MeshPassResourceAvailability::Unavailable;
        }

        const RenderResourceStatus status = registry->QueryStatus(handle);
        if (status.code != RenderResourceStatusCode::Current)
        {
            return MeshPassResourceAvailability::Unavailable;
        }

        switch (status.state)
        {
            case RenderResourcePublicState::Reserved:
            case RenderResourcePublicState::UploadQueued:
            case RenderResourcePublicState::Uploading:
                return MeshPassResourceAvailability::Pending;
            case RenderResourcePublicState::GPUReady:
                return gpuDataReady
                           ? MeshPassResourceAvailability::Ready
                           : MeshPassResourceAvailability::Unavailable;
            case RenderResourcePublicState::Released:
            case RenderResourcePublicState::Failed:
            case RenderResourcePublicState::Evicting:
            default:
                return MeshPassResourceAvailability::Unavailable;
        }
    }

    MeshPassProcessorInput MakeMeshPassProcessorInput(
        RenderDrawPacket packet,
        const RenderResourceRegistry* registry,
        uint32 sourceOrdinal,
        float32 viewDepth)
    {
        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            registry, packet.geometryKey.mesh);
        const bool geometryReady = buffers.IsValid() &&
            packet.geometryKey.submeshIndex < buffers.submeshes.size();
        if (geometryReady)
        {
            const SubmeshGPUInfo& submesh =
                buffers.submeshes[packet.geometryKey.submeshIndex];
            packet.arguments.indexCount = submesh.indexCount;
            packet.arguments.firstIndex = submesh.indexOffset;
            packet.arguments.vertexOffset = submesh.baseVertex;
        }

        MeshPassProcessorInput input;
        input.packet = packet;
        input.availability.pipeline = MeshPassResourceAvailability::Ready;
        input.availability.geometry = ResolvePassResourceAvailability(
            registry, packet.geometryKey.mesh, geometryReady);
        input.availability.material = ResolvePassResourceAvailability(
            registry,
            packet.materialKey.material,
            registry != nullptr &&
                registry->ResolveMaterial(packet.materialKey.material) != nullptr);
        input.sourceOrdinal = sourceOrdinal;
        input.viewDepth = viewDepth;
        return input;
    }

    RenderDrawFlags SetRenderDrawFlag(RenderDrawFlags flags,
                                      RenderDrawFlags flag,
                                      bool enabled)
    {
        const uint32 flagBits = static_cast<uint32>(flag);
        const uint32 resultBits = enabled
                                      ? static_cast<uint32>(flags) | flagBits
                                      : static_cast<uint32>(flags) & ~flagBits;
        return static_cast<RenderDrawFlags>(resultBits);
    }

    RenderDrawPacket BuildShadowPacket(const RenderScene& scene,
                                       const RenderObject& object,
                                       const MeshBatch& batch)
    {
        RenderDrawPacket packet;
        if (scene.FindCachedDrawPacketTemplate(batch, packet))
        {
            packet.objectId = batch.objectId;
            packet.primitiveData = batch.primitiveData;
            packet.submeshIndex = batch.submeshIndex;
        }
        else
        {
            packet = BuildLegacyMaterialDrawPacket(batch);
        }
        packet.flags = SetRenderDrawFlag(
            packet.flags, RenderDrawFlags::CastsShadow, object.castsShadow);
        packet.flags = SetRenderDrawFlag(
            packet.flags, RenderDrawFlags::Skinned, object.HasSkinningData());
        return packet;
    }

    MeshBatch BuildLegacyShadowBatch(const RenderObject& object,
                                     uint32 objectIndex,
                                     uint32 submeshIndex,
                                     const SubmeshGPUInfo* submesh)
    {
        RenderBatchFlags flags = RenderBatchFlags::None;
        if (object.HasSkinningData())
            flags |= RenderBatchFlags::Skinned;
        if (object.castsShadow)
            flags |= RenderBatchFlags::CastsShadow;
        if (object.receivesShadow)
            flags |= RenderBatchFlags::ReceivesShadow;

        const RenderMaterialMode materialMode =
            submeshIndex < object.materialModes.size()
                ? object.materialModes[submeshIndex]
                : RenderMaterialMode::Opaque;
        if (materialMode == RenderMaterialMode::Masked)
            flags |= RenderBatchFlags::Masked;
        if (materialMode == RenderMaterialMode::Transparent)
            flags |= RenderBatchFlags::Transparent;
        if (!object.material.IsValid())
            flags |= RenderBatchFlags::MissingMaterial;

        MeshBatch batch;
        batch.objectId = object.entityId;
        batch.mesh = object.mesh;
        batch.material = object.material;
        batch.submeshIndex = submeshIndex;
        batch.primitiveData = objectIndex;
        batch.materialMode = materialMode;
        batch.flags = flags;
        if (submesh != nullptr)
        {
            batch.geometry.indexOffset = submesh->indexOffset;
            batch.geometry.indexCount = submesh->indexCount;
            batch.geometry.baseVertex = submesh->baseVertex;
        }
        return batch;
    }

    uint64 GetRenderObjectHistoryKey(const RenderObject& object)
    {
        return object.entityId;
    }

    float ClampFiniteRange(float value, float fallback, float minValue, float maxValue)
    {
        return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : fallback;
    }

    uint32 ResolveRayTracingBudgetReflectionDimension(uint32 dimension, float scale)
    {
        if (dimension == 0)
            return 0;

        const float scaled = std::ceil(static_cast<float>(dimension) * scale);
        return std::max(1u, static_cast<uint32>(scaled));
    }

    uint64 EstimateRayTracingPixelCount(uint32 width, uint32 height)
    {
        return static_cast<uint64>(width) * static_cast<uint64>(height);
    }

    uint64 EstimateReflectionRayCount(uint32 width, uint32 height, float resolutionScale, uint32 samplesPerPixel)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) * static_cast<uint64>(samplesPerPixel);
    }

    float ReduceReflectionResolutionScaleToFitRayBudget(uint32 width,
                                                        uint32 height,
                                                        float resolutionScale,
                                                        float minResolutionScale,
                                                        uint32 samplesPerPixel,
                                                        uint64 availableRayBudget)
    {
        if (width == 0 || height == 0 || samplesPerPixel == 0 || availableRayBudget == 0)
        {
            return minResolutionScale;
        }

        float scale = std::clamp(resolutionScale, minResolutionScale, 1.0f);
        uint64 estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        while (estimatedRayCount > availableRayBudget && scale > minResolutionScale)
        {
            const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, scale);
            const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, scale);

            float nextScale = scale;
            if (scaledWidth > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledWidth - 1u) / static_cast<float>(width), 0.0f));
            }
            if (scaledHeight > 1)
            {
                nextScale = std::min(
                    nextScale,
                    std::nextafter(static_cast<float>(scaledHeight - 1u) / static_cast<float>(height), 0.0f));
            }

            nextScale = std::max(minResolutionScale, nextScale);
            if (nextScale >= scale)
            {
                break;
            }

            scale = nextScale;
            estimatedRayCount = EstimateReflectionRayCount(width, height, scale, samplesPerPixel);
        }

        return scale;
    }

    uint32 EstimateReflectionDenoiseKernelTapCount(uint32 radius)
    {
        if (radius == 0)
            return 0;

        const uint32 diameter = radius * 2u + 1u;
        return diameter * diameter;
    }

    uint64 EstimateReflectionDenoiseTapCount(uint32 width, uint32 height, float resolutionScale, uint32 radius)
    {
        const uint32 scaledWidth = ResolveRayTracingBudgetReflectionDimension(width, resolutionScale);
        const uint32 scaledHeight = ResolveRayTracingBudgetReflectionDimension(height, resolutionScale);
        return EstimateRayTracingPixelCount(scaledWidth, scaledHeight) *
               static_cast<uint64>(EstimateReflectionDenoiseKernelTapCount(radius));
    }

    uint32 ApplyRayTracingGpuBudgetScale(uint32 value, uint32 minValue, float qualityScale)
    {
        if (value == 0 || qualityScale >= 0.999f)
            return value;

        const uint32 scaledValue = std::max(minValue,
                                           static_cast<uint32>(std::floor(static_cast<float>(value) * qualityScale)));
        if (scaledValue == value && value > minValue)
            return value - 1u;

        return scaledValue;
    }

    PostProcessSettings MakeDefaultRuntimePostProcessSettings()
    {
        PostProcessSettings settings;
        settings.enableToneMapping = true;
        settings.exposure = 1.0f;
        settings.gamma = 2.2f;
        settings.toneMappingOperator = ToneMappingOperator::ACES;
        settings.enableBloom = true;
        settings.bloomIntensity = 0.0f;
        settings.enableFXAA = false;
        settings.enableDOF = false;
        settings.enableMotionBlur = false;
        settings.enableColorGrading = false;
        settings.enableVignette = false;
        settings.enableChromaticAberration = false;
        settings.enableFilmGrain = false;
        settings.enableVolumetricLighting = false;
        settings.enableSSAO = false;
        settings.enableSSR = false;
        settings.enableRayTracedReflections = false;
        settings.enableRayTracedReflectionDenoise = true;
        settings.enableTAA = false;
        return settings;
    }

    RenderPolicyReadiness ResolveGPUShaderReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        switch (decision.fallbackReason)
        {
            case GPUCullingFallbackReason::ShaderFileMissing:
            case GPUCullingFallbackReason::ShaderCompilationFailed:
                return RenderPolicyReadiness::Unavailable;
            default:
                return RenderPolicyReadiness::Ready;
        }
    }

    RenderPolicyReadiness ResolveGPUBindingReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        switch (decision.fallbackReason)
        {
            case GPUCullingFallbackReason::DescriptorSetLayoutCreationFailed:
            case GPUCullingFallbackReason::PipelineLayoutCreationFailed:
            case GPUCullingFallbackReason::DescriptorSetCreationFailed:
                return RenderPolicyReadiness::Unavailable;
            default:
                return RenderPolicyReadiness::Ready;
        }
    }

    RenderPolicyReadiness ResolveGPUResourceReadiness(
        const GPUCullingExecutionDecision& decision)
    {
        return decision.fallbackReason ==
                       GPUCullingFallbackReason::PipelineResourcesUnavailable
            ? RenderPolicyReadiness::Unavailable
            : RenderPolicyReadiness::Ready;
    }

    bool IsGPUImplementationAvailable(
        const GPUCullingExecutionDecision& decision)
    {
        return decision.fallbackReason != GPUCullingFallbackReason::DeviceMissing &&
               decision.fallbackReason !=
                   GPUCullingFallbackReason::ShaderBackendUnsupported;
    }

    template <typename Resolver>
    RenderPolicyReadiness ResolveAnyGPUOwnerReadiness(
        const GPUCulling* depthOwner,
        const GPUCulling* opaqueOwner,
        Resolver&& resolver)
    {
        RenderPolicyReadiness aggregate = RenderPolicyReadiness::Unavailable;
        for (const GPUCulling* owner : {depthOwner, opaqueOwner})
        {
            if (owner == nullptr)
            {
                continue;
            }
            const RenderPolicyReadiness readiness =
                resolver(owner->GetExecutionDecision());
            if (readiness == RenderPolicyReadiness::Ready)
            {
                return RenderPolicyReadiness::Ready;
            }
            if (readiness == RenderPolicyReadiness::Pending)
            {
                aggregate = RenderPolicyReadiness::Pending;
            }
        }
        return aggregate;
    }

    RenderPassPolicyFacts MakeRenderPassPolicyFacts(
        RenderPassKind pass,
        const IRenderPass* renderPass,
        const MeshPassPacketStream& stream,
        const GPUCullingExecutionDecision& gpuExecution,
        bool gpuPassPipelineReady)
    {
        RenderPassPolicyFacts facts;
        facts.pass = pass;
        facts.requested = renderPass && renderPass->IsRequestedEnabled();
        facts.supported = renderPass && renderPass->IsSupported();
        facts.directAllowed = facts.supported;
        facts.gpuDrivenAllowed = facts.supported &&
            (pass == RenderPassKind::Depth || pass == RenderPassKind::Opaque);
        facts.fixedCountIndirectAllowed = false;
        const RenderPolicyReadiness directReadiness = facts.supported
            ? RenderPolicyReadiness::Ready
            : RenderPolicyReadiness::Unavailable;
        facts.directShaderReadiness = directReadiness;
        facts.directPipelineReadiness = directReadiness;
        facts.directResourceReadiness = directReadiness;
        facts.gpuDrivenShaderReadiness =
            ResolveGPUShaderReadiness(gpuExecution);
        facts.gpuDrivenPipelineReadiness =
            gpuExecution.pipelineReady && gpuPassPipelineReady
            ? RenderPolicyReadiness::Ready
            : RenderPolicyReadiness::Unavailable;
        facts.gpuDrivenResourceReadiness =
            ResolveGPUResourceReadiness(gpuExecution);
        facts.inputPacketCount = stream.stats.inputPacketCount;
        facts.relevantPacketCount = stream.stats.relevantPacketCount;
        facts.candidatePacketCount = stream.stats.gpuCandidatePacketCount;
        facts.directPacketCount = stream.stats.directPacketCount;
        facts.skippedPacketCount = stream.stats.skippedPacketCount;
        facts.drawGroupCount = static_cast<uint32>(stream.groups.size());
        facts.workloadBeneficial = facts.candidatePacketCount != 0;
        return facts;
    }

    GPUDrivenPolicyReason ProjectLegacyPolicyReason(RenderPolicyReason reason)
    {
        switch (reason)
        {
            case RenderPolicyReason::None:
            case RenderPolicyReason::ForcedGPUDriven:
                return GPUDrivenPolicyReason::None;
            case RenderPolicyReason::InvalidRequest:
                return GPUDrivenPolicyReason::InvalidMode;
            case RenderPolicyReason::ForcedDirect:
                return GPUDrivenPolicyReason::ForcedDisabled;
            case RenderPolicyReason::BackendNotQualified:
                return GPUDrivenPolicyReason::BackendNotQualified;
            case RenderPolicyReason::QualificationInvalid:
                return GPUDrivenPolicyReason::QualificationInvalid;
            case RenderPolicyReason::PipelineUnavailable:
            case RenderPolicyReason::PipelinePending:
            case RenderPolicyReason::ShaderUnavailable:
            case RenderPolicyReason::ShaderPending:
            case RenderPolicyReason::BindingsUnavailable:
            case RenderPolicyReason::BindingsPending:
                return GPUDrivenPolicyReason::PipelineUnavailable;
            case RenderPolicyReason::CapabilityUnavailable:
                return GPUDrivenPolicyReason::IndirectDrawCountUnsupported;
            default:
                return GPUDrivenPolicyReason::PipelineUnavailable;
        }
    }

}

bool SceneRendererExternalTargetDesc::IsValid() const
{
    return colorTarget &&
           colorTarget->GetWidth() > 0 &&
           colorTarget->GetHeight() > 0;
}

SceneRenderer::SceneRenderer() = default;

SceneRenderer::~SceneRenderer()
{
    Shutdown();
}

bool SceneRenderer::SupportsHDRSceneColor() const
{
    const IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
    if (!device)
    {
        return false;
    }

    switch (device->GetBackendType())
    {
        case RHIBackendType::DX11:
        case RHIBackendType::DX12:
        case RHIBackendType::Vulkan:
        case RHIBackendType::Metal:
        case RHIBackendType::OpenGL:
            return true;
        case RHIBackendType::Auto:
        case RHIBackendType::None:
        default:
            return false;
    }
}

ToneMappingOutputColorSpace SceneRenderer::ResolveToneMappingOutputColorSpace(RHIFormat outputFormat) const
{
    if (IsSRGBFormat(outputFormat))
    {
        return ToneMappingOutputColorSpace::Linear;
    }

    switch (outputFormat)
    {
        case RHIFormat::R16_FLOAT:
        case RHIFormat::R32_FLOAT:
        case RHIFormat::RG16_FLOAT:
        case RHIFormat::RG32_FLOAT:
        case RHIFormat::RG11B10_FLOAT:
        case RHIFormat::RGBA16_FLOAT:
        case RHIFormat::RGBA32_FLOAT:
            return ToneMappingOutputColorSpace::Linear;
        case RHIFormat::RGBA8_UNORM:
        case RHIFormat::BGRA8_UNORM:
        case RHIFormat::RGB10A2_UNORM:
        default:
            return ToneMappingOutputColorSpace::SRGB;
    }
}

SceneColorFormatPolicy SceneRenderer::ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                                    bool postProcessActive) const
{
    SceneColorFormatPolicy policy;
    policy.backBufferFormat = backBufferFormat;
    policy.toneMappingOutputFormat = backBufferFormat;
    policy.toneMappingOutputColorSpace = ResolveToneMappingOutputColorSpace(policy.toneMappingOutputFormat);
    policy.actualSceneColorFormat = backBufferFormat;

    if (backBufferFormat == RHIFormat::Unknown)
    {
        policy.hdrFallbackReason = "back buffer format is unavailable";
        return policy;
    }

    if (!postProcessActive)
    {
        policy.hdrFallbackReason = "post-process stack has no supported enabled effects";
        return policy;
    }

    if (!SupportsHDRSceneColor())
    {
        policy.hdrFallbackReason = "current RHI backend does not advertise HDR scene color support";
        return policy;
    }

    policy.actualSceneColorFormat = policy.requestedSceneColorFormat;
    policy.hdrSceneColorEnabled = true;
    policy.hdrFallbackReason.clear();
    return policy;
}

void SceneRenderer::Initialize(
    RenderContext* renderContext,
    RenderResourceRegistry* resourceRegistry,
    RenderRetirementQueue* retirementQueue)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("SceneRenderer already initialized");
        return;
    }

    if (!renderContext || !resourceRegistry || !retirementQueue)
    {
        RVX_CORE_ERROR("SceneRenderer: Invalid render-owned dependencies");
        return;
    }

    m_renderContext = renderContext;
    m_renderResourceRegistry = resourceRegistry;
    m_retirementQueue = retirementQueue;
    m_passRegistry = std::make_unique<RenderPassRegistry>();
    m_rayTracingSceneManager = std::make_unique<RayTracingSceneManager>();
    m_rayTracingSceneManager->Initialize(m_renderContext->GetDevice());
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();

    // Create render graph
    m_renderGraph = std::make_unique<RenderGraph>();
    m_renderGraph->SetDevice(m_renderContext->GetDevice());

    // Candidate extraction is shared, but Depth and Opaque own independent
    // instance/group/indirect/count streams. This keeps their policy and
    // failure domains independent until a later GPU-scene compaction stage.
    GPUCullingConfig gpuCullingConfig;
    gpuCullingConfig.enableOcclusionCulling = false;
    gpuCullingConfig.enableDistanceCulling = false;
    const FrameSynchronizer* frameSynchronizer =
        m_renderContext->GetFrameSynchronizer();
    const uint32 gpuCullingFrameSlotCount = frameSynchronizer != nullptr
        ? frameSynchronizer->GetFrameCount()
        : 1u;
    m_depthGPUCulling = std::make_unique<GPUCulling>();
    m_depthGPUCulling->Initialize(
        m_renderContext->GetDevice(), gpuCullingConfig,
        gpuCullingFrameSlotCount);
    m_opaqueGPUCulling = std::make_unique<GPUCulling>();
    m_opaqueGPUCulling->Initialize(
        m_renderContext->GetDevice(), gpuCullingConfig,
        gpuCullingFrameSlotCount);

    // Create pipeline cache with shader reflection
    m_pipelineCache = std::make_unique<PipelineCache>();

    // Create material system after pipeline layouts are available.
    m_materialSystem = std::make_unique<MaterialSystem>();
    m_lightManager = std::make_unique<LightManager>();
    m_lightManager->Initialize(renderContext->GetDevice());
    m_clusteredLighting = std::make_unique<ClusteredLighting>();
    if (!m_clusteredLighting->Initialize(renderContext->GetDevice()))
    {
        m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
        RVX_CORE_WARN("SceneRenderer: ClusteredLighting unavailable: {}",
                      m_clusteredLightingStats.fallbackReason);
    }

    // Create transient resource pool for RenderGraph
    m_transientResourcePool = std::make_unique<TransientResourcePool>();
    RenderSubmissionTracker* submissionTracker =
        RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext);
    m_transientResourcePool->Initialize(m_renderContext->GetDevice(),
                                        submissionTracker,
                                        m_retirementQueue);
    m_renderGraph->SetTransientResourcePool(m_transientResourcePool.get());

    // Create resource view cache for automatic view management
    m_resourceViewCache = std::make_unique<ResourceViewCache>();
    m_resourceViewCache->Initialize(m_renderContext->GetDevice(),
                                    submissionTracker,
                                    m_retirementQueue);

    RVX_CORE_INFO("SceneRenderer: Searching for shader directory...");
    RVX_CORE_INFO("  Current working directory: {}", std::filesystem::current_path().string());

    // Determine shader directory
    std::string shaderDir = m_shaderDir;
    if (shaderDir.empty())
    {
        const std::filesystem::path workspaceShaderDir =
            ResolveWorkspaceRelativePath(
                std::filesystem::path("Render") / "Shaders");
        RVX_CORE_DEBUG("  Checking workspace shader directory: {} -> exists: {}",
                       workspaceShaderDir.string(),
                       std::filesystem::exists(workspaceShaderDir));
        if (std::filesystem::exists(workspaceShaderDir))
        {
            shaderDir = workspaceShaderDir.string();
            RVX_CORE_INFO("  Found shader directory: {}", shaderDir);
        }
    }
    if (shaderDir.empty())
    {
        // Default shader directory - look for Render/Shaders in several locations
        std::vector<std::string> searchPaths = {
            "Render/Shaders",
            "../Render/Shaders",
            "../../Render/Shaders",
            "../../../Render/Shaders",
            "../../../../Render/Shaders",
            "../../../../../Render/Shaders"
        };

        for (const auto& path : searchPaths)
        {
            RVX_CORE_DEBUG("  Checking: {} -> exists: {}", path, std::filesystem::exists(path));
            if (std::filesystem::exists(path))
            {
                shaderDir = path;
                RVX_CORE_INFO("  Found shader directory: {}", path);
                break;
            }
        }
    }

    if (shaderDir.empty())
    {
        RVX_CORE_ERROR("SceneRenderer: Could not find shader directory!");
    }

    // Get render target format from swap chain
    RHIFormat rtFormat = RHIFormat::BGRA8_UNORM;
    if (m_renderContext->GetSwapChain())
    {
        rtFormat = m_renderContext->GetSwapChain()->GetFormat();
        RVX_CORE_INFO("SceneRenderer: Render target format from swap chain: {}", static_cast<int>(rtFormat));
    }
    else
    {
        RVX_CORE_WARN("SceneRenderer: Swap chain not ready, using default render target format: {}", static_cast<int>(rtFormat));
    }
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(rtFormat, true);
    m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.actualSceneColorFormat,
                                            m_sceneColorFormatPolicy.toneMappingOutputFormat);
    RVX_CORE_INFO("SceneRenderer: Pipeline formats scene={}, postProcessIntermediate={}, toneMappingOutput={}",
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.actualSceneColorFormat),
                  static_cast<int>(m_sceneColorFormatPolicy.toneMappingOutputFormat));
    if (!m_sceneColorFormatPolicy.hdrSceneColorEnabled)
    {
        RVX_CORE_WARN("SceneRenderer: HDR scene color fallback: {}",
                      m_sceneColorFormatPolicy.hdrFallbackReason);
    }

    // Initialize pipeline cache
    RVX_CORE_INFO("SceneRenderer: Initializing PipelineCache...");
    if (!shaderDir.empty() && m_pipelineCache->Initialize(m_renderContext->GetDevice(), shaderDir))
    {
        RVX_CORE_INFO("SceneRenderer: PipelineCache initialized successfully!");
        RVX_CORE_INFO("  OpaquePipeline: {}", m_pipelineCache->GetOpaquePipeline() ? "created" : "null");
    }
    else
    {
        RVX_CORE_ERROR("SceneRenderer: PipelineCache failed to initialize - rendering will be limited");
        RVX_CORE_ERROR("  shaderDir was: '{}'", shaderDir);
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized() && m_materialSystem)
    {
        if (!m_materialSystem->Initialize(
                m_renderContext->GetDevice(),
                m_pipelineCache->GetMaterialSetLayout(),
                m_renderResourceRegistry))
        {
            RVX_CORE_ERROR("SceneRenderer: MaterialSystem failed to initialize");
        }
    }

    // Setup default passes (can be customized later)
    SetupDefaultPasses();
    SetupDefaultPostProcess();

    m_initialized = true;
    RVX_CORE_DEBUG("SceneRenderer initialized");
}

void SceneRenderer::Shutdown()
{
    InvalidateRenderFramePlan();
    if (!m_initialized)
        return;

    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Pending submission ownership requires retirement queue");
        m_submissionBatch->ReleaseUnsubmitted(*m_retirementQueue);
        m_submissionBatch.reset();
        m_viewData.submissionResourceBatch = nullptr;
    }

    ClearPasses();
    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_rayTracedShadowPass = nullptr;
    m_cameraVelocityPass = nullptr;
    m_objectVelocityPass = nullptr;
    m_rayTracedReflectionPass = nullptr;
    m_rayTracedReflectionDenoisePass = nullptr;
    m_rayTracedReflectionCompositePass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_bloomPostProcess = nullptr;
    m_toneMappingPostProcess = nullptr;
    m_colorGradingPostProcess = nullptr;
    m_chromaticAberrationPostProcess = nullptr;
    m_vignettePostProcess = nullptr;
    m_filmGrainPostProcess = nullptr;
    m_ssaoPostProcess = nullptr;
    m_fxaaPostProcess = nullptr;
    if (m_postProcessStack)
    {
        m_postProcessStack->Shutdown();
        m_postProcessStack.reset();
    }

    if (m_materialSystem)
    {
        m_materialSystem->Shutdown();
        m_materialSystem.reset();
    }
    if (m_lightManager)
    {
        m_lightManager->Shutdown();
        m_lightManager.reset();
    }
    if (m_clusteredLighting)
    {
        m_clusteredLighting->Shutdown();
        m_clusteredLighting.reset();
    }

    if (m_pipelineCache)
    {
        m_pipelineCache->Shutdown();
        m_pipelineCache.reset();
    }

    if (m_transientResourcePool)
    {
        m_transientResourcePool->Shutdown();
        m_transientResourcePool.reset();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Shutdown();
        m_resourceViewCache.reset();
    }

    if (m_rayTracingSceneManager)
    {
        m_rayTracingSceneManager->Shutdown();
        m_rayTracingSceneManager.reset();
    }

    if (m_depthGPUCulling)
    {
        m_depthGPUCulling->Shutdown();
        m_depthGPUCulling.reset();
    }
    if (m_opaqueGPUCulling)
    {
        m_opaqueGPUCulling->Shutdown();
        m_opaqueGPUCulling.reset();
    }

    m_renderGraph.reset();
    m_passRegistry.reset();
    m_rayTracingSceneStats = {};
    m_rayTracingFrameBudgetStats = {};
    m_previousViewProjectionMatrix = Mat4Identity();
    m_previousViewProjectionValid = false;
    m_pendingTemporalHistoryReset = false;
    m_previousObjectWorldMatrices.clear();
    m_preGraphPrepareCallbacks.clear();
    m_renderResourceRegistry = nullptr;
    m_retirementQueue = nullptr;
    m_renderContext = nullptr;
    m_initialized = false;

    RVX_CORE_DEBUG("SceneRenderer shutdown");
}

void SceneRenderer::RequestTemporalHistoryReset()
{
    m_pendingTemporalHistoryReset = true;
}

void SceneRenderer::PrepareForSwapChainResize()
{
    InvalidateRenderFramePlan();
    if (!m_initialized)
        return;

    if (m_pipelineCache)
    {
        m_pipelineCache->ResetFrameResourceBindings();
    }

    if (m_renderGraph)
    {
        m_renderGraph->Clear();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->Clear();
    }

    m_depthTextureView.Reset();
    m_depthTexture.Reset();
    m_depthWidth = 0;
    m_depthHeight = 0;
    m_depthAccessSnapshot = MakeRHITextureAccessSnapshot(
        RHIResourceState::Undefined,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics,
        RHIContentValidity::Invalid);

    m_backBufferAccessSnapshots.clear();
    m_depthGraphHandle = {};
    m_backBufferGraphHandle = {};
    m_activeBackBufferIndex = RVX_INVALID_INDEX;
    m_lastSwapChainWidth = 0;
    m_lastSwapChainHeight = 0;
    m_viewData.colorTarget = {};
    m_viewData.depthTarget = {};
    m_viewData.velocityTarget = {};

    RequestTemporalHistoryReset();
}

void SceneRenderer::SetExternalRenderTarget(const SceneRendererExternalTargetDesc& desc)
{
    const bool targetChanged =
        m_externalRenderTarget.colorTarget != desc.colorTarget ||
        m_externalRenderTarget.depthTarget != desc.depthTarget ||
        m_externalRenderTarget.colorInitialState != desc.colorInitialState ||
        m_externalRenderTarget.colorFinalState != desc.colorFinalState ||
        m_externalRenderTarget.depthInitialState != desc.depthInitialState ||
        m_externalRenderTarget.depthFinalState != desc.depthFinalState ||
        (m_externalRenderTargetStats.width != 0 &&
         desc.colorTarget &&
         (m_externalRenderTargetStats.width != desc.colorTarget->GetWidth() ||
          m_externalRenderTargetStats.height != desc.colorTarget->GetHeight() ||
          m_externalRenderTargetStats.colorFormat != desc.colorTarget->GetFormat())) ||
        (m_externalRenderTargetStats.depthFormat != RHIFormat::Unknown &&
         desc.depthTarget &&
         m_externalRenderTargetStats.depthFormat != desc.depthTarget->GetFormat());

    m_externalRenderTarget = desc;
    if (targetChanged)
    {
        InvalidateRenderFramePlan();
        RequestTemporalHistoryReset();
    }
}

void SceneRenderer::ClearExternalRenderTarget()
{
    InvalidateRenderFramePlan();
    m_externalRenderTarget = {};
    m_externalRenderTargetStats = {};
    RequestTemporalHistoryReset();
}

void SceneRenderer::SetGPUDrivenCullingMode(RenderGPUDrivenMode mode)
{
    m_gpuDrivenCullingMode = mode;
    InvalidateRenderFramePlan();
    m_gpuDrivenCullingEnabled = false;
}

void SceneRenderer::SetGPUDrivenCullingEnabled(bool enabled)
{
    SetGPUDrivenCullingMode(enabled
                                ? RenderGPUDrivenMode::ForceEnabled
                                : RenderGPUDrivenMode::ForceDisabled);
}

void SceneRenderer::RefreshFrameDiagnostics(bool renderAttempted,
                                            bool rendered,
                                            bool graphBuilt,
                                            bool graphCompiled,
                                            const char* skippedReason)
{
    SceneRendererFrameDiagnostics diagnostics;
    diagnostics.frameCount = ++m_frameDiagnosticsCounter;
    diagnostics.renderAttempted = renderAttempted;
    diagnostics.rendered = rendered;
    diagnostics.graphBuilt = graphBuilt;
    diagnostics.graphCompiled = graphCompiled;
    diagnostics.graphExecutionSkipped = renderAttempted && !rendered;
    diagnostics.skippedReason = skippedReason ? skippedReason : "";

    diagnostics.renderSceneObjectCount = m_renderScene.GetObjectCount();
    diagnostics.renderSceneLightCount = m_renderScene.GetLightCount();
    diagnostics.visibleObjectCount = m_visibleObjectIndices.size();
    diagnostics.opaqueDrawItemCount = m_opaqueDrawItems.size();
    diagnostics.maskedDrawItemCount = m_maskedDrawItems.size();
    diagnostics.transparentDrawItemCount = m_transparentDrawItems.size();
    diagnostics.featureExtractionStats = m_featureExtractionStats;
    diagnostics.pointLightCount = m_localLightingStats.pointLightCount;
    diagnostics.spotLightCount = m_localLightingStats.spotLightCount;
    diagnostics.lightConstantsBufferReady = m_localLightingStats.lightConstantsBufferReady;
    diagnostics.pointLightsBufferReady = m_localLightingStats.pointLightsBufferReady;
    diagnostics.spotLightsBufferReady = m_localLightingStats.spotLightsBufferReady;
    if (m_pipelineCache)
    {
        const FrameLightBindingResult& frameLightBinding =
            m_pipelineCache->GetLastFrameLightBindingResult();
        m_localLightingStats.frameLightResourcesBound = frameLightBinding.lightResourcesBound;
        m_localLightingStats.frameLightFallbackReason = frameLightBinding.fallbackReason;
    }
    diagnostics.frameLightResourcesBound = m_localLightingStats.frameLightResourcesBound;
    diagnostics.frameLightFallbackReason = m_localLightingStats.frameLightFallbackReason;
    diagnostics.pointShadowRequestCount = m_localLightingStats.pointShadowRequestCount;
    diagnostics.spotShadowRequestCount = m_localLightingStats.spotShadowRequestCount;
    diagnostics.localShadowRequestCount = m_localLightingStats.localShadowRequestCount;
    diagnostics.localShadowAtlasReady = m_localLightingStats.localShadowAtlasReady;
    diagnostics.localShadowFallbackReason = m_localLightingStats.localShadowFallbackReason;
    diagnostics.clusteredLightingInitialized = m_clusteredLightingStats.initialized;
    diagnostics.clusteredLightingFrameBegun = m_clusteredLightingStats.frameBegun;
    diagnostics.clusteredLightingLightsAssigned = m_clusteredLightingStats.lightsAssigned;
    diagnostics.clusteredLightingGpuBuffersUploaded = m_clusteredLightingStats.gpuBuffersUploaded;
    diagnostics.clusteredLightingClusterAABBBufferReady = m_clusteredLightingStats.clusterAABBBufferReady;
    diagnostics.clusteredLightingClusterBufferReady = m_clusteredLightingStats.clusterBufferReady;
    diagnostics.clusteredLightingLightIndexBufferReady = m_clusteredLightingStats.lightIndexBufferReady;
    diagnostics.clusteredLightingConstantsBufferReady = m_clusteredLightingStats.clusterConstantsBufferReady;
    diagnostics.clusteredLightingClusterCount = m_clusteredLightingStats.clusterCount;
    diagnostics.clusteredLightingLightIndexCount = m_clusteredLightingStats.lightIndexCount;
    diagnostics.clusteredLightingActiveClusters = m_clusteredLightingStats.activeClusters;
    diagnostics.clusteredLightingTotalLightAssignments = m_clusteredLightingStats.totalLightAssignments;
    diagnostics.clusteredLightingMaxLightsInCluster = m_clusteredLightingStats.maxLightsInCluster;
    diagnostics.clusteredLightingAvgLightsPerCluster = m_clusteredLightingStats.avgLightsPerCluster;
    diagnostics.clusteredLightingFallbackReason = m_clusteredLightingStats.fallbackReason;

    diagnostics.registeredPassCount = m_passChainStats.registeredPassCount;
    diagnostics.graphPassCount = m_passChainStats.graphPassCount;
    diagnostics.skippedDisabledPassCount = m_passChainStats.skippedDisabledPassCount;
    diagnostics.skippedUnsupportedPassCount = m_passChainStats.skippedUnsupportedPassCount;
    diagnostics.passStatuses = m_passChainStats.passStatuses;
    if (m_renderResourceRegistry)
    {
        diagnostics.gpuResourceStats = m_renderResourceRegistry->GetStats();
    }
    diagnostics.gpuDrivenCullingStats = m_gpuDrivenCullingStats;
    diagnostics.policy = m_renderPolicyDiagnostics;
    diagnostics.rayTracingSceneStats = m_rayTracingSceneStats;

    diagnostics.requestedPostProcessEffectCount = m_postProcessStats.stackStats.requestedEffectCount;
    diagnostics.enabledPostProcessEffectCount = m_postProcessStats.stackStats.enabledEffectCount;
    diagnostics.unsupportedPostProcessSkippedCount = m_postProcessStats.stackStats.unsupportedSkippedCount;
    diagnostics.scheduledPostProcessEffectCount = m_postProcessStats.stackStats.scheduledEffectCount;
    diagnostics.postProcessGraphPassCount = m_postProcessStats.stackStats.graphPassCount;
    diagnostics.requestedVisualQualityPreset = m_postProcessStats.stackStats.requestedQualityPreset;
    diagnostics.appliedVisualQualityPreset = m_postProcessStats.stackStats.appliedQualityPreset;
    diagnostics.hdrSceneColorEnabled = m_postProcessStats.hdrSceneColorEnabled;
    diagnostics.toneMappingOutputColorSpace = m_postProcessStats.toneMappingOutputColorSpace;
    diagnostics.postProcessFinalOutputFormat = m_postProcessStats.stackStats.finalOutputFormat;
    diagnostics.hdrFallbackReason = m_postProcessStats.hdrFallbackReason;
    diagnostics.postProcessToneMappingBoundaryValid = m_postProcessStats.stackStats.toneMappingBoundaryValid;
    diagnostics.postProcessFallbackCopyApplied = m_postProcessStats.stackStats.fallbackCopyApplied;
    diagnostics.postProcessFallbackCopyPassCount = m_postProcessStats.stackStats.fallbackCopyPassCount;
    diagnostics.postProcessFallbackCopyReason = m_postProcessStats.stackStats.fallbackCopyReason;
    diagnostics.postProcessDepthInputAvailable = m_postProcessStats.frameInputDepthAvailable;
    diagnostics.postProcessVelocityInputAvailable = m_postProcessStats.frameInputVelocityAvailable;
    diagnostics.postProcessTemporalHistoryAvailable = m_postProcessStats.frameInputTemporalHistoryAvailable;
    diagnostics.postProcessToneMappingBoundaryWarning = m_postProcessStats.stackStats.toneMappingBoundaryWarning;
    diagnostics.postProcessEffectPlans = m_postProcessStats.stackStats.effectPlans;

    diagnostics.externalTargetRequested = m_externalRenderTargetStats.requested;
    diagnostics.externalTargetActive = m_externalRenderTargetStats.active;
    diagnostics.externalColorImported = m_externalRenderTargetStats.importedColor;
    diagnostics.externalDepthImported = m_externalRenderTargetStats.importedDepth;
    diagnostics.externalTargetFallbackReason = m_externalRenderTargetStats.fallbackReason;

    if (graphCompiled && m_renderGraph)
    {
        const RenderGraph::CompileStats& stats = m_renderGraph->GetCompileStats();
        diagnostics.graphCompileValid = stats.compileValid;
        diagnostics.renderGraphTotalPasses = stats.totalPasses;
        diagnostics.renderGraphCulledPasses = stats.culledPasses;
        diagnostics.renderGraphBarrierCount = stats.barrierCount;
        diagnostics.renderGraphTextureBarrierCount = stats.textureBarrierCount;
        diagnostics.renderGraphBufferBarrierCount = stats.bufferBarrierCount;
        diagnostics.renderGraphValidationWarningCount = stats.validationWarningCount;
        diagnostics.renderGraphValidationErrorCount = stats.validationErrorCount;
        diagnostics.renderGraphMemorySavingsPercent = stats.GetMemorySavingsPercent();
        diagnostics.graphDiagnostics = m_renderGraph->GetCompileDiagnostics();

        if (!diagnostics.graphCompileValid && diagnostics.skippedReason.empty())
        {
            diagnostics.skippedReason = "RenderGraph compile reported validation errors";
        }
    }

    diagnostics.featureReport = BuildRenderFeatureReport(diagnostics);

    if (!diagnostics.skippedReason.empty())
    {
        diagnostics.graphExecutionSkipped = true;
    }

    m_frameDiagnostics = std::move(diagnostics);

    m_toolDiagnosticsSnapshot = {};
    m_toolDiagnosticsSnapshot.frameDiagnosticsAvailable = true;
    m_toolDiagnosticsSnapshot.frame = m_frameDiagnostics;
    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : m_featureReportDeviceForTesting;
    if (device)
    {
        m_toolDiagnosticsSnapshot.rhiCapabilityReportAvailable = true;
        m_toolDiagnosticsSnapshot.rhiCapabilityReport = device->GetCapabilityReport();
    }
    if (graphBuilt && m_renderGraph)
    {
        m_toolDiagnosticsSnapshot.renderGraphDiagnosticsAvailable = true;
        m_toolDiagnosticsSnapshot.renderGraph = m_renderGraph->GetDiagnostics();
    }
}

RenderFrameApplyResult SceneRenderer::ApplyFramePacket(
    const RenderFramePacket& packet,
    RenderResourceRegistry& registry)
{
    InvalidateRenderFramePlan();
    m_renderResourceRegistry = &registry;
    if (m_depthPrepass)
    {
        m_depthPrepass->SetResourceRegistry(&registry);
    }
    if (m_shadowPass)
    {
        m_shadowPass->SetResourceRegistry(&registry);
    }
    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetResourceRegistry(&registry);
    }
    if (m_opaquePass)
    {
        m_opaquePass->SetResourceRegistry(&registry);
    }
    if (m_transparentPass)
    {
        m_transparentPass->SetResourceRegistry(&registry);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetResourceRegistry(&registry);
    }
    if (m_rayTracedReflectionPass)
    {
        m_rayTracedReflectionPass->SetResourceRegistry(&registry);
    }
    RenderFrameApplyResult result =
        m_renderScene.ApplyFramePacket(packet, registry);
    if (!result.IsApplied())
    {
        return result;
    }

    const bool previousViewValid =
        m_renderScene.GetLastRenderedFrameSequence() != 0;
    const Mat4& previousViewProjection = previousViewValid
                                             ? m_renderScene.GetLastRenderedView()
                                                   .viewProjectionMatrix
                                             : m_renderScene.GetView()
                                                   .viewProjectionMatrix;
    m_viewData.SetupFromSnapshot(m_renderScene.GetView(),
                                 previousViewProjection,
                                 previousViewValid,
                                 result.temporalHistoryReset);

    const RenderFrameSettings& frameSettings = m_renderScene.GetSettings();
    SetGPUDrivenCullingMode(frameSettings.gpuCulling.mode);
    m_postProcessSettings.enableBloom =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableBloom;
    m_postProcessSettings.enableSSAO =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableSSAO;
    m_postProcessSettings.enableSSR =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableSSR;
    m_postProcessSettings.enableTAA =
        frameSettings.postProcess.enabled &&
        frameSettings.postProcess.enableTAA;
    m_postProcessSettings.enableRayTracedReflectionDenoise =
        frameSettings.postProcess.enableRayTracedReflectionDenoise;
    m_postProcessSettings.toneMappingOperator =
        static_cast<ToneMappingOperator>(
            frameSettings.postProcess.toneMappingOperator);
    m_postProcessSettings.exposureMode =
        static_cast<ToneMappingExposureMode>(
            frameSettings.postProcess.exposureMode);
    m_postProcessSettings.exposure = frameSettings.postProcess.exposure;
    m_postProcessSettings.cameraEV100 =
        frameSettings.postProcess.cameraEV100;
    m_postProcessSettings.exposureCompensationEV =
        frameSettings.postProcess.exposureCompensationEV;
    m_postProcessSettings.gamma = frameSettings.postProcess.gamma;
    m_postProcessSettings.bloomThreshold =
        frameSettings.postProcess.bloomThreshold;
    m_postProcessSettings.bloomIntensity =
        frameSettings.postProcess.bloomIntensity;
    m_postProcessSettings.bloomRadius =
        frameSettings.postProcess.bloomRadius;
    m_postProcessSettings.exposure = m_renderScene.GetView().exposure;
    m_postProcessSettings.enableRayTracedReflections =
        frameSettings.rayTracing.enabled &&
        frameSettings.rayTracing.enableReflections;
    ApplyPostProcessSettings(m_postProcessSettings);

    m_shadowPassConfig.shadowMapSize =
        frameSettings.shadows.atlasResolution;
    m_shadowPassConfig.numCascades = frameSettings.shadows.cascadeCount;
    m_shadowPassConfig.cascadeSplitLambda =
        frameSettings.shadows.cascadeSplitLambda;
    m_shadowPassConfig.filterRadiusTexels =
        frameSettings.shadows.filterRadiusTexels;
    m_shadowPassConfig.shadowBias = frameSettings.shadows.shadowBias;
    m_shadowPassConfig.normalBias = frameSettings.shadows.normalBias;
    m_shadowPassConfig.cascadeBlendRatio =
        frameSettings.shadows.cascadeBlendRatio;
    ApplyShadowPassConfig(m_shadowPassConfig);

    GPUCullingConfig gpuCullingConfig = GetGPUDrivenCullingConfig();
    gpuCullingConfig.maxInstances = frameSettings.gpuCulling.maxVisibleObjects;
    gpuCullingConfig.enableOcclusionCulling =
        frameSettings.gpuCulling.enableOcclusionCulling;
    gpuCullingConfig.enableDistanceCulling =
        frameSettings.gpuCulling.enableDistanceCulling;
    gpuCullingConfig.maxDrawDistance =
        frameSettings.gpuCulling.maxDrawDistance;
    SetGPUDrivenCullingConfig(gpuCullingConfig);

    SceneRayTracingBudgetSettings rayTracingBudget =
        GetRayTracingBudgetSettings();
    rayTracingBudget.enabled = frameSettings.rayTracing.budgetEnabled;
    rayTracingBudget.maxRayCount = frameSettings.rayTracing.maxRayCount;
    rayTracingBudget.maxDenoiseTapCount =
        frameSettings.rayTracing.maxDenoiseTapCount;
    rayTracingBudget.maxTrackedResourceBytes =
        frameSettings.rayTracing.maxTrackedResourceBytes;
    rayTracingBudget.maxMeasuredGpuMs =
        frameSettings.rayTracing.maxMeasuredGpuMs;
    rayTracingBudget.maxShadowMeasuredGpuMs =
        frameSettings.rayTracing.maxShadowMeasuredGpuMs;
    rayTracingBudget.maxReflectionMeasuredGpuMs =
        frameSettings.rayTracing.maxReflectionMeasuredGpuMs;
    rayTracingBudget.gpuTimingAdjustmentFrameCount =
        frameSettings.rayTracing.gpuTimingAdjustmentFrameCount;
    ApplyRayTracingBudgetSettings(rayTracingBudget);

    m_featureSnapshot = m_renderScene.GetFeatures();
    const RenderFeatureSnapshotMetadata featureMetadata =
        m_featureSnapshot.GetMetadata();
    m_featureExtractionStats = {};
    m_featureExtractionStats.attempted = true;
    m_featureExtractionStats.usedProviderPath = true;
    m_featureExtractionStats.requiresLegacyFallback = false;
    m_featureExtractionStats.snapshotSchemaVersion =
        featureMetadata.schemaVersion;
    m_featureExtractionStats.snapshotSequence = featureMetadata.sequence;
    m_featureExtractionStats.snapshotComplete = featureMetadata.complete;
    m_featureExtractionStats.providerCount = featureMetadata.providerCount;
    m_featureExtractionStats.skippedProviderCount =
        featureMetadata.skippedProviderCount;
    m_featureExtractionStats.particleItemCount =
        featureMetadata.particleItemCount;
    m_featureExtractionStats.waterItemCount =
        featureMetadata.waterItemCount;
    m_featureExtractionStats.terrainItemCount =
        featureMetadata.terrainItemCount;
    for (const ParticleRenderSnapshotItem& item :
         m_featureSnapshot.particles.items)
    {
        if (item.payloadStatus ==
            ParticleRenderSnapshotPayloadStatus::MetadataOnly)
        {
            ++m_featureExtractionStats.particleMetadataOnlyCount;
        }
        if (item.renderPayloadAvailable ||
            item.payloadStatus ==
                ParticleRenderSnapshotPayloadStatus::RenderOwnedPayloadReady)
        {
            ++m_featureExtractionStats.particleRenderPayloadReadyCount;
        }
        if (item.sortingSupported)
        {
            ++m_featureExtractionStats.particleSortingSupportedCount;
        }
    }
    if (m_particleFeaturePass)
    {
        m_particleFeaturePass->SetSnapshot(&m_featureSnapshot.particles);
    }

    if (m_skyboxPass)
    {
        const RenderSkySnapshot& sky = m_renderScene.GetSky();
        RHITexture* texture = sky.skyTexture.IsValid()
                                  ? registry.ResolveTextureObject(
                                        sky.skyTexture)
                                  : nullptr;
        if (texture != nullptr)
        {
            m_skyboxPass->SetCubemap(texture,
                                     sky.intensity,
                                     sky.rotationRadians,
                                     0.0f);
        }
        else
        {
            m_skyboxPass->SetSolidColor(sky.tint, sky.intensity);
        }
    }

    const RenderEnvironmentSnapshot& environment =
        m_renderScene.GetEnvironment();
    const bool textureIBLReady =
        environment.irradianceTexture.IsValid() &&
        environment.prefilteredTexture.IsValid() &&
        environment.brdfLutTexture.IsValid();
    ++m_environmentIBLStats.frameCount;
    m_environmentIBLStats.skyboxFound =
        m_renderScene.GetSky().skyTexture.IsValid();
    m_environmentIBLStats.uploadRequested = false;
    m_environmentIBLStats.textureIBLEnabled = textureIBLReady;
    m_environmentIBLStats.prefilteredMipLevels = 1;
    m_environmentIBLStats.intensity = environment.intensity;
    m_environmentIBLStats.fallbackReason =
        textureIBLReady ? std::string{}
                        : "PacketEnvironmentResourcesUnavailable";
    if (textureIBLReady)
    {
        if (RHITexture* prefiltered = registry.ResolveTextureObject(
                environment.prefilteredTexture))
        {
            m_environmentIBLStats.prefilteredMipLevels =
                std::max(1U, prefiltered->GetMipLevels());
        }
    }
    if (m_materialSystem)
    {
        m_materialSystem->SetEnvironmentIBLResources(
            environment.irradianceTexture,
            environment.prefilteredTexture,
            environment.brdfLutTexture,
            environment.intensity);
    }
    m_viewData.textureIBLEnabled = textureIBLReady ? 1 : 0;
    m_viewData.textureIBLIntensity = environment.intensity;
    m_viewData.ambientFloorIntensity = textureIBLReady ? 0.0f : 0.08f;

    BuildMaterialDrawLists();
    return result;
}

RenderFrameExecutionResult SceneRenderer::RenderAcceptedFrame()
{
    RenderFrameExecutionResult result;
    if (!m_renderScene.HasAcceptedFrame())
    {
        return result;
    }
    RVX_ASSERT_MSG(!m_submissionBatch,
                   "Previous frame submission ownership was not resolved");
    m_submissionBatch = std::make_unique<RenderSubmissionResourceBatch>();
    m_viewData.submissionResourceBatch = m_submissionBatch.get();
    result.frameSequence = m_renderScene.GetAcceptedHeader().sequence;
    result.referencedResources = m_renderScene.GetReferencedResources();
    Render();
    if ((m_depthGPUCulling &&
         !m_depthGPUCulling->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_opaqueGPUCulling &&
         !m_opaqueGPUCulling->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_depthGPUCullingRecordedState &&
         !m_depthGPUCullingRecordedState->RetainSubmissionResources(*m_submissionBatch)) ||
        (m_opaqueGPUCullingRecordedState &&
         !m_opaqueGPUCullingRecordedState->RetainSubmissionResources(*m_submissionBatch)))
    {
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    if (!m_frameDiagnostics.rendered ||
        !m_frameDiagnostics.graphCompileValid)
    {
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    if (!m_renderGraph->RetainSubmissionResources(*m_submissionBatch))
    {
        result.code = RenderFrameExecutionCode::SubmissionFailed;
        return result;
    }
    result.code = RenderFrameExecutionCode::Rendered;
    return result;
}

void SceneRenderer::NotifySubmission(const GPUCompletionToken& completion)
{
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->NotifySubmission(
            m_activeRenderPassIdentity, completion);
    }
    RetireOwnerSnapshots(completion);
    if (m_transientResourcePool)
    {
        m_transientResourcePool->NotifySubmission(completion);
    }
    if (m_resourceViewCache)
    {
        m_resourceViewCache->NotifySubmission(completion);
    }
    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Submission ownership requires retirement queue");
        m_submissionBatch->SealAndTransfer(completion, *m_retirementQueue);
        m_submissionBatch.reset();
        m_viewData.submissionResourceBatch = nullptr;
    }
}

void SceneRenderer::ReleaseUnsubmittedFrame()
{
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->ReleaseUnsubmittedFrame(
            m_activeRenderPassIdentity);
    }
    if (m_renderContext)
    {
        if (RenderSubmissionTracker* tracker =
                RenderContextInternalAccess::GetSubmissionTracker(
                    *m_renderContext))
        {
            RetireOwnerSnapshots(tracker->CaptureLastSubmittedToken());
        }
    }
    const GPUCompletionToken emptyCompletion;
    if (m_transientResourcePool)
    {
        m_transientResourcePool->NotifySubmission(emptyCompletion);
    }
    if (m_resourceViewCache)
    {
        m_resourceViewCache->NotifySubmission(emptyCompletion);
    }
    if (m_submissionBatch)
    {
        RVX_ASSERT_MSG(m_retirementQueue != nullptr,
                       "Unsubmitted ownership requires retirement queue");
        m_submissionBatch->ReleaseUnsubmitted(*m_retirementQueue);
        m_submissionBatch.reset();
        m_viewData.submissionResourceBatch = nullptr;
    }
}

void SceneRenderer::RetireOwnerSnapshots(
    const GPUCompletionToken& completion)
{
    if (!m_retirementQueue)
    {
        return;
    }
    if (m_depthGPUCulling)
    {
        m_depthGPUCulling->RetireOwnerSnapshots(completion, *m_retirementQueue);
    }
    if (m_opaqueGPUCulling)
    {
        m_opaqueGPUCulling->RetireOwnerSnapshots(completion, *m_retirementQueue);
    }
    if (m_pipelineCache)
    {
        m_pipelineCache->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_materialSystem)
    {
        m_materialSystem->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_rayTracingSceneManager)
    {
        m_rayTracingSceneManager->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
    if (m_rayTracedReflectionPass)
    {
        m_rayTracedReflectionPass->RetireOwnerSnapshots(
            completion, *m_retirementQueue);
    }
}

void SceneRenderer::MarkAcceptedFramePresented()
{
    m_renderScene.MarkAcceptedFrameRendered();
}

void SceneRenderer::SetSurfaceCompatibilityKey(uint64 key) noexcept
{
    m_renderScene.SetSurfaceCompatibilityKey(key);
}

void SceneRenderer::ResolveRenderTargetExtent(uint32& width, uint32& height) const
{
    width = 1280;
    height = 720;

    if (m_externalRenderTarget.IsValid())
    {
        width = m_externalRenderTarget.colorTarget->GetWidth();
        height = m_externalRenderTarget.colorTarget->GetHeight();
    }
    else if (m_renderContext && m_renderContext->GetSwapChain())
    {
        width = m_renderContext->GetSwapChain()->GetWidth();
        height = m_renderContext->GetSwapChain()->GetHeight();
    }
}

void SceneRenderer::BuildMaterialDrawLists()
{
    // Direct/Transparent/legacy consumers receive the CPU-final list. GPU
    // candidates deliberately start from the coarser visible+drawable list so
    // the GPU lane never inherits a CPU frustum pre-elimination.
    m_renderCandidates.Clear();
    for (uint32 objectIndex = 0;
         objectIndex < static_cast<uint32>(m_renderScene.GetObjectCount());
         ++objectIndex)
    {
        const RenderObject& object = m_renderScene.GetObject(objectIndex);
        RenderVisibilityCandidate candidate;
        candidate.objectIndex = objectIndex;
        candidate.objectVisible = object.visible;
        candidate.drawable = object.drawable;
        candidate.worldBounds = object.bounds;
        (void)m_renderCandidates.Add(candidate);
    }

    const GPUCullingConfig initialVisibilityConfig =
        GetGPUDrivenCullingConfig();
    RenderVisibilityRequest visibilityRequest;
    visibilityRequest.viewProjection =
        m_renderScene.GetView().viewProjectionMatrix;
    visibilityRequest.cameraPosition =
        m_renderScene.GetView().cameraPosition;
    visibilityRequest.enableDistanceCulling =
        initialVisibilityConfig.enableDistanceCulling;
    visibilityRequest.maxDrawDistance =
        initialVisibilityConfig.maxDrawDistance;
    GPUVisibilityProvider visibilityProvider;
    visibilityProvider.Evaluate(m_renderCandidates,
                                visibilityRequest,
                                m_renderVisibility);
    m_visibleObjectIndices = m_renderVisibility.cpuVisibleObjectIndices;
    m_coarseCandidateObjectIndices =
        m_renderVisibility.coarseVisibleObjectIndices;
    m_renderScene.SortVisibleObjects(m_visibleObjectIndices,
                                     m_renderScene.GetView().cameraPosition);
    m_renderScene.SortVisibleObjects(m_coarseCandidateObjectIndices,
                                     m_renderScene.GetView().cameraPosition);

    RVX::BuildMaterialDrawLists(m_renderScene,
                                m_visibleObjectIndices,
                                m_viewData.cameraPosition,
                                m_opaqueDrawItems,
                                m_maskedDrawItems,
                                m_transparentDrawItems);

    std::vector<RenderDrawItem> ignoredTransparentCandidates;
    RVX::BuildMaterialDrawLists(m_renderScene,
                                m_coarseCandidateObjectIndices,
                                m_viewData.cameraPosition,
                                m_coarseOpaqueDrawItems,
                                m_coarseMaskedDrawItems,
                                ignoredTransparentCandidates);

    PrepareMeshPassPackets();

    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetRenderScene(&m_renderScene, &m_opaqueDrawItems, &m_maskedDrawItems);
    }
}

void SceneRenderer::PrepareMeshPassPackets()
{
    m_meshPassPreparation.Clear();

    DepthMeshPassProcessor depthProcessor;
    OpaqueMeshPassProcessor opaqueProcessor;
    TransparentMeshPassProcessor transparentProcessor;
    ShadowMeshPassProcessor shadowProcessor;

    uint32 materialSourceOrdinal = 0;
    const auto prepareMaterialItems =
        [this, &depthProcessor, &opaqueProcessor, &materialSourceOrdinal](
            const std::vector<RenderDrawItem>& drawItems)
    {
        for (const RenderDrawItem& item : drawItems)
        {
            MeshPassProcessorInput input = MakeMeshPassProcessorInput(
                item.packet,
                m_renderResourceRegistry,
                materialSourceOrdinal++,
                item.depthFromCamera);
            m_meshPassPreparation.depth.Record(depthProcessor.Process(input));
            m_meshPassPreparation.opaque.Record(opaqueProcessor.Process(input));
        }
    };
    prepareMaterialItems(m_coarseOpaqueDrawItems);
    prepareMaterialItems(m_coarseMaskedDrawItems);

    for (uint32 sourceOrdinal = 0;
         sourceOrdinal < static_cast<uint32>(m_transparentDrawItems.size());
         ++sourceOrdinal)
    {
        const RenderDrawItem& item = m_transparentDrawItems[sourceOrdinal];
        const MeshPassProcessorInput input = MakeMeshPassProcessorInput(
            item.packet,
            m_renderResourceRegistry,
            sourceOrdinal,
            item.depthFromCamera);
        m_meshPassPreparation.transparent.Record(
            transparentProcessor.Process(input));
    }

    uint32 shadowSourceOrdinal = 0;
    for (uint32 objectIndex = 0;
         objectIndex < static_cast<uint32>(m_renderScene.GetObjectCount());
         ++objectIndex)
    {
        const RenderObject& object = m_renderScene.GetObject(objectIndex);
        if (object.meshBatchesAuthoritative)
        {
            for (const MeshBatch& batch : object.meshBatches)
            {
                const RenderDrawPacket packet = BuildShadowPacket(
                    m_renderScene, object, batch);
                const MeshPassProcessorInput input =
                    MakeMeshPassProcessorInput(
                        packet,
                        m_renderResourceRegistry,
                        shadowSourceOrdinal++,
                        0.0f);
                m_meshPassPreparation.shadow.Record(
                    shadowProcessor.Process(input));
            }
            continue;
        }

        const MeshGPUBuffers buffers = ResolveRenderMeshBuffers(
            m_renderResourceRegistry, object.mesh);
        const uint32 submeshCount = buffers.submeshes.empty()
                                        ? 1U
                                        : static_cast<uint32>(
                                              buffers.submeshes.size());
        for (uint32 submeshIndex = 0;
             submeshIndex < submeshCount;
             ++submeshIndex)
        {
            const SubmeshGPUInfo* submesh =
                submeshIndex < buffers.submeshes.size()
                    ? &buffers.submeshes[submeshIndex]
                    : nullptr;
            const MeshBatch batch = BuildLegacyShadowBatch(
                object, objectIndex, submeshIndex, submesh);
            const MeshPassProcessorInput input = MakeMeshPassProcessorInput(
               BuildLegacyMaterialDrawPacket(batch),
               m_renderResourceRegistry,
               shadowSourceOrdinal++,
                0.0f);
            m_meshPassPreparation.shadow.Record(
                shadowProcessor.Process(input));
        }
    }

    m_meshPassPreparation.depth.FinalizeGroups();
    m_meshPassPreparation.opaque.FinalizeGroups();
    m_meshPassPreparation.transparent.FinalizeGroups();
    m_meshPassPreparation.shadow.FinalizeGroups();

    const uint32 firstPassVisibilityCandidate = static_cast<uint32>(
        m_renderCandidates.GetCandidates().size());
    const auto appendVisibilityCandidates = [this](
                                                RenderPassKind pass,
                                                const MeshPassPacketStream& stream)
    {
        for (uint32 sourcePacketIndex = 0;
             sourcePacketIndex < static_cast<uint32>(stream.packets.size());
             ++sourcePacketIndex)
        {
            const MeshPassProcessorResult& source =
                stream.packets[sourcePacketIndex];
            const RenderDrawPacket& packet = source.packet;
            if (packet.primitiveData == RVX_INVALID_PRIMITIVE_DATA_INDEX ||
                packet.primitiveData >= m_renderScene.GetObjectCount())
            {
                continue;
            }
            const RenderObject& object =
                m_renderScene.GetObject(packet.primitiveData);
            if (packet.objectId == 0 || object.entityId != packet.objectId ||
                object.mesh != packet.geometryKey.mesh)
            {
                continue;
            }
            RenderVisibilityCandidate candidate;
            candidate.sourcePacketIndex = sourcePacketIndex;
            candidate.sourceOrdinal = source.sourceOrdinal;
            candidate.objectIndex = packet.primitiveData;
            candidate.pass = pass;
            candidate.objectVisible = object.visible;
            candidate.drawable = object.drawable;
            candidate.worldBounds = object.bounds;
            (void)m_renderCandidates.Add(candidate);
        }
    };
    appendVisibilityCandidates(RenderPassKind::Depth,
                               m_meshPassPreparation.depth);
    appendVisibilityCandidates(RenderPassKind::Opaque,
                               m_meshPassPreparation.opaque);
    appendVisibilityCandidates(RenderPassKind::Transparent,
                               m_meshPassPreparation.transparent);
    // Shadow/light views are intentionally not governed by camera visibility.
    GPUVisibilityProvider visibilityProvider;
    visibilityProvider.AppendPassCandidates(m_renderCandidates,
                                            firstPassVisibilityCandidate,
                                            m_renderVisibility);
    m_renderVisibility.diagnostics.occlusionRequested =
        (m_depthGPUCulling && m_depthGPUCulling->WasOcclusionRequested()) ||
        (m_opaqueGPUCulling && m_opaqueGPUCulling->WasOcclusionRequested());
    m_renderVisibility.diagnostics.occlusionAvailable = false;
    m_viewData.renderVisibility = &m_renderVisibility;

    const auto validateStream = [](const MeshPassPacketStream& stream)
    {
        RVX_DEBUG_ASSERT(stream.stats.HasCompleteRelevantOutcome());
        RVX_DEBUG_ASSERT(
            stream.stats.HasExactlyOneReasonPerRejectedPacket());
    };
    validateStream(m_meshPassPreparation.depth);
    validateStream(m_meshPassPreparation.opaque);
    validateStream(m_meshPassPreparation.transparent);
    validateStream(m_meshPassPreparation.shadow);
}

void SceneRenderer::InvalidateRenderFramePlan()
{
    m_renderPolicyDiagnostics = {};
    m_viewData.renderFrameExecutionPlan = nullptr;
    m_viewData.meshPassPreparation = nullptr;
    m_viewData.renderFrameExecutionReport = nullptr;
    m_viewData.renderVisibility = nullptr;
    m_frameDiagnostics.policy = {};
    m_gpuDrivenCullingEnabled = false;
    m_gpuDrivenPolicyDecision = {};
    m_gpuDrivenPolicyDecision.requestedMode = m_gpuDrivenCullingMode;
}

void SceneRenderer::CompileRenderFramePlan()
{
    m_renderPolicyDiagnostics.planAvailable = false;
    m_renderPolicyDiagnostics.reportAvailable = false;
    m_renderPolicyDiagnostics.selectedPlan = {};
    m_renderPolicyDiagnostics.executionReport = {};
    m_viewData.renderFrameExecutionPlan = nullptr;
    m_viewData.meshPassPreparation = nullptr;
    m_viewData.renderFrameExecutionReport = nullptr;

    RenderPolicyResolverInput input;
    input.request.frameSequence = m_renderScene.GetAcceptedHeader().sequence;
    input.request.gpuDrivenMode = m_gpuDrivenCullingMode;
    input.viewOrdinal = 0;
    m_renderPolicyDiagnostics.requestAvailable = true;
    m_renderPolicyDiagnostics.request = input.request;

    const IRHIDevice* device = m_renderContext
        ? m_renderContext->GetDevice()
        : nullptr;
    if (device)
    {
        const RHICapabilities& capabilities = device->GetCapabilities();
        input.capabilities.backend = device->GetBackendType();
        input.capabilities.supportsComputeVisibility =
            capabilities.supportsComputePipeline;
        input.capabilities.supportsIndirectDrawCount =
            capabilities.supportsIndirectDrawCount;
        input.capabilities.supportsDescriptorResourceBindings =
            capabilities.supportsDescriptorSets;
    }
    input.qualification = GetGPUDrivenBackendQualification(
        input.capabilities.backend);

    const GPUCullingExecutionDecision depthGPUExecution = m_depthGPUCulling
        ? m_depthGPUCulling->GetExecutionDecision()
        : GPUCullingExecutionDecision{};
    const GPUCullingExecutionDecision opaqueGPUExecution = m_opaqueGPUCulling
        ? m_opaqueGPUCulling->GetExecutionDecision()
        : GPUCullingExecutionDecision{};

    bool depthGPUPipelineReady = false;
    bool opaqueGPUPipelineReady = false;
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        depthGPUPipelineReady =
            m_pipelineCache->GetGPUDrivenDepthOnlyPipeline() != nullptr;

        RHIFormat backBufferFormat = RHIFormat::Unknown;
        if (m_externalRenderTarget.IsValid())
        {
            backBufferFormat =
                m_externalRenderTarget.colorTarget->GetFormat();
        }
        else if (m_renderContext && m_renderContext->GetCurrentBackBuffer())
        {
            backBufferFormat =
                m_renderContext->GetCurrentBackBuffer()->GetFormat();
        }
        const PostProcessStackExecuteStats stackStats = m_postProcessStack
            ? m_postProcessStack->EvaluateEffects()
            : PostProcessStackExecuteStats{};
        const bool postProcessActive =
            stackStats.enabledEffectCount > 0 &&
            stackStats.toneMappingBoundaryValid;
        const SceneColorFormatPolicy formatPolicy =
            ResolveSceneColorFormatPolicy(backBufferFormat,
                                          postProcessActive);
        opaqueGPUPipelineReady =
            formatPolicy.actualSceneColorFormat != RHIFormat::Unknown;
        for (const RenderDrawGroupRange& group :
             m_meshPassPreparation.opaque.groups)
        {
            opaqueGPUPipelineReady = opaqueGPUPipelineReady &&
                m_pipelineCache->GetGPUDrivenPipelineForVariant(
                    group.key.pipeline.materialVariant,
                    formatPolicy.actualSceneColorFormat) != nullptr;
        }
    }
    input.view.rendererAllowsGPUDriven = true;
    input.view.viewAllowsGPUDriven = true;
    input.view.implementationAvailable =
        (m_depthGPUCulling &&
         IsGPUImplementationAvailable(depthGPUExecution)) ||
        (m_opaqueGPUCulling &&
         IsGPUImplementationAvailable(opaqueGPUExecution));
    const GPUCullingConfig cullingConfig = GetGPUDrivenCullingConfig();
    input.view.requestedVisibility = cullingConfig.enableDistanceCulling
        ? RenderVisibilityMode::GpuFrustumAndDistance
        : RenderVisibilityMode::GpuFrustum;
    input.view.visibilityShaderReadiness = ResolveAnyGPUOwnerReadiness(
        m_depthGPUCulling.get(), m_opaqueGPUCulling.get(),
        [](const GPUCullingExecutionDecision& decision)
        {
            return ResolveGPUShaderReadiness(decision);
        });
    input.view.visibilityPipelineReadiness = ResolveAnyGPUOwnerReadiness(
        m_depthGPUCulling.get(), m_opaqueGPUCulling.get(),
        [](const GPUCullingExecutionDecision& decision)
        {
            return decision.pipelineReady
                ? RenderPolicyReadiness::Ready
                : RenderPolicyReadiness::Unavailable;
        });
    input.view.sharedResourceReadiness = ResolveAnyGPUOwnerReadiness(
        m_depthGPUCulling.get(), m_opaqueGPUCulling.get(),
        [](const GPUCullingExecutionDecision& decision)
        {
            return ResolveGPUResourceReadiness(decision);
        });
    input.view.requiredBindingReadiness = ResolveAnyGPUOwnerReadiness(
        m_depthGPUCulling.get(), m_opaqueGPUCulling.get(),
        [](const GPUCullingExecutionDecision& decision)
        {
            return ResolveGPUBindingReadiness(decision);
        });

    input.passes = {
        MakeRenderPassPolicyFacts(RenderPassKind::Depth,
                                  m_depthPrepass,
                                  m_meshPassPreparation.depth,
                                  depthGPUExecution,
                                  depthGPUPipelineReady),
        MakeRenderPassPolicyFacts(RenderPassKind::Opaque,
                                  m_opaquePass,
                                  m_meshPassPreparation.opaque,
                                  opaqueGPUExecution,
                                  opaqueGPUPipelineReady),
        MakeRenderPassPolicyFacts(RenderPassKind::Shadow,
                                  m_shadowPass,
                                  m_meshPassPreparation.shadow,
                                  GPUCullingExecutionDecision{},
                                  false),
        MakeRenderPassPolicyFacts(RenderPassKind::Transparent,
                                  m_transparentPass,
                                  m_meshPassPreparation.transparent,
                                  GPUCullingExecutionDecision{},
                                  false),
    };

    const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
    const RenderFramePlanCompileResult compiled =
        CompileRenderFrameExecutionPlan(resolution, m_meshPassPreparation);
    if (compiled.succeeded)
    {
        m_renderPolicyDiagnostics.planAvailable = true;
        m_renderPolicyDiagnostics.selectedPlan = compiled.plan;
        m_viewData.renderFrameExecutionPlan =
            &m_renderPolicyDiagnostics.selectedPlan;
        m_viewData.meshPassPreparation = &m_meshPassPreparation;
        m_viewData.renderFrameExecutionReport =
            &m_renderPolicyDiagnostics.executionReport;
        m_renderPolicyDiagnostics.executionReport.frameSequence =
            compiled.plan.frameSequence;
        for (const RenderPassExecutionPlan& passPlan : compiled.plan.passes)
        {
            RenderPassExecutionReport report;
            report.pass = passPlan.pass;
            report.executedVisibility = passPlan.visibility;
            report.gpuDrivenLane.packetRange = passPlan.gpuEligiblePackets;
            report.gpuDrivenLane.submission = passPlan.preferredSubmission;
            report.directLane.packetRange = passPlan.directPackets;
            report.directLane.submission = passPlan.fallbackSubmission;
            report.skippedPacketCount = passPlan.partition.skippedPacketCount;
            report.reason = passPlan.reason;
            m_renderPolicyDiagnostics.executionReport.passes.push_back(report);
        }
    }
    ApplyRenderFramePlanProjection();
}

void SceneRenderer::ApplyRenderFramePlanProjection()
{
    bool depthGPU = false;
    bool opaqueGPU = false;
    m_gpuDrivenPolicyDecision = {};
    m_gpuDrivenPolicyDecision.requestedMode = m_gpuDrivenCullingMode;

    if (m_renderPolicyDiagnostics.planAvailable)
    {
        const RenderFrameExecutionPlan& plan =
            m_renderPolicyDiagnostics.selectedPlan;
        for (const RenderPassExecutionPlan& pass : plan.passes)
        {
            const bool gpu = pass.partition.gpuDrivenPacketCount != 0;
            depthGPU |= pass.pass == RenderPassKind::Depth && gpu;
            opaqueGPU |= pass.pass == RenderPassKind::Opaque && gpu;
        }
        m_gpuDrivenPolicyDecision.reason =
            ProjectLegacyPolicyReason(plan.viewPolicy.reason);
        m_gpuDrivenPolicyDecision.qualificationLevel =
            plan.qualification.level;
        m_gpuDrivenPolicyDecision.qualificationRevision =
            plan.qualification.revision;
        m_gpuDrivenPolicyDecision.passedQualificationGateMask =
            plan.qualification.passedGateMask;
        m_gpuDrivenPolicyDecision.requiredQualificationGateMask =
            plan.qualification.requiredGateMask;
        m_gpuDrivenPolicyDecision.missingQualificationGateMask =
            plan.qualification.requiredGateMask &
            ~plan.qualification.passedGateMask;
        m_gpuDrivenPolicyDecision.backendQualified =
            plan.qualification.level ==
            GPUDrivenQualificationLevel::Qualified;
        m_gpuDrivenPolicyDecision.capabilitiesReady =
            plan.capabilities.supportsComputeVisibility &&
            plan.capabilities.supportsDescriptorResourceBindings &&
            plan.capabilities.supportsIndirectDrawCount;
        m_gpuDrivenPolicyDecision.pipelineReady = depthGPU || opaqueGPU;
    }

    m_gpuDrivenCullingEnabled = depthGPU || opaqueGPU;
    m_gpuDrivenPolicyDecision.enabled = m_gpuDrivenCullingEnabled;
}

void SceneRenderer::BuildGPUDrivenVisibilityInputs()
{
    m_depthGPUCullingFramePrepared = false;
    m_opaqueGPUCullingFramePrepared = false;
    m_gpuDrivenCullingStats = {};
    m_gpuDrivenCullingStats.policyDecisionAvailable = true;
    m_gpuDrivenCullingStats.policyDecision = m_gpuDrivenPolicyDecision;
    m_gpuDrivenCullingStats.enabled = m_gpuDrivenCullingEnabled;
    m_gpuDrivenCullingStats.opaqueMeshPassProcessorStats =
        m_meshPassPreparation.opaque.stats;
    m_gpuDrivenCullingStats.visibilityCandidateCount =
        m_renderVisibility.diagnostics.sceneObjectCandidateCount;
    m_gpuDrivenCullingStats.cpuVisibleCandidateCount =
        m_renderVisibility.diagnostics.cpuVisibleObjectCount;
    m_gpuDrivenCullingStats.passVisibilityCandidateCount =
        m_renderVisibility.diagnostics.passCandidateCount;
    m_gpuDrivenCullingStats.invalidVisibilityBoundsCount =
        m_renderVisibility.diagnostics.invalidBoundsCount;
    m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount =
        m_renderVisibility.diagnostics.gpuDeferredCandidateCount;
    m_gpuDrivenCullingStats.gpuVisibilityReadbackPerformed =
        m_renderVisibility.diagnostics.gpuReadbackPerformed;
    m_gpuDrivenCullingStats.occlusionRequestedButUnavailable =
        (m_depthGPUCulling && m_depthGPUCulling->WasOcclusionRequested() &&
         !m_depthGPUCulling->IsOcclusionAvailable()) ||
        (m_opaqueGPUCulling && m_opaqueGPUCulling->WasOcclusionRequested() &&
         !m_opaqueGPUCulling->IsOcclusionAvailable());
    m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount = 0;
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.pass == RenderPassKind::Depth ||
                passPlan.pass == RenderPassKind::Opaque)
            {
                m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount +=
                    passPlan.gpuEligiblePackets.count;
            }
            if (passPlan.pass == RenderPassKind::Opaque)
            {
                for (uint32 offset = 0;
                     offset < passPlan.gpuEligiblePackets.count;
                     ++offset)
                {
                    const RenderDrawPacketReference& reference =
                        m_viewData.renderFrameExecutionPlan->packetReferences[
                            static_cast<size_t>(
                                passPlan.gpuEligiblePackets.first + offset)];
                    if (m_renderVisibility.IsDirectPacketVisible(
                            RenderPassKind::Opaque,
                            reference.sourcePacketIndex))
                    {
                        ++m_gpuDrivenCullingStats
                              .cpuReferenceVisibleCullableDrawItemCount;
                    }
                    else
                    {
                        ++m_gpuDrivenCullingStats
                              .cpuReferenceCulledDrawItemCount;
                    }
                }
            }
        }
    }
    m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount =
        m_gpuDrivenCullingStats.gpuPlannedVisibilityCandidateCount;
    m_renderVisibility.diagnostics.gpuDeferredCandidateCount =
        m_gpuDrivenCullingStats.gpuDeferredVisibilityCandidateCount;
    m_gpuDrivenCullingStats.inputOpaqueDrawItemCount =
        static_cast<uint32>(m_coarseOpaqueDrawItems.size());
    m_gpuDrivenCullingStats.inputMaskedDrawItemCount =
        static_cast<uint32>(m_coarseMaskedDrawItems.size());
    m_gpuDrivenCullingStats.outputOpaqueDrawItemCount =
        static_cast<uint32>(m_opaqueDrawItems.size());
    m_gpuDrivenCullingStats.outputMaskedDrawItemCount =
        static_cast<uint32>(m_maskedDrawItems.size());
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.pass != RenderPassKind::Opaque)
            {
                continue;
            }
            for (uint32 offset = 0;
                 offset < passPlan.gpuEligiblePackets.count;
                 ++offset)
            {
                const size_t referenceIndex = static_cast<size_t>(
                    passPlan.gpuEligiblePackets.first + offset);
                if (referenceIndex >=
                    m_viewData.renderFrameExecutionPlan->packetReferences.size())
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    break;
                }
                const uint32 sourcePacketIndex =
                    m_viewData.renderFrameExecutionPlan
                        ->packetReferences[referenceIndex]
                        .sourcePacketIndex;
                if (sourcePacketIndex >= m_meshPassPreparation.opaque.packets.size())
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    break;
                }
                const MaterialPipelineVariant variant =
                    m_meshPassPreparation.opaque.packets[sourcePacketIndex]
                        .packet.pipelineKey.materialVariant;
                if (variant == MaterialPipelineVariant::Masked)
                {
                    ++m_gpuDrivenCullingStats.cullableMaskedDrawItemCount;
                }
                else
                {
                    ++m_gpuDrivenCullingStats.cullableOpaqueDrawItemCount;
                }
            }
            break;
        }
    }
    const GPUCulling* diagnosticGPUCulling = nullptr;
    if (m_viewData.renderFrameExecutionPlan)
    {
        for (const RenderPassExecutionPlan& passPlan :
             m_viewData.renderFrameExecutionPlan->passes)
        {
            if (passPlan.gpuEligiblePackets.count == 0)
            {
                continue;
            }
            if (passPlan.pass == RenderPassKind::Opaque && m_opaqueGPUCulling)
            {
                diagnosticGPUCulling = m_opaqueGPUCulling.get();
            }
            else if (passPlan.pass == RenderPassKind::Depth &&
                     m_depthGPUCulling && diagnosticGPUCulling == nullptr)
            {
                diagnosticGPUCulling = m_depthGPUCulling.get();
            }
        }
    }
    if (diagnosticGPUCulling)
    {
        m_gpuDrivenCullingStats.executionDecisionAvailable = true;
        m_gpuDrivenCullingStats.executionDecision =
            diagnosticGPUCulling->GetExecutionDecision();
    }

    if (!m_gpuDrivenCullingEnabled ||
        (!m_depthGPUCulling && !m_opaqueGPUCulling) ||
        m_renderResourceRegistry == nullptr)
    {
        return;
    }
    PrepareGPUDrivenGraphCullInputs();
}

void SceneRenderer::PrepareGPUDrivenGraphCullInputs()
{
    if (!m_gpuDrivenCullingEnabled ||
        (!m_depthGPUCulling && !m_opaqueGPUCulling) ||
        m_renderResourceRegistry == nullptr)
    {
        return;
    }

    m_gpuDrivenCullingStats.graphInputDrawItemCount = 0;
    const RenderFrameExecutionPlan* framePlan =
        m_viewData.renderFrameExecutionPlan;
    if (framePlan == nullptr || !m_renderVisibility.structurallyValid)
    {
        return;
    }

    // RenderContext::BeginFrame has already waited this slot's previous
    // submission before RenderAcceptedFrame reaches SceneRenderer. Select the
    // matching per-flight CPU-written inputs before either owner uploads data.
    const uint32 frameSlot = m_renderContext != nullptr
        ? m_renderContext->GetFrameIndex()
        : 0u;
    if ((m_depthGPUCulling && !m_depthGPUCulling->SetFrameSlot(frameSlot)) ||
        (m_opaqueGPUCulling && !m_opaqueGPUCulling->SetFrameSlot(frameSlot)))
    {
        ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
        return;
    }

    const auto preparePass =
        [this, framePlan](RenderPassKind pass,
                          const MeshPassPacketStream& stream,
                          GPUCulling* owner,
                          bool& outPrepared)
    {
        outPrepared = false;
        if (owner == nullptr)
        {
            return;
        }

        const RenderPassExecutionPlan* passPlan = nullptr;
        for (const RenderPassExecutionPlan& candidatePlan : framePlan->passes)
        {
            if (candidatePlan.pass == pass)
            {
                passPlan = &candidatePlan;
                break;
            }
        }
        if (passPlan == nullptr || passPlan->gpuEligiblePackets.count == 0)
        {
            return;
        }
        if (!ValidatePlannedGPUDrivenPacketRange(*framePlan, pass, stream) ||
            passPlan->gpuEligiblePackets.count > owner->GetConfig().maxInstances)
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            return;
        }

        // Complete structural preflight occurs before the per-pass owner is
        // reset. A malformed Depth plan therefore cannot touch Opaque state,
        // and vice versa.
        for (uint32 offset = 0;
             offset < passPlan->gpuEligiblePackets.count;
             ++offset)
        {
            const uint32 sourcePacketIndex =
                stream.sortedGPUCandidatePacketIndices[offset];
            const RenderDrawPacketReference& reference =
                framePlan->packetReferences[static_cast<size_t>(
                    passPlan->gpuEligiblePackets.first + offset)];
            const RenderVisibilityCandidate* visibilityCandidate =
                m_renderCandidates.Find(pass, sourcePacketIndex);
            if (reference.sourcePacketIndex != sourcePacketIndex ||
                visibilityCandidate == nullptr ||
                !visibilityCandidate->objectVisible ||
                !visibilityCandidate->drawable ||
                visibilityCandidate->objectIndex >= m_renderScene.GetObjectCount() ||
                sourcePacketIndex >= stream.packets.size() ||
                stream.packets[sourcePacketIndex].packet.arguments.indexCount == 0)
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                return;
            }
        }

        owner->BeginFrame();
        uint32 plannedOffset = 0;
        for (const RenderDrawGroupRange& group : stream.groups)
        {
            const RenderDrawGroupKey& key = group.key;
            const uint64 meshId =
                (static_cast<uint64>(key.geometry.mesh.slot) << 32U) |
                key.geometry.mesh.generation;
            const uint64 materialId =
                (static_cast<uint64>(key.material.material.slot) << 32U) |
                key.material.material.generation;
            const uint32 groupIndex = owner->BeginDrawGroup(
                meshId,
                materialId,
                key.pipeline.materialVariant,
                key.geometry.mesh,
                key.material.material);
            if (groupIndex == RVX_INVALID_INDEX)
            {
                ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                owner->EndFrame();
                return;
            }

            for (uint32 offset = 0; offset < group.count; ++offset)
            {
                const uint32 sourcePacketIndex =
                    stream.sortedGPUCandidatePacketIndices[group.first + offset];
                const MeshPassProcessorResult& result =
                    stream.packets[sourcePacketIndex];
                const RenderVisibilityCandidate* visibilityCandidate =
                    m_renderCandidates.Find(pass, sourcePacketIndex);
                GPUIndexedDrawDesc drawDesc;
                drawDesc.indexCount = result.packet.arguments.indexCount;
                drawDesc.firstIndex = result.packet.arguments.firstIndex;
                drawDesc.vertexOffset = result.packet.arguments.vertexOffset;
                if (visibilityCandidate == nullptr ||
                    owner->AddVisibilityCandidateInstance(
                        m_renderScene,
                        *visibilityCandidate,
                        result.packet,
                        drawDesc) == RVX_INVALID_INDEX)
                {
                    ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
                    owner->EndDrawGroup();
                    owner->EndFrame();
                    return;
                }
                ++plannedOffset;
            }
            owner->EndDrawGroup();
        }

        if (plannedOffset != passPlan->gpuEligiblePackets.count ||
            owner->GetInstanceCount() != passPlan->gpuEligiblePackets.count)
        {
            ++m_gpuDrivenCullingStats.skippedMissingGpuDataCount;
            owner->EndFrame();
            return;
        }
        owner->EndFrame();
        outPrepared = true;
        m_gpuDrivenCullingStats.graphInputDrawItemCount +=
            owner->GetInstanceCount();
    };

    preparePass(RenderPassKind::Depth,
                m_meshPassPreparation.depth,
                m_depthGPUCulling.get(),
                m_depthGPUCullingFramePrepared);
    preparePass(RenderPassKind::Opaque,
                m_meshPassPreparation.opaque,
                m_opaqueGPUCulling.get(),
                m_opaqueGPUCullingFramePrepared);
}

void SceneRenderer::ApplyObjectMotionHistory()
{
    for (size_t objectIndex = 0; objectIndex < m_renderScene.GetObjectCount(); ++objectIndex)
    {
        RenderObject& object = m_renderScene.GetMutableObject(objectIndex);
        object.previousWorldMatrix = object.worldMatrix;
        object.previousWorldMatrixValid = 0;

        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey == 0 || m_viewData.resetTemporalHistory)
        {
            continue;
        }

        const auto previousIt = m_previousObjectWorldMatrices.find(historyKey);
        if (previousIt == m_previousObjectWorldMatrices.end())
        {
            continue;
        }

        object.previousWorldMatrix = previousIt->second;
        object.previousWorldMatrixValid = 1;
    }
}

void SceneRenderer::UpdateObjectMotionHistory()
{
    std::unordered_map<uint64, Mat4> currentObjectWorldMatrices;
    currentObjectWorldMatrices.reserve(m_renderScene.GetObjectCount());

    for (const RenderObject& object : m_renderScene.GetObjects())
    {
        const uint64 historyKey = GetRenderObjectHistoryKey(object);
        if (historyKey != 0)
        {
            currentObjectWorldMatrices[historyKey] = object.worldMatrix;
        }
    }

    m_previousObjectWorldMatrices = std::move(currentObjectWorldMatrices);
}

const RayTracingSceneManagerStats& SceneRenderer::GetRayTracingSceneStats() const
{
    return m_rayTracingSceneStats;
}

const CameraVelocityPassStats& SceneRenderer::GetCameraVelocityStats() const
{
    static const CameraVelocityPassStats emptyStats;
    return m_cameraVelocityPass ? m_cameraVelocityPass->GetStats() : emptyStats;
}

const ObjectVelocityPassStats& SceneRenderer::GetObjectVelocityStats() const
{
    static const ObjectVelocityPassStats emptyStats;
    return m_objectVelocityPass ? m_objectVelocityPass->GetStats() : emptyStats;
}

const ParticleFeaturePassStats& SceneRenderer::GetParticleFeaturePassStats() const
{
    static const ParticleFeaturePassStats emptyStats;
    return m_particleFeaturePass ? m_particleFeaturePass->GetStats() : emptyStats;
}

RHIAccelerationStructure* SceneRenderer::GetRayTracingTopLevelAS() const
{
    return m_rayTracingSceneManager ? m_rayTracingSceneManager->GetTopLevelAS() : nullptr;
}

const RayTracedShadowPassStats& SceneRenderer::GetRayTracedShadowStats() const
{
    static const RayTracedShadowPassStats emptyStats;
    // This accessor feeds the next frame's GPU budget policy.  It must return
    // the completion-published sample rather than a current graph-owned
    // snapshot, which may be unsubmitted, rejected, or awaiting timestamp
    // readback.
    return m_rayTracedShadowPass ? m_rayTracedShadowPass->GetStats() : emptyStats;
}

const RayTracedReflectionPassStats& SceneRenderer::GetRayTracedReflectionStats() const
{
    static const RayTracedReflectionPassStats emptyStats;
    return m_rayTracedReflectionPass ? m_rayTracedReflectionPass->GetStats() : emptyStats;
}

const RayTracedReflectionDenoisePassStats& SceneRenderer::GetRayTracedReflectionDenoiseStats() const
{
    static const RayTracedReflectionDenoisePassStats emptyStats;
    return m_rayTracedReflectionDenoisePass ? m_rayTracedReflectionDenoisePass->GetStats() : emptyStats;
}

const RayTracedReflectionCompositePassStats& SceneRenderer::GetRayTracedReflectionCompositeStats() const
{
    static const RayTracedReflectionCompositePassStats emptyStats;
    return m_rayTracedReflectionCompositePass ? m_rayTracedReflectionCompositePass->GetStats() : emptyStats;
}

SceneRayTracingFrameStats SceneRenderer::GetRayTracingFrameStats() const
{
    // Frame diagnostics intentionally report the active graph snapshot after
    // execution.  Budget consumers use GetRayTracedShadowStats(), above, and
    // therefore observe only submitted/completed timing data.
    const RayTracedShadowPassStats& shadowStats =
        m_activeRenderPassResults &&
        m_activeRenderPassResults->identity == m_activeRenderPassIdentity
            ? m_activeRenderPassResults->rayTracedShadowStats
            : GetRayTracedShadowStats();
    const RayTracedReflectionPassStats& reflectionStats = GetRayTracedReflectionStats();
    const RayTracedReflectionDenoisePassStats& denoiseStats = GetRayTracedReflectionDenoiseStats();
    const RayTracedReflectionCompositePassStats& compositeStats = GetRayTracedReflectionCompositeStats();

    SceneRayTracingFrameStats stats = m_rayTracingFrameBudgetStats;
    stats.sceneSupported = m_rayTracingSceneStats.supported;
    stats.scenePrepared = m_rayTracingSceneStats.prepared;
    stats.tlasAvailable = m_rayTracingSceneStats.hasTopLevelAS || GetRayTracingTopLevelAS() != nullptr;
    stats.shadowRequested = shadowStats.requested;
    stats.shadowSupported = shadowStats.supported;
    stats.shadowRecorded = shadowStats.dispatchRecorded;
    stats.reflectionRequested = reflectionStats.requested;
    stats.reflectionSupported = reflectionStats.supported;
    stats.reflectionRecorded = reflectionStats.dispatchRecorded;
    stats.reflectionDenoiseRequested = denoiseStats.requested;
    stats.reflectionDenoiseSupported = denoiseStats.supported;
    stats.reflectionDenoiseRecorded = denoiseStats.denoiseRecorded;
    stats.reflectionCompositeRequested = compositeStats.requested;
    stats.reflectionCompositeSupported = compositeStats.supported;
    stats.reflectionCompositeRecorded = compositeStats.compositeRecorded;
    stats.denoiseFallbackToRaw = compositeStats.denoiseFallbackToRaw;
    stats.reflectionMaterialTextureTableAvailable = reflectionStats.materialTextureTableAvailable;
    stats.reflectionGeometryMetadataAvailable = reflectionStats.geometryMetadataAvailable;
    stats.reflectionGeometryTableAvailable = reflectionStats.geometryTableAvailable;
    stats.shadowMaterialTextureTableAvailable = shadowStats.materialTextureTableAvailable;
    stats.shadowAlphaMetadataAvailable = shadowStats.alphaMetadataAvailable;
    stats.shadowAlphaTextureTableAvailable = shadowStats.alphaTextureTableAvailable;
    stats.shadowAlphaGeometryTableAvailable = shadowStats.alphaGeometryTableAvailable;
    stats.shadowHistoryAvailable = shadowStats.historyAvailable;
    stats.shadowDepthHistoryAvailable = shadowStats.depthHistoryAvailable;
    stats.shadowNormalHistoryAvailable = shadowStats.normalHistoryAvailable;
    stats.shadowHistoryReset = shadowStats.historyReset;
    stats.shadowHistoryRecreated = shadowStats.historyRecreated;
    stats.shadowHistoryResolutionChanged = shadowStats.historyResolutionChanged;
    stats.shadowHistoryConfigChanged = shadowStats.historyConfigChanged;
    stats.shadowTemporalAccumulated = shadowStats.temporalAccumulated;
    stats.reflectionHistoryAvailable = reflectionStats.historyAvailable;
    stats.reflectionDepthHistoryAvailable = reflectionStats.depthHistoryAvailable;
    stats.reflectionNormalHistoryAvailable = reflectionStats.normalHistoryAvailable;
    stats.reflectionHistoryReset = reflectionStats.historyReset;
    stats.reflectionHistoryRecreated = reflectionStats.historyRecreated;
    stats.reflectionHistoryResolutionChanged = reflectionStats.historyResolutionChanged;
    stats.reflectionHistoryConfigChanged = reflectionStats.historyConfigChanged;
    stats.reflectionTemporalAccumulated = reflectionStats.temporalAccumulated;
    stats.shadowGpuTimingSupported = shadowStats.gpuTimingSupported;
    stats.shadowGpuTimingQueriesRecorded = shadowStats.gpuTimingQueriesRecorded;
    stats.shadowGpuTimingResolveRecorded = shadowStats.gpuTimingResolveRecorded;
    stats.shadowGpuTimingReadbackBufferAvailable = shadowStats.gpuTimingReadbackBufferAvailable;
    stats.shadowGpuTimingResultAvailable = shadowStats.gpuTimingResultAvailable;
    stats.reflectionGpuTimingSupported = reflectionStats.gpuTimingSupported;
    stats.reflectionGpuTimingQueriesRecorded = reflectionStats.gpuTimingQueriesRecorded;
    stats.reflectionGpuTimingResolveRecorded = reflectionStats.gpuTimingResolveRecorded;
    stats.reflectionGpuTimingReadbackBufferAvailable = reflectionStats.gpuTimingReadbackBufferAvailable;
    stats.reflectionGpuTimingResultAvailable = reflectionStats.gpuTimingResultAvailable;
    stats.shadowGpuTimestampFrequency = shadowStats.gpuTimestampFrequency;
    stats.reflectionGpuTimestampFrequency = reflectionStats.gpuTimestampFrequency;
    stats.shadowGpuTimingReadbackBytes = shadowStats.gpuTimingReadbackBytes;
    stats.reflectionGpuTimingReadbackBytes = reflectionStats.gpuTimingReadbackBytes;
    stats.shadowGpuTimingStartTimestamp = shadowStats.gpuTimingStartTimestamp;
    stats.shadowGpuTimingEndTimestamp = shadowStats.gpuTimingEndTimestamp;
    stats.shadowGpuTimingElapsedTicks = shadowStats.gpuTimingElapsedTicks;
    stats.reflectionGpuTimingStartTimestamp = reflectionStats.gpuTimingStartTimestamp;
    stats.reflectionGpuTimingEndTimestamp = reflectionStats.gpuTimingEndTimestamp;
    stats.reflectionGpuTimingElapsedTicks = reflectionStats.gpuTimingElapsedTicks;
    stats.shadowGpuTimingElapsedMs = shadowStats.gpuTimingElapsedMs;
    stats.reflectionGpuTimingElapsedMs = reflectionStats.gpuTimingElapsedMs;
    stats.totalMeasuredRayTracingGpuMs =
        (stats.shadowGpuTimingResultAvailable ? stats.shadowGpuTimingElapsedMs : 0.0f) +
        (stats.reflectionGpuTimingResultAvailable ? stats.reflectionGpuTimingElapsedMs : 0.0f);
    stats.shadowGpuTimingReadbackBufferCount = shadowStats.gpuTimingReadbackBufferCount;
    stats.reflectionGpuTimingReadbackBufferCount = reflectionStats.gpuTimingReadbackBufferCount;
    stats.shadowGpuTimingReadbackFrameIndex = shadowStats.gpuTimingReadbackFrameIndex;
    stats.reflectionGpuTimingReadbackFrameIndex = reflectionStats.gpuTimingReadbackFrameIndex;
    stats.shadowGpuTimingStartQueryIndex = shadowStats.gpuTimingStartQueryIndex;
    stats.shadowGpuTimingEndQueryIndex = shadowStats.gpuTimingEndQueryIndex;
    stats.reflectionGpuTimingStartQueryIndex = reflectionStats.gpuTimingStartQueryIndex;
    stats.reflectionGpuTimingEndQueryIndex = reflectionStats.gpuTimingEndQueryIndex;
    stats.estimatedShadowRayCount = shadowStats.estimatedRayCount;
    stats.estimatedReflectionRayCount = reflectionStats.estimatedRayCount;
    stats.estimatedTotalRayCount = stats.estimatedShadowRayCount + stats.estimatedReflectionRayCount;
    stats.estimatedReflectionDenoiseTapCount = denoiseStats.estimatedTapCount;
    stats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    stats.blasCacheEvictionFrameThreshold = m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold;
    stats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    stats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    stats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    stats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    stats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    stats.cachedBLASAccelerationStructureBytes = m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    stats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    stats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    stats.topLevelAccelerationStructureBytes = m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    stats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    stats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    stats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    stats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    stats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    stats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    stats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    stats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    stats.shadowMaterialTextureCount = shadowStats.materialTextureCount;
    stats.shadowMaterialTexturesBound = shadowStats.materialTexturesBound;
    stats.shadowAlphaTextureCount = shadowStats.alphaTextureCount;
    stats.shadowAlphaTexturesBound = shadowStats.alphaTexturesBound;
    stats.shadowAlphaIndexBufferCount = shadowStats.alphaIndexBufferCount;
    stats.shadowAlphaUVBufferCount = shadowStats.alphaUVBufferCount;
    stats.reflectionMaterialTextureCount = reflectionStats.materialTextureCount;
    stats.reflectionMaterialTexturesBound = reflectionStats.materialTexturesBound;
    stats.reflectionGeometryIndexBufferCount = reflectionStats.geometryIndexBufferCount;
    stats.reflectionGeometryUVBufferCount = reflectionStats.geometryUVBufferCount;
    stats.reflectionGeometryNormalBufferCount = reflectionStats.geometryNormalBufferCount;
    stats.reflectionGeometryTangentBufferCount = reflectionStats.geometryTangentBufferCount;
    stats.shadowSamplesPerPixel = shadowStats.requested ? shadowStats.samplesPerPixel : stats.shadowSamplesPerPixel;
    stats.reflectionResolutionScale = reflectionStats.requested
                                          ? reflectionStats.resolutionScale
                                          : stats.reflectionResolutionScale;
    stats.reflectionSamplesPerPixel = reflectionStats.requested
                                          ? reflectionStats.samplesPerPixel
                                          : stats.reflectionSamplesPerPixel;
    stats.reflectionDenoiseRadius = denoiseStats.requested ? denoiseStats.radius : stats.reflectionDenoiseRadius;
    stats.reflectionDenoiseKernelTapCount = denoiseStats.requested
                                                ? denoiseStats.kernelTapCount
                                                : stats.reflectionDenoiseKernelTapCount;
    stats.shadowWidth = shadowStats.width;
    stats.shadowHeight = shadowStats.height;
    stats.reflectionWidth = reflectionStats.width;
    stats.reflectionHeight = reflectionStats.height;
    return stats;
}

void SceneRenderer::ApplyPostProcessSettings(const PostProcessSettings& settings)
{
    m_postProcessSettings = settings;
    if (m_postProcessStack)
    {
        m_postProcessStack->ApplySettings(m_postProcessSettings);
    }
}

void SceneRenderer::ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings)
{
    m_rayTracingBudgetSettings = settings;
    m_rayTracingBudgetSettings.maxTrackedResourceBytes = settings.maxTrackedResourceBytes;
    m_rayTracingBudgetSettings.maxMeasuredGpuMs =
        ClampFiniteRange(settings.maxMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs =
        ClampFiniteRange(settings.maxShadowMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs =
        ClampFiniteRange(settings.maxReflectionMeasuredGpuMs, 0.0f, 0.0f, 1000.0f);
    m_rayTracingBudgetSettings.gpuTimingHysteresis =
        ClampFiniteRange(settings.gpuTimingHysteresis, 0.15f, 0.0f, 0.95f);
    m_rayTracingBudgetSettings.gpuTimingRecoveryRate =
        ClampFiniteRange(settings.gpuTimingRecoveryRate, 0.05f, 0.001f, 1.0f);
    m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount =
        std::clamp(settings.gpuTimingAdjustmentFrameCount, 1u, 120u);
    m_rayTracingBudgetSettings.minReflectionResolutionScale =
        ClampFiniteRange(settings.minReflectionResolutionScale, 0.25f, 0.25f, 1.0f);
    m_rayTracingBudgetSettings.minShadowSamplesPerPixel =
        std::clamp(settings.minShadowSamplesPerPixel, 1u, 8u);
    m_rayTracingBudgetSettings.minReflectionSamplesPerPixel =
        std::clamp(settings.minReflectionSamplesPerPixel, 1u, 4u);
    m_rayTracingBudgetSettings.minReflectionDenoiseRadius =
        std::min(settings.minReflectionDenoiseRadius, 3u);
    m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold =
        std::min<uint64>(settings.blasCacheEvictionFrameThreshold, 36000u);
    if (!m_rayTracingBudgetSettings.enabled)
    {
        m_rayTracingGpuBudgetQualityScale = 1.0f;
        m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
        m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
        m_rayTracingLastMeasuredGpuMs = 0.0f;
        m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
        m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
        m_rayTracingLastMeasuredGpuMsValid = false;
        m_rayTracingLastShadowMeasuredGpuMsValid = false;
        m_rayTracingLastReflectionMeasuredGpuMsValid = false;
        m_rayTracingGpuBudgetLastShadowEndTimestamp = 0;
        m_rayTracingGpuBudgetLastReflectionEndTimestamp = 0;
        m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
        m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
    }
    else
    {
        if (m_rayTracingBudgetSettings.maxMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastMeasuredGpuMs = 0.0f;
            m_rayTracingLastMeasuredGpuMsValid = false;
            m_rayTracingGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingShadowGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastShadowMeasuredGpuMs = 0.0f;
            m_rayTracingLastShadowMeasuredGpuMsValid = false;
            m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;
        }
        if (m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs <= 0.0f)
        {
            m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;
            m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;
            m_rayTracingLastReflectionMeasuredGpuMsValid = false;
            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;
            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;
        }
    }
}

void SceneRenderer::ApplyShadowPassConfig(const ShadowPassConfig& config)
{
    m_shadowPassConfig = config;
    if (m_shadowPass)
    {
        m_shadowPass->SetConfig(m_shadowPassConfig);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetConfig(m_shadowPassConfig);
    }
}

void SceneRenderer::Render()
{
    if (!m_initialized || !m_renderGraph || !m_renderContext)
    {
        RefreshFrameDiagnostics(true,
                                false,
                                false,
                                false,
                                "SceneRenderer is not initialized or is missing a render context");
        return;
    }

    // Begin new frame for transient resource pool and view cache
    if (m_transientResourcePool)
    {
        m_transientResourcePool->BeginFrame();
    }

    if (m_resourceViewCache)
    {
        m_resourceViewCache->BeginFrame();
    }

    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->BeginFrame();
    }

    if (m_materialSystem && m_materialSystem->IsInitialized())
    {
        m_materialSystem->BeginFrame();
    }

    // Update view constants in pipeline cache
    if (m_pipelineCache && m_pipelineCache->IsInitialized())
    {
        m_pipelineCache->UpdateViewConstants(m_viewData);
    }

    PreparePassesForFrame();
    RunPreGraphPrepareCallbacks();
    PrepareRayTracingScene();

    const bool rayTracedReflectionsRequested =
        m_postProcessSettings.enableRayTracedReflections && m_rayTracedReflectionPass != nullptr;
    const bool rayTracedReflectionDenoiseRequested =
        rayTracedReflectionsRequested && m_postProcessSettings.enableRayTracedReflectionDenoise;
    const bool rayTracedShadowsRequested =
        m_rayTracedShadowPass && m_rayTracedShadowPass->IsRequestedEnabled();
    const bool cameraVelocityRequested = rayTracedReflectionsRequested && m_postProcessSettings.enableTAA;
    if (m_cameraVelocityPass)
    {
        m_cameraVelocityPass->SetEnabled(cameraVelocityRequested);
    }
    if (m_objectVelocityPass)
    {
        const bool objectVelocityRequested = m_cameraVelocityPass && m_cameraVelocityPass->IsEnabled();
        m_objectVelocityPass->SetEnabled(objectVelocityRequested);
    }

    ShadowPassConfig effectiveShadowConfig = m_shadowPassConfig;
    RayTracedReflectionPassConfig reflectionConfig;
    RayTracedReflectionDenoisePassConfig denoiseConfig;
    if (rayTracedReflectionsRequested)
    {
        reflectionConfig.intensity = std::max(0.0f, m_postProcessSettings.rayTracedReflectionIntensity);
        reflectionConfig.resolutionScale =
            std::clamp(m_postProcessSettings.rayTracedReflectionResolutionScale, 0.25f, 1.0f);
        reflectionConfig.maxRoughness =
            std::clamp(m_postProcessSettings.rayTracedReflectionMaxRoughness, 0.0f, 1.0f);
        reflectionConfig.maxTraceDistance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionMaxDistance);
        reflectionConfig.distanceFadeStart =
            std::clamp(m_postProcessSettings.rayTracedReflectionDistanceFadeStart, 0.0f, 1.0f);
        reflectionConfig.instanceMask =
            std::min<uint32>(m_postProcessSettings.rayTracedReflectionInstanceMask, 0xFFu);
        reflectionConfig.samplesPerPixel =
            std::clamp(m_postProcessSettings.rayTracedReflectionSamplesPerPixel, 1u, 4u);
        reflectionConfig.roughnessConeSpread =
            std::clamp(m_postProcessSettings.rayTracedReflectionRoughnessConeSpread, 0.0f, 1.0f);
        reflectionConfig.normalBias =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionNormalBias);
        reflectionConfig.rayMinT =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionRayMinT);
        reflectionConfig.fireflyClamp =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionFireflyClamp);
        reflectionConfig.temporalAccumulation = m_postProcessSettings.enableTAA;
        reflectionConfig.temporalBlendFactor =
            std::clamp(m_postProcessSettings.rayTracedReflectionTemporalBlendFactor, 0.0f, 1.0f);
        reflectionConfig.historyDepthThreshold =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryDepthThreshold);
        reflectionConfig.historyNormalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryNormalThreshold, 0.0f, 1.0f);
        reflectionConfig.historyLuminanceTolerance =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryLuminanceTolerance);
        reflectionConfig.historyConfidenceThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionHistoryConfidenceThreshold, 0.0f, 1.0f);
        reflectionConfig.historyVelocityRejectionScale =
            std::max(0.0f, m_postProcessSettings.rayTracedReflectionHistoryVelocityRejectionScale);

        denoiseConfig.radius = std::min<uint32>(
            std::max<uint32>(m_postProcessSettings.rayTracedReflectionDenoiseRadius, 0u),
            3u);
        denoiseConfig.depthSigma =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseDepthSigma, 1.0e-5f);
        denoiseConfig.normalThreshold =
            std::clamp(m_postProcessSettings.rayTracedReflectionDenoiseNormalThreshold, 0.0f, 1.0f);
        denoiseConfig.confidencePower =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseConfidencePower, 0.01f);
        denoiseConfig.centerWeight =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseCenterWeight, 0.0f);
        denoiseConfig.lowConfidenceDepthScale =
            std::max(m_postProcessSettings.rayTracedReflectionDenoiseLowConfidenceDepthScale, 1.0f);
    }

    ApplyRayTracingBudget(effectiveShadowConfig,
                          reflectionConfig,
                          denoiseConfig,
                          rayTracedShadowsRequested,
                          rayTracedReflectionsRequested,
                          rayTracedReflectionDenoiseRequested);
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetConfig(effectiveShadowConfig);
    }

    if (m_rayTracedReflectionPass && rayTracedReflectionsRequested)
    {
        m_rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
        m_rayTracedReflectionPass->SetConfig(reflectionConfig);
        m_rayTracedReflectionPass->SetEnabled(true);
        if (m_rayTracedReflectionDenoisePass)
        {
            m_rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
            m_rayTracedReflectionDenoisePass->SetConfig(denoiseConfig);
            m_rayTracedReflectionDenoisePass->SetEnabled(rayTracedReflectionDenoiseRequested);
        }
        if (m_rayTracedReflectionCompositePass)
        {
            m_rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
            m_rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
            m_rayTracedReflectionCompositePass->SetEnabled(true);
        }
    }
    else
    {
        if (m_rayTracedReflectionPass)
        {
            m_rayTracedReflectionPass->SetEnabled(false);
        }
        if (m_rayTracedReflectionDenoisePass)
        {
            m_rayTracedReflectionDenoisePass->SetEnabled(false);
        }
        if (m_rayTracedReflectionCompositePass)
        {
            m_rayTracedReflectionCompositePass->SetEnabled(false);
        }
    }

    // Freeze the per-view policy after pass configuration is stable and before
    // any RenderGraph construction or command-recording decisions occur.
    CompileRenderFramePlan();
    BuildGPUDrivenVisibilityInputs();

    // Clear the render graph for this frame
    m_renderGraph->Clear();

    // Build the render graph (creates depth buffer if needed, imports resources)
    BuildRenderGraph();

    // Update pass resources AFTER BuildRenderGraph creates resources
    // This provides passes with render scene data and texture views
    UpdatePassResources();

    // Compile the render graph (computes barriers, memory aliasing, pass culling)
    m_renderGraph->Compile();
    const bool graphCompileValid = m_renderGraph->GetCompileStats().compileValid;

    if (RenderSubmissionTracker* tracker =
            RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext))
    {
        RetireOwnerSnapshots(tracker->CaptureLastSubmittedToken());
    }

    // Execute through RenderGraph for automatic barrier management
    RHICommandContext* ctx = m_renderContext->GetGraphicsContext();
    bool graphExecuted = false;
    const char* executionSkippedReason = nullptr;
    if (ctx && graphCompileValid)
    {
        m_renderGraph->Execute(*ctx);
        graphExecuted = true;
        CommitGPUDrivenAccessSnapshots();
        if (m_activeRenderPassResults &&
            m_activeRenderPassResults->identity == m_activeRenderPassIdentity)
        {
            m_renderPolicyDiagnostics.executionReport =
                m_activeRenderPassResults->executionReport;
            if (m_shadowPass)
            {
                m_shadowPass->PublishRecordResults(
                    m_activeRenderPassResults,
                    m_activeRenderPassIdentity);
            }
            if (m_depthPrepass)
            {
                m_depthPrepass->PublishRecordResults(
                    m_activeRenderPassResults,
                    m_activeRenderPassIdentity);
            }
            if (m_opaquePass)
            {
                m_opaquePass->PublishRecordResults(
                    m_activeRenderPassResults,
                    m_activeRenderPassIdentity);
            }
        }
        if (m_depthTexture && m_depthGraphHandle.IsValid())
        {
            m_depthAccessSnapshot = m_renderGraph->GetRealizedAccess(
                m_depthGraphHandle);
        }
        if (m_backBufferGraphHandle.IsValid() &&
            m_activeBackBufferIndex < m_backBufferAccessSnapshots.size())
        {
            m_backBufferAccessSnapshots[m_activeBackBufferIndex] =
                m_renderGraph->GetRealizedAccess(m_backBufferGraphHandle);
        }
        if (m_opaquePass)
        {
            const OpaquePassDrawStats& opaqueStats = m_opaquePass->GetDrawStats();
            m_gpuDrivenCullingStats.opaqueIndirectRequested = opaqueStats.gpuDrivenRequested;
            m_gpuDrivenCullingStats.opaqueCullingReady = opaqueStats.gpuDrivenCullingReady;
            m_gpuDrivenCullingStats.opaquePipelineReady = opaqueStats.gpuDrivenPipelineReady;
            m_gpuDrivenCullingStats.opaqueIndirectEligible = opaqueStats.gpuDrivenEligible;
            m_gpuDrivenCullingStats.opaqueIndirectSubmitted = opaqueStats.gpuDrivenSubmitted;
            m_gpuDrivenCullingStats.opaqueDirectDrawCount = opaqueStats.directDrawCount;
            m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectBatchCount =
                opaqueStats.gpuDrivenIndirectBatchCount;
            m_gpuDrivenCullingStats
                .opaqueGpuDrivenIndirectSubmittedDrawUpperBound =
                opaqueStats.gpuDrivenIndirectSubmittedDrawUpperBound;
            m_gpuDrivenCullingStats
                .opaqueGpuDrivenExecutedDrawCountAvailable =
                opaqueStats.gpuDrivenIndirectExecutedDrawCountAvailable;
            m_gpuDrivenCullingStats.opaqueGpuDrivenIndirectDrawCount =
                opaqueStats.gpuDrivenIndirectDrawCount;
            m_gpuDrivenCullingStats.opaqueFallbackReason =
                opaqueStats.gpuDrivenFallbackReason;
        }
        if (m_opaqueGPUCulling && m_opaqueGPUCullingFramePrepared)
        {
            m_gpuDrivenCullingStats.gpuVisibilityCountsAvailable =
                m_opaqueGPUCulling->WasCpuFallbackUsedLastCull();
            if (m_gpuDrivenCullingStats.gpuVisibilityCountsAvailable)
            {
                const GPUCulling::Statistics cullingStats =
                    m_opaqueGPUCulling->GetStatistics();
                m_gpuDrivenCullingStats.visibleCullableDrawItemCount =
                    cullingStats.visibleInstances;
                m_gpuDrivenCullingStats.frustumCulledDrawItemCount =
                    cullingStats.frustumCulled;
                m_gpuDrivenCullingStats.distanceCulledDrawItemCount =
                    cullingStats.distanceCulled;
            }
        }
    }
    else
    {
        executionSkippedReason = graphCompileValid
                                     ? "Graphics command context is unavailable"
                                     : "RenderGraph compile reported validation errors";
    }

    if (graphExecuted && m_renderPolicyDiagnostics.planAvailable)
    {
        m_renderPolicyDiagnostics.reportAvailable = true;
        bool anyFailed = false;
        bool allCompleted =
            !m_renderPolicyDiagnostics.executionReport.passes.empty();
        for (const RenderPassExecutionReport& report :
             m_renderPolicyDiagnostics.executionReport.passes)
        {
            anyFailed |= report.status == RenderExecutionStatus::Failed;
            allCompleted &=
                report.status == RenderExecutionStatus::Completed;
        }
        m_renderPolicyDiagnostics.executionReport.status = anyFailed
            ? RenderExecutionStatus::Failed
            : (allCompleted ? RenderExecutionStatus::Completed
                            : RenderExecutionStatus::NotAttempted);
    }

    RefreshFrameDiagnostics(true,
                            graphExecuted && graphCompileValid,
                            true,
                            true,
                            executionSkippedReason);

    // Log compile stats periodically for debugging
    static uint64_t frameCount = 0;
    if (++frameCount == 1)
    {
        const auto& stats = m_renderGraph->GetCompileStats();
        RVX_CORE_INFO("RenderGraph stats: {} passes ({} culled), {} barriers, memory savings: {:.1f}%",
                      stats.totalPasses, stats.culledPasses, stats.barrierCount,
                      stats.GetMemorySavingsPercent());
    }

    // End frame for transient resource pool
    if (m_transientResourcePool)
    {
        m_transientResourcePool->EndFrame();

        // Evict unused resources every 60 frames (approx 1 second at 60fps)
        if (frameCount % 60 == 0)
        {
            m_transientResourcePool->EvictUnused(3);  // Evict resources unused for 3 frames
        }
    }
    UpdateObjectMotionHistory();
    m_previousViewProjectionMatrix = m_viewData.viewProjectionMatrix;
    m_previousViewProjectionValid = true;
}

void SceneRenderer::ExecutePasses(RHICommandContext& ctx)
{
    // NOTE: This is a legacy path for manual pass execution without RenderGraph.
    // The preferred path is Render() -> BuildRenderGraph() -> RenderGraph::Execute()
    // which handles barrier management automatically.

    RHISwapChain* swapChain = m_renderContext->GetSwapChain();
    if (!swapChain)
        return;

    // Get current back buffer and its tracked state
    // State tracking is managed by BuildRenderGraph(), but if called standalone,
    // ensure we have valid state tracking
    uint32_t bufferCount = swapChain->GetBufferCount();
    if (m_backBufferAccessSnapshots.size() != bufferCount)
    {
        m_backBufferAccessSnapshots.assign(
            bufferCount,
            MakeRHITextureAccessSnapshot(
                RHIResourceState::Undefined,
                RHIShaderStage::None,
                GPUQueueDomain::Graphics,
                RHIContentValidity::Invalid));
    }

    uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
    RHITextureAccessSnapshot& backBufferAccess =
        m_backBufferAccessSnapshots[backBufferIndex];
    const RHIAccessSnapshot renderTargetAccess = MakeRHIAccessSnapshot(
        RHIResourceState::RenderTarget,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics);

    // Transition back buffer to RenderTarget (from Undefined on first use, Present thereafter)
    if (backBuffer && backBufferAccess.uniformAccess != renderTargetAccess)
    {
        ctx.TextureBarrier(
            backBuffer,
            backBufferAccess.uniformAccess,
            renderTargetAccess,
            RHISubresourceRange::All(),
            backBufferAccess.uniformAccess.contentValidity == RHIContentValidity::Valid
                ? RHIDiscardIntent::Preserve
                : RHIDiscardIntent::Discard);
        backBufferAccess.uniformAccess = renderTargetAccess;
    }

    // Transition depth buffer to DepthWrite if it exists
    const RHIAccessSnapshot depthWriteAccess = MakeRHIAccessSnapshot(
        RHIResourceState::DepthWrite,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics);
    if (m_depthTexture && m_depthAccessSnapshot.uniformAccess != depthWriteAccess)
    {
        ctx.TextureBarrier(
            m_depthTexture.Get(),
            m_depthAccessSnapshot.uniformAccess,
            depthWriteAccess,
            RHISubresourceRange::All(),
            m_depthAccessSnapshot.uniformAccess.contentValidity == RHIContentValidity::Valid
                ? RHIDiscardIntent::Preserve
                : RHIDiscardIntent::Discard);
        m_depthAccessSnapshot.uniformAccess = depthWriteAccess;
    }

    if (!m_passRegistry)
        return;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (pass && pass->IsEnabled())
        {
            pass->Execute(ctx, m_viewData);
        }
    }

    // Transition back buffer from RenderTarget back to Present
    const RHIAccessSnapshot presentAccess = MakeRHIAccessSnapshot(
        RHIResourceState::Present,
        RHIShaderStage::None,
        GPUQueueDomain::Graphics);
    if (backBuffer && backBufferAccess.uniformAccess != presentAccess)
    {
        ctx.TextureBarrier(
            backBuffer,
            backBufferAccess.uniformAccess,
            presentAccess);
        backBufferAccess.uniformAccess = presentAccess;
    }
}

void SceneRenderer::UpdatePassResources()
{
    if (!m_renderContext)
        return;

    RHITextureView* colorTargetView = nullptr;
    RHITextureView* depthTargetView = m_depthTextureView.Get();
    if (m_externalRenderTargetStats.active && m_renderGraph && m_resourceViewCache)
    {
        if (RHITexture* colorTarget = m_renderGraph->GetTexture(m_viewData.colorTarget))
        {
            colorTargetView = m_resourceViewCache->GetDefaultRTV(colorTarget);
        }
        if (RHITexture* depthTarget = m_renderGraph->GetTexture(m_viewData.depthTarget))
        {
            depthTargetView = m_resourceViewCache->GetDefaultDSV(depthTarget);
        }
    }

    RenderFrameResourceBinder::BindScenePassResources(
        *m_renderContext,
        m_renderScene,
        m_opaqueDrawItems,
        m_maskedDrawItems,
        m_transparentDrawItems,
        colorTargetView,
        depthTargetView,
        nullptr,
        nullptr,
        m_shadowPass,
        m_transparentPass,
        m_skyboxPass);

    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetRenderScene(&m_renderScene, &m_opaqueDrawItems, &m_maskedDrawItems);
    }
}

void SceneRenderer::ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,
                                          RayTracedReflectionPassConfig& reflectionConfig,
                                          RayTracedReflectionDenoisePassConfig& denoiseConfig,
                                          bool shadowRequested,
                                          bool reflectionRequested,
                                          bool denoiseRequested)
{
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->RefreshCompletionDiagnostics();
    }

    SceneRayTracingFrameStats budgetStats;
    budgetStats.budgetEnabled = m_rayTracingBudgetSettings.enabled;
    budgetStats.rayBudget = m_rayTracingBudgetSettings.maxRayCount;
    budgetStats.denoiseTapBudget = m_rayTracingBudgetSettings.maxDenoiseTapCount;
    budgetStats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;
    budgetStats.shadowRequested = shadowRequested;
    budgetStats.reflectionRequested = reflectionRequested;
    budgetStats.reflectionDenoiseRequested = denoiseRequested;
    budgetStats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;
    budgetStats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;
    budgetStats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;
    budgetStats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;
    budgetStats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;
    budgetStats.cachedBLASAccelerationStructureBytes =
        m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;
    budgetStats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;
    budgetStats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;
    budgetStats.topLevelAccelerationStructureBytes =
        m_rayTracingSceneStats.topLevelAccelerationStructureBytes;
    budgetStats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;
    budgetStats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;
    budgetStats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;
    budgetStats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;
    budgetStats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;
    budgetStats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;
    budgetStats.resourceBudgetExceeded =
        m_rayTracingBudgetSettings.enabled &&
        m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0 &&
        m_rayTracingSceneStats.resourceBudgetExceeded;
    budgetStats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;
    budgetStats.shadowWidth = shadowRequested ? m_viewData.viewportWidth : 0;
    budgetStats.shadowHeight = shadowRequested ? m_viewData.viewportHeight : 0;

    const uint32 requestedShadowSamples =
        shadowRequested ? std::clamp(shadowConfig.rayTracedSamplesPerPixel, 1u, 8u) : 0u;
    const uint32 requestedReflectionSamples =
        reflectionRequested ? std::clamp(reflectionConfig.samplesPerPixel, 1u, 4u) : 0u;
    const float requestedReflectionScale =
        reflectionRequested ? ClampFiniteRange(reflectionConfig.resolutionScale, 1.0f, 0.25f, 1.0f) : 0.0f;
    const uint32 requestedDenoiseRadius = denoiseRequested ? std::min(denoiseConfig.radius, 3u) : 0u;

    uint32 appliedShadowSamples = requestedShadowSamples;
    uint32 appliedReflectionSamples = requestedReflectionSamples;
    float appliedReflectionScale = requestedReflectionScale;
    uint32 appliedDenoiseRadius = requestedDenoiseRadius;

    const auto estimateShadowRays =
        [this](uint32 samplesPerPixel) -> uint64
        {
            return EstimateRayTracingPixelCount(m_viewData.viewportWidth, m_viewData.viewportHeight) *
                   static_cast<uint64>(samplesPerPixel);
        };
    const auto estimateReflectionRays =
        [this](float resolutionScale, uint32 samplesPerPixel) -> uint64
        {
            return EstimateReflectionRayCount(m_viewData.viewportWidth,
                                              m_viewData.viewportHeight,
                                              resolutionScale,
                                              samplesPerPixel);
        };
    const auto estimateTotalRays =
        [&](uint32 shadowSamples, float reflectionScale, uint32 reflectionSamples) -> uint64
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(shadowSamples) : 0u;
            const uint64 reflectionRays = reflectionRequested
                                              ? estimateReflectionRays(reflectionScale, reflectionSamples)
                                              : 0u;
            return shadowRays + reflectionRays;
        };

    budgetStats.requestedShadowSamplesPerPixel = requestedShadowSamples;
    budgetStats.requestedReflectionSamplesPerPixel = requestedReflectionSamples;
    budgetStats.requestedReflectionResolutionScale = requestedReflectionScale;
    budgetStats.requestedReflectionDenoiseRadius = requestedDenoiseRadius;
    budgetStats.requestedReflectionDenoiseKernelTapCount =
        EstimateReflectionDenoiseKernelTapCount(requestedDenoiseRadius);
    budgetStats.estimatedRayCountBeforeBudget =
        estimateTotalRays(requestedShadowSamples, requestedReflectionScale, requestedReflectionSamples);
    budgetStats.estimatedDenoiseTapCountBeforeBudget =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                requestedReflectionScale,
                                                requestedDenoiseRadius)
            : 0u;

    const RayTracedShadowPassStats& previousShadowStats = GetRayTracedShadowStats();
    const RayTracedReflectionPassStats& previousReflectionStats = GetRayTracedReflectionStats();
    const bool shadowTimingAvailable = previousShadowStats.gpuTimingResultAvailable;
    const bool reflectionTimingAvailable = previousReflectionStats.gpuTimingResultAvailable;
    const bool hasNewShadowGpuTimingSample =
        shadowTimingAvailable &&
        previousShadowStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastShadowEndTimestamp;
    const bool hasNewReflectionGpuTimingSample =
        reflectionTimingAvailable &&
        previousReflectionStats.gpuTimingEndTimestamp != m_rayTracingGpuBudgetLastReflectionEndTimestamp;
    const bool hasNewGpuTimingSample = hasNewShadowGpuTimingSample || hasNewReflectionGpuTimingSample;

    if (hasNewGpuTimingSample)
    {
        float measuredGpuMs = 0.0f;
        if (shadowTimingAvailable)
        {
            measuredGpuMs += previousShadowStats.gpuTimingElapsedMs;
        }
        if (reflectionTimingAvailable)
        {
            measuredGpuMs += previousReflectionStats.gpuTimingElapsedMs;
        }

        if (hasNewShadowGpuTimingSample && std::isfinite(previousShadowStats.gpuTimingElapsedMs) &&
            previousShadowStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastShadowMeasuredGpuMs = previousShadowStats.gpuTimingElapsedMs;
            m_rayTracingLastShadowMeasuredGpuMsValid = true;
        }
        if (hasNewReflectionGpuTimingSample && std::isfinite(previousReflectionStats.gpuTimingElapsedMs) &&
            previousReflectionStats.gpuTimingElapsedMs >= 0.0f)
        {
            m_rayTracingLastReflectionMeasuredGpuMs = previousReflectionStats.gpuTimingElapsedMs;
            m_rayTracingLastReflectionMeasuredGpuMsValid = true;
        }
        if (std::isfinite(measuredGpuMs) && measuredGpuMs >= 0.0f)
        {
            m_rayTracingLastMeasuredGpuMs = measuredGpuMs;
            m_rayTracingLastMeasuredGpuMsValid = true;
        }

        if (hasNewShadowGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastShadowEndTimestamp = previousShadowStats.gpuTimingEndTimestamp;
        }
        if (hasNewReflectionGpuTimingSample)
        {
            m_rayTracingGpuBudgetLastReflectionEndTimestamp = previousReflectionStats.gpuTimingEndTimestamp;
        }
    }

    budgetStats.gpuTimeBudget = m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    budgetStats.shadowGpuTimeBudget = m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    budgetStats.reflectionGpuTimeBudget = m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    budgetStats.measuredGpuTimeAvailable = m_rayTracingLastMeasuredGpuMsValid;
    budgetStats.measuredGpuTimeForBudgetMs = m_rayTracingLastMeasuredGpuMsValid
                                               ? m_rayTracingLastMeasuredGpuMs
                                               : 0.0f;
    budgetStats.measuredShadowGpuTimeForBudgetMs = m_rayTracingLastShadowMeasuredGpuMsValid
                                                     ? m_rayTracingLastShadowMeasuredGpuMs
                                                     : 0.0f;
    budgetStats.measuredReflectionGpuTimeForBudgetMs = m_rayTracingLastReflectionMeasuredGpuMsValid
                                                         ? m_rayTracingLastReflectionMeasuredGpuMs
                                                         : 0.0f;
    budgetStats.gpuTimingRecoveryRate = m_rayTracingBudgetSettings.gpuTimingRecoveryRate;
    budgetStats.gpuTimeBudgetAdjustmentFrameCount =
        m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;

    const auto updateGpuBudgetFeedback =
        [this](float budget,
               float measuredGpuMs,
               bool measuredGpuMsValid,
               bool hasNewSample,
               bool requested,
               float& qualityScale,
               uint32& overBudgetFrameCount,
               uint32& underBudgetFrameCount,
               bool& budgetExceeded,
               bool& qualityScaleAdjusted)
        {
            if (!m_rayTracingBudgetSettings.enabled || budget <= 0.0f || !requested)
            {
                qualityScale = 1.0f;
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
                return;
            }

            if (!measuredGpuMsValid)
                return;

            budgetExceeded = measuredGpuMs > budget;
            if (!hasNewSample)
                return;

            const float recoveryThreshold = budget * (1.0f - m_rayTracingBudgetSettings.gpuTimingHysteresis);
            const uint32 adjustmentFrameCount = m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount;
            if (budgetExceeded && measuredGpuMs > 0.0f)
            {
                ++overBudgetFrameCount;
                underBudgetFrameCount = 0;
                if (overBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float targetScale = static_cast<float>(
                        std::sqrt(static_cast<double>(budget) / static_cast<double>(measuredGpuMs)));
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::clamp(std::min(qualityScale, targetScale),
                                              m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                              1.0f);
                    qualityScaleAdjusted = qualityScale < previousQualityScale;
                    overBudgetFrameCount = 0;
                }
            }
            else if (measuredGpuMs < recoveryThreshold && qualityScale < 1.0f)
            {
                ++underBudgetFrameCount;
                overBudgetFrameCount = 0;
                if (underBudgetFrameCount >= adjustmentFrameCount)
                {
                    const float previousQualityScale = qualityScale;
                    qualityScale = std::min(
                        1.0f,
                        qualityScale * (1.0f + m_rayTracingBudgetSettings.gpuTimingRecoveryRate));
                    qualityScaleAdjusted = qualityScale > previousQualityScale;
                    underBudgetFrameCount = 0;
                }
            }
            else
            {
                overBudgetFrameCount = 0;
                underBudgetFrameCount = 0;
            }
        };

    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMs,
                            m_rayTracingLastMeasuredGpuMsValid,
                            hasNewGpuTimingSample,
                            shadowRequested || reflectionRequested,
                            m_rayTracingGpuBudgetQualityScale,
                            m_rayTracingGpuBudgetOverBudgetFrameCount,
                            m_rayTracingGpuBudgetUnderBudgetFrameCount,
                            budgetStats.gpuTimeBudgetExceeded,
                            budgetStats.gpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMs,
                            m_rayTracingLastShadowMeasuredGpuMsValid,
                            hasNewShadowGpuTimingSample,
                            shadowRequested,
                            m_rayTracingShadowGpuBudgetQualityScale,
                            m_rayTracingShadowGpuBudgetOverBudgetFrameCount,
                            m_rayTracingShadowGpuBudgetUnderBudgetFrameCount,
                            budgetStats.shadowGpuTimeBudgetExceeded,
                            budgetStats.shadowGpuTimeBudgetQualityScaleAdjusted);
    updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMs,
                            m_rayTracingLastReflectionMeasuredGpuMsValid,
                            hasNewReflectionGpuTimingSample,
                            reflectionRequested,
                            m_rayTracingReflectionGpuBudgetQualityScale,
                            m_rayTracingReflectionGpuBudgetOverBudgetFrameCount,
                            m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount,
                            budgetStats.reflectionGpuTimeBudgetExceeded,
                            budgetStats.reflectionGpuTimeBudgetQualityScaleAdjusted);

    budgetStats.gpuTimeBudgetOverBudgetFrameCount = m_rayTracingGpuBudgetOverBudgetFrameCount;
    budgetStats.gpuTimeBudgetUnderBudgetFrameCount = m_rayTracingGpuBudgetUnderBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetOverBudgetFrameCount = m_rayTracingShadowGpuBudgetOverBudgetFrameCount;
    budgetStats.shadowGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingShadowGpuBudgetUnderBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetOverBudgetFrameCount = m_rayTracingReflectionGpuBudgetOverBudgetFrameCount;
    budgetStats.reflectionGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount;

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0 &&
        budgetStats.estimatedRayCountBeforeBudget > m_rayTracingBudgetSettings.maxRayCount)
    {
        const uint32 minReflectionSamples = std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                                                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples);
        const uint32 minShadowSamples = std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                                                 appliedShadowSamples == 0 ? 1u : appliedShadowSamples);

        if (reflectionRequested && appliedReflectionSamples > minReflectionSamples)
        {
            appliedReflectionSamples = minReflectionSamples;
            budgetStats.budgetApplied = true;
        }
        if (shadowRequested && appliedShadowSamples > minShadowSamples)
        {
            appliedShadowSamples = minShadowSamples;
            budgetStats.budgetApplied = true;
        }

        uint64 estimatedRays = estimateTotalRays(appliedShadowSamples,
                                                 appliedReflectionScale,
                                                 appliedReflectionSamples);
        if (reflectionRequested && estimatedRays > m_rayTracingBudgetSettings.maxRayCount &&
            appliedReflectionScale > m_rayTracingBudgetSettings.minReflectionResolutionScale)
        {
            const uint64 shadowRays = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
            const uint64 remainingReflectionBudget =
                m_rayTracingBudgetSettings.maxRayCount > shadowRays
                    ? m_rayTracingBudgetSettings.maxRayCount - shadowRays
                    : 0u;
            float budgetScale = m_rayTracingBudgetSettings.minReflectionResolutionScale;
            if (remainingReflectionBudget > 0 && appliedReflectionSamples > 0 &&
                m_viewData.viewportWidth > 0 && m_viewData.viewportHeight > 0)
            {
                const double fullResolutionPixels = static_cast<double>(m_viewData.viewportWidth) *
                                                    static_cast<double>(m_viewData.viewportHeight);
                const double affordablePixels =
                    static_cast<double>(remainingReflectionBudget) /
                    static_cast<double>(appliedReflectionSamples);
                budgetScale = static_cast<float>(std::sqrt(std::max(0.0, affordablePixels / fullResolutionPixels)));
            }
            const float previousReflectionScale = appliedReflectionScale;
            appliedReflectionScale = std::clamp(budgetScale,
                                                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                                appliedReflectionScale);
            appliedReflectionScale = ReduceReflectionResolutionScaleToFitRayBudget(
                m_viewData.viewportWidth,
                m_viewData.viewportHeight,
                appliedReflectionScale,
                m_rayTracingBudgetSettings.minReflectionResolutionScale,
                appliedReflectionSamples,
                remainingReflectionBudget);
            budgetStats.budgetApplied = budgetStats.budgetApplied ||
                                        appliedReflectionScale < previousReflectionScale;
            estimatedRays = estimateTotalRays(appliedShadowSamples,
                                              appliedReflectionScale,
                                              appliedReflectionSamples);
        }

        budgetStats.rayBudgetExceeded = estimatedRays > m_rayTracingBudgetSettings.maxRayCount;
    }

    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0 &&
        budgetStats.estimatedDenoiseTapCountBeforeBudget > m_rayTracingBudgetSettings.maxDenoiseTapCount)
    {
        const uint32 minDenoiseRadius = std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius,
                                                 appliedDenoiseRadius);
        while (appliedDenoiseRadius > minDenoiseRadius)
        {
            const uint64 estimatedTapCount =
                EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                  m_viewData.viewportHeight,
                                                  appliedReflectionScale,
                                                  appliedDenoiseRadius);
            if (estimatedTapCount <= m_rayTracingBudgetSettings.maxDenoiseTapCount)
                break;

            --appliedDenoiseRadius;
            budgetStats.budgetApplied = true;
        }
    }

    const bool gpuTimeBudgetingEnabled =
        m_rayTracingBudgetSettings.enabled &&
        (m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f ||
         m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f);
    const float shadowGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingShadowGpuBudgetQualityScale);
    const float reflectionGpuQualityScale =
        std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingReflectionGpuBudgetQualityScale);
    if (gpuTimeBudgetingEnabled && shadowRequested && shadowGpuQualityScale < 0.999f)
    {
        appliedShadowSamples = ApplyRayTracingGpuBudgetScale(
            appliedShadowSamples,
            std::min(m_rayTracingBudgetSettings.minShadowSamplesPerPixel,
                     appliedShadowSamples == 0 ? 1u : appliedShadowSamples),
            shadowGpuQualityScale);
        budgetStats.shadowGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && reflectionRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedReflectionScale = std::max(m_rayTracingBudgetSettings.minReflectionResolutionScale,
                                          appliedReflectionScale * reflectionGpuQualityScale);
        appliedReflectionSamples = ApplyRayTracingGpuBudgetScale(
            appliedReflectionSamples,
            std::min(m_rayTracingBudgetSettings.minReflectionSamplesPerPixel,
                     appliedReflectionSamples == 0 ? 1u : appliedReflectionSamples),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    if (gpuTimeBudgetingEnabled && denoiseRequested && reflectionGpuQualityScale < 0.999f)
    {
        appliedDenoiseRadius = ApplyRayTracingGpuBudgetScale(
            appliedDenoiseRadius,
            std::min(m_rayTracingBudgetSettings.minReflectionDenoiseRadius, appliedDenoiseRadius),
            reflectionGpuQualityScale);
        budgetStats.reflectionGpuTimeBudgetApplied = true;
    }
    budgetStats.gpuTimeBudgetApplied =
        budgetStats.shadowGpuTimeBudgetApplied || budgetStats.reflectionGpuTimeBudgetApplied;
    if (budgetStats.gpuTimeBudgetApplied)
    {
        budgetStats.budgetApplied = true;
    }
    budgetStats.gpuTimeBudgetQualityScale = m_rayTracingGpuBudgetQualityScale;
    budgetStats.shadowGpuTimeBudgetQualityScale = m_rayTracingShadowGpuBudgetQualityScale;
    budgetStats.reflectionGpuTimeBudgetQualityScale = m_rayTracingReflectionGpuBudgetQualityScale;

    if (shadowRequested)
    {
        shadowConfig.rayTracedSamplesPerPixel = appliedShadowSamples;
    }
    if (reflectionRequested)
    {
        reflectionConfig.samplesPerPixel = appliedReflectionSamples;
        reflectionConfig.resolutionScale = appliedReflectionScale;
    }
    if (denoiseRequested)
    {
        denoiseConfig.radius = appliedDenoiseRadius;
    }

    budgetStats.shadowSamplesPerPixel = appliedShadowSamples;
    budgetStats.reflectionSamplesPerPixel = appliedReflectionSamples;
    budgetStats.reflectionResolutionScale = appliedReflectionScale;
    budgetStats.reflectionDenoiseRadius = appliedDenoiseRadius;
    budgetStats.reflectionDenoiseKernelTapCount = EstimateReflectionDenoiseKernelTapCount(appliedDenoiseRadius);
    budgetStats.reflectionWidth = reflectionRequested
                                      ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportWidth,
                                                                                   appliedReflectionScale)
                                      : 0u;
    budgetStats.reflectionHeight = reflectionRequested
                                       ? ResolveRayTracingBudgetReflectionDimension(m_viewData.viewportHeight,
                                                                                    appliedReflectionScale)
                                       : 0u;
    budgetStats.estimatedShadowRayCount = shadowRequested ? estimateShadowRays(appliedShadowSamples) : 0u;
    budgetStats.estimatedReflectionRayCount = reflectionRequested
                                                  ? estimateReflectionRays(appliedReflectionScale,
                                                                           appliedReflectionSamples)
                                                  : 0u;
    budgetStats.estimatedTotalRayCount = budgetStats.estimatedShadowRayCount +
                                         budgetStats.estimatedReflectionRayCount;
    budgetStats.estimatedReflectionDenoiseTapCount =
        denoiseRequested
            ? EstimateReflectionDenoiseTapCount(m_viewData.viewportWidth,
                                                m_viewData.viewportHeight,
                                                appliedReflectionScale,
                                                appliedDenoiseRadius)
            : 0u;
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxRayCount > 0)
    {
        budgetStats.rayBudgetExceeded =
            budgetStats.estimatedTotalRayCount > m_rayTracingBudgetSettings.maxRayCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxMeasuredGpuMs > 0.0f &&
        m_rayTracingLastMeasuredGpuMsValid)
    {
        budgetStats.gpuTimeBudgetExceeded =
            m_rayTracingLastMeasuredGpuMs > m_rayTracingBudgetSettings.maxMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs > 0.0f &&
        m_rayTracingLastShadowMeasuredGpuMsValid)
    {
        budgetStats.shadowGpuTimeBudgetExceeded =
            m_rayTracingLastShadowMeasuredGpuMs > m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs > 0.0f &&
        m_rayTracingLastReflectionMeasuredGpuMsValid)
    {
        budgetStats.reflectionGpuTimeBudgetExceeded =
            m_rayTracingLastReflectionMeasuredGpuMs > m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxDenoiseTapCount > 0)
    {
        budgetStats.denoiseTapBudgetExceeded =
            budgetStats.estimatedReflectionDenoiseTapCount > m_rayTracingBudgetSettings.maxDenoiseTapCount;
    }
    if (m_rayTracingBudgetSettings.enabled && m_rayTracingBudgetSettings.maxTrackedResourceBytes > 0)
    {
        budgetStats.resourceBudgetExceeded = m_rayTracingSceneStats.resourceBudgetExceeded;
    }

    m_rayTracingFrameBudgetStats = budgetStats;
}

void SceneRenderer::PreparePassesForFrame()
{
    m_viewData.directionalLightDirection = Vec3{0.5f, -0.8f, 0.3f};
    m_viewData.directionalLightIntensity = 4.0f;
    m_viewData.directionalLightColor = Vec3{1.0f, 1.0f, 1.0f};
    m_viewData.directionalShadowEnabled = 0;
    m_viewData.directionalShadowCascadeCount = 0;
    m_viewData.directionalShadowCascadeSplits = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowCascadeFadeDistances = Vec4(0.0f, 0.0f, 0.0f, 0.0f);
    m_viewData.directionalShadowViewProjection = Mat4Identity();
    m_viewData.directionalShadowInvMapSize = 0.0f;
    m_viewData.directionalShadowFilterRadiusTexels = 1.0f;
    m_viewData.directionalShadowNormalBias = 0.0f;
    m_viewData.rayTracedShadowEnabled = 0;
    m_viewData.rayTracedShadowFilterRadiusPixels = 1.0f;
    m_viewData.rayTracedShadowMode = RayTracedShadowMode::ComplementRaster;

    if (m_shadowPass)
    {
        m_shadowPass->SetEnabled(false);
    }
    if (m_rayTracedShadowPass)
    {
        m_rayTracedShadowPass->SetEnabled(false);
        m_rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (m_cameraVelocityPass)
    {
        m_cameraVelocityPass->SetEnabled(false);
    }
    if (m_objectVelocityPass)
    {
        m_objectVelocityPass->SetEnabled(false);
    }
    if (m_rayTracedReflectionPass)
    {
        m_rayTracedReflectionPass->SetEnabled(false);
        m_rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    }
    if (m_rayTracedReflectionDenoisePass)
    {
        m_rayTracedReflectionDenoisePass->SetEnabled(false);
        m_rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
    }
    if (m_rayTracedReflectionCompositePass)
    {
        m_rayTracedReflectionCompositePass->SetEnabled(false);
        m_rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
        m_rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
    }

    if (m_opaquePass)
    {
        m_opaquePass->SetDirectionalShadowSource(m_shadowPass);
    }

    if (m_lightManager)
    {
        m_lightManager->CollectLights(m_renderScene);
        m_lightManager->UpdateGPUBuffers();
        m_localLightingStats.frameCount++;
        m_localLightingStats.pointLightCount = m_lightManager->GetPointLightCount();
        m_localLightingStats.spotLightCount = m_lightManager->GetSpotLightCount();
        m_localLightingStats.lightConstantsBufferReady = m_lightManager->GetLightConstantsBuffer() != nullptr;
        m_localLightingStats.pointLightsBufferReady = m_lightManager->GetPointLightsBuffer() != nullptr;
        m_localLightingStats.spotLightsBufferReady = m_lightManager->GetSpotLightsBuffer() != nullptr;
        m_localLightingStats.pointShadowRequestCount = m_lightManager->GetPointShadowRequestCount();
        m_localLightingStats.spotShadowRequestCount = m_lightManager->GetSpotShadowRequestCount();
        m_localLightingStats.localShadowRequestCount = m_lightManager->GetLocalShadowRequestCount();
        m_localLightingStats.localShadowAtlasReady = false;
        m_localLightingStats.localShadowFallbackReason =
            m_localLightingStats.localShadowRequestCount > 0
                ? "local point/spot shadow atlas is not implemented"
                : "";
    }

    const uint64 clusteredLightingFrameCount = m_clusteredLightingStats.frameCount + 1;
    m_clusteredLightingStats = {};
    m_clusteredLightingStats.frameCount = clusteredLightingFrameCount;
    if (!m_clusteredLighting)
    {
        m_clusteredLightingStats.fallbackReason = "clustered lighting system is not created";
    }
    else
    {
        m_clusteredLightingStats.initialized = m_clusteredLighting->IsInitialized();
        m_clusteredLightingStats.clusterAABBBufferReady = m_clusteredLighting->GetClusterAABBBuffer() != nullptr;
        m_clusteredLightingStats.clusterBufferReady = m_clusteredLighting->GetClusterBuffer() != nullptr;
        m_clusteredLightingStats.lightIndexBufferReady = m_clusteredLighting->GetLightIndexBuffer() != nullptr;
        m_clusteredLightingStats.clusterConstantsBufferReady =
            m_clusteredLighting->GetClusterConstantsBuffer() != nullptr;

        if (!m_clusteredLightingStats.initialized)
        {
            m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            if (m_clusteredLightingStats.fallbackReason.empty())
            {
                m_clusteredLightingStats.fallbackReason = "clustered lighting system is not initialized";
            }
        }
        else if (!m_lightManager)
        {
            m_clusteredLightingStats.fallbackReason = "frame light manager is not available";
        }
        else if (!m_clusteredLighting->BeginFrame(m_viewData.viewMatrix,
                                                 m_viewData.projectionMatrix,
                                                 m_viewData.viewportWidth,
                                                 m_viewData.viewportHeight))
        {
            m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
        }
        else
        {
            m_clusteredLightingStats.frameBegun = true;
            if (!m_clusteredLighting->AssignLights(*m_lightManager))
            {
                m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
            }
            else
            {
                m_clusteredLightingStats.lightsAssigned = true;
                const ClusteredLighting::Statistics clusteredStats = m_clusteredLighting->GetStatistics();
                m_clusteredLightingStats.clusterCount = clusteredStats.clusterCount;
                m_clusteredLightingStats.lightIndexCount = clusteredStats.lightIndexCount;
                m_clusteredLightingStats.activeClusters = clusteredStats.activeClusters;
                m_clusteredLightingStats.totalLightAssignments = clusteredStats.totalLightAssignments;
                m_clusteredLightingStats.maxLightsInCluster = clusteredStats.maxLightsInCluster;
                m_clusteredLightingStats.avgLightsPerCluster = clusteredStats.avgLightsPerCluster;

                if (!m_clusteredLighting->UploadFrameData())
                {
                    m_clusteredLightingStats.fallbackReason = m_clusteredLighting->GetLastError();
                }
                else
                {
                    m_clusteredLightingStats.gpuBuffersUploaded = true;
                }
            }
        }
    }

    for (const RenderLight& light : m_renderScene.GetLights())
    {
        if (light.type != RenderLight::Type::Directional || light.intensity <= 0.0f)
            continue;

        m_viewData.directionalLightDirection = light.direction;
        m_viewData.directionalLightIntensity = light.intensity;
        m_viewData.directionalLightColor = light.color;

        if (light.castsShadow)
        {
            if (m_shadowPass)
            {
                m_shadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);
            }
            if (m_rayTracedShadowPass)
            {
                m_rayTracedShadowPass->SetEnabled(true);
                m_rayTracedShadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);
            }
        }
        break;
    }

}

void SceneRenderer::EnsureDepthBuffer(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return;

    // Check if we need to create or resize the depth buffer
    if (!m_depthTexture || m_depthWidth != width || m_depthHeight != height)
    {
        IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
        if (!device)
            return;

        RVX_CORE_DEBUG("SceneRenderer: Creating depth buffer {}x{}", width, height);

        // Create depth texture
        RHITextureDesc depthDesc;
        depthDesc.width = width;
        depthDesc.height = height;
        depthDesc.depth = 1;
        depthDesc.mipLevels = 1;
        depthDesc.arraySize = 1;
        depthDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        depthDesc.dimension = RHITextureDimension::Texture2D;
        depthDesc.usage = RHITextureUsage::DepthStencil | RHITextureUsage::ShaderResource;
        depthDesc.SetOptimizedClearDepthStencil({
            m_pipelineCache ? m_pipelineCache->GetDepthClearValue()
                            : PipelineCache::GetDepthClearValue(false),
            0});
        depthDesc.debugName = "SceneDepthBuffer";

        m_depthTexture = device->CreateTexture(depthDesc);
        if (!m_depthTexture)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer");
            return;
        }

        // Create depth texture view
        RHITextureViewDesc viewDesc;
        viewDesc.format = PipelineCache::GetDefaultDepthStencilFormat();
        viewDesc.dimension = RHITextureDimension::Texture2D;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        viewDesc.type = RHITextureViewType::DepthStencil;
        viewDesc.debugName = "SceneDepthBufferView";

        m_depthTextureView = device->CreateTextureView(m_depthTexture.Get(), viewDesc);
        if (!m_depthTextureView)
        {
            RVX_CORE_ERROR("SceneRenderer: Failed to create depth buffer view");
            m_depthTexture.Reset();
            return;
        }

        m_depthWidth = width;
        m_depthHeight = height;
        m_depthAccessSnapshot = MakeRHITextureAccessSnapshot(
            RHIResourceState::Undefined,
            RHIShaderStage::None,
            GPUQueueDomain::Graphics,
            RHIContentValidity::Invalid);

        RVX_CORE_INFO("SceneRenderer: Created depth buffer {}x{}", width, height);
    }
}

void SceneRenderer::PrepareRayTracingScene()
{
    m_rayTracingSceneStats = {};

    if (!m_rayTracingSceneManager || !m_renderResourceRegistry)
    {
        m_rayTracingSceneStats.fallbackReason = "ray tracing scene dependencies are unavailable";
        return;
    }

    if (!m_rayTracingSceneManager->IsSupported())
    {
        m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
        m_rayTracingSceneStats.fallbackReason = "RHI device does not support ray tracing";
        return;
    }

    m_rayTracingSceneManager->SetBLASCacheEvictionFrameThreshold(
        m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold);
    m_rayTracingSceneManager->SetTrackedResourceBudget(
        m_rayTracingBudgetSettings.enabled ? m_rayTracingBudgetSettings.maxTrackedResourceBytes : 0u);

    RayTracingSceneOptions options;
    RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan(
        m_renderScene,
        m_visibleObjectIndices,
        *m_renderResourceRegistry,
        options);

    m_rayTracingSceneManager->Prepare(plan);
    m_rayTracingSceneStats = m_rayTracingSceneManager->GetStats();
}

void SceneRenderer::AddRayTracingSceneBuildPass()
{
    if (!m_rayTracingSceneManager || !m_rayTracingSceneStats.prepared)
        return;

    RHIBuffer* instanceBuffer = m_rayTracingSceneManager->GetInstanceBuffer();
    RHIBuffer* tlasScratch = m_rayTracingSceneManager->GetTopLevelScratchBuffer();
    if (!instanceBuffer || !tlasScratch)
        return;

    std::vector<RHIBuffer*> blasScratchBuffers;
    m_rayTracingSceneManager->GatherPendingBuildScratchBuffers(blasScratchBuffers);

    std::vector<RGBufferHandle> blasScratchHandles;
    blasScratchHandles.reserve(blasScratchBuffers.size());
    for (RHIBuffer* scratchBuffer : blasScratchBuffers)
    {
        if (!scratchBuffer)
            continue;

        RGBufferHandle scratchBufferHandle =
            m_renderGraph->ImportBuffer(scratchBuffer, RHIResourceState::Common);
        m_renderGraph->SetExportState(scratchBufferHandle, RHIResourceState::Common);
        blasScratchHandles.push_back(scratchBufferHandle);
    }

    struct RayTracingSceneBuildPassData
    {
        RayTracingSceneManager* manager = nullptr;
        RGBufferHandle instanceBuffer;
        std::vector<RGBufferHandle> blasScratchBuffers;
        RGBufferHandle tlasScratch;
    };

    RGBufferHandle instanceHandle =
        m_renderGraph->ImportBuffer(instanceBuffer, RHIResourceState::ShaderResource);
    RGBufferHandle scratchHandle =
        m_renderGraph->ImportBuffer(tlasScratch, RHIResourceState::Common);
    m_renderGraph->SetExportState(instanceHandle, RHIResourceState::ShaderResource);
    m_renderGraph->SetExportState(scratchHandle, RHIResourceState::Common);

    m_renderGraph->AddPass<RayTracingSceneBuildPassData>(
        "RayTracingSceneBuild",
        RenderGraphPassType::RayTracing,
        [this, instanceHandle, blasScratchHandles, scratchHandle](
            RenderGraphBuilder& builder,
            RayTracingSceneBuildPassData& data)
        {
            data.manager = m_rayTracingSceneManager.get();
            data.instanceBuffer = builder.Read(instanceHandle, RHIShaderStage::AllRayTracing);
            data.blasScratchBuffers.clear();
            data.blasScratchBuffers.reserve(blasScratchHandles.size());
            for (RGBufferHandle handle : blasScratchHandles)
            {
                data.blasScratchBuffers.push_back(builder.Write(handle, RHIResourceState::UnorderedAccess));
            }
            data.tlasScratch = builder.Write(scratchHandle, RHIResourceState::UnorderedAccess);
        },
        [](const RayTracingSceneBuildPassData& data, RHICommandContext& ctx)
        {
            if (data.manager)
            {
                data.manager->RecordBuildCommands(ctx);
            }
        });
}

void SceneRenderer::AddGPUDrivenCullingPass(
    const RenderPassRecordIdentity& recordIdentity)
{
    m_depthGPUCullingGraphHandles = {};
    m_opaqueGPUCullingGraphHandles = {};
    m_depthGPUCullingRecordedState.reset();
    m_opaqueGPUCullingRecordedState.reset();

    if (!m_renderGraph || !m_gpuDrivenCullingEnabled || !recordIdentity.IsValid())
    {
        return;
    }

    struct GPUDrivenCullPassData
    {
        RGBufferHandle constants;
        RGBufferHandle instances;
        RGBufferHandle instanceIndices;
        RGBufferHandle visibility;
        RGBufferHandle visibleInstances;
        RGBufferHandle indirectDraws;
        RGBufferHandle drawCount;
    };

    GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
    IRHIDevice* device = m_renderContext ? m_renderContext->GetDevice() : nullptr;
    const RHIQueueTopology queueTopology = device
        ? device->GetCapabilities().queueTopology
        : RHIQueueTopology{};
    TryGetGPUQueueDomain(
        queueTopology,
        RHICommandQueueType::Graphics,
        graphicsDomain);
    // SceneRenderer currently submits RenderGraph through Execute(graphicsCtx),
    // so compute shader execution still belongs to the graphics physical domain.
    const GPUQueueDomain computeExecutionDomain = graphicsDomain;
    const Mat4 cullViewMatrix = m_viewData.viewMatrix;
    const Mat4 cullProjectionMatrix = m_viewData.projectionMatrix;
    SceneGPUDrivenCullingStats* const cullingStatsSink =
        &m_gpuDrivenCullingStats;
    const auto addPass =
        [this,
         graphicsDomain,
         computeExecutionDomain,
         cullViewMatrix,
         cullProjectionMatrix,
         cullingStatsSink,
         recordIdentity](
            const char* name,
            GPUCulling* owner,
            bool framePrepared,
            GPUCullingGraphHandles& outHandles,
            std::shared_ptr<GPUCullingRecordedState>& outRecordedState) -> bool
    {
        if (!framePrepared || owner == nullptr ||
            owner->GetInstanceCount() == 0)
        {
            return false;
        }

        std::shared_ptr<GPUCullingRecordedState> recordedState =
            owner->SealForGraph(GPUCullingRecordingIdentity{
                recordIdentity.graphIdentity,
                recordIdentity.graphRecordingGeneration,
                recordIdentity.frameSequence,
                recordIdentity.viewOrdinal,
                recordIdentity.recordEpoch});
        if (!recordedState || !recordedState->IsValid())
        {
            RVX_RENDER_WARN("SceneRenderer: failed to seal {} GPU culling inputs", name);
            return false;
        }

        const GPUCulling& recordedCulling = recordedState->GetCulling();

        RHIBuffer* constantsBuffer = recordedCulling.GetCullingConstantsBuffer();
        RHIBuffer* instanceBuffer = recordedCulling.GetInstanceBuffer();
        RHIBuffer* instanceIndexBuffer = recordedCulling.GetInstanceIndexBuffer();
        RHIBuffer* visibilityBuffer = recordedCulling.GetVisibilityBuffer();
        RHIBuffer* visibleInstanceBuffer = recordedCulling.GetVisibleInstanceBuffer();
        RHIBuffer* indirectDrawBuffer = recordedCulling.GetIndirectBuffer();
        RHIBuffer* drawCountBuffer = recordedCulling.GetDrawCountBuffer();
        if (!constantsBuffer || !instanceBuffer || !instanceIndexBuffer ||
            !visibilityBuffer || !visibleInstanceBuffer ||
            !indirectDrawBuffer || !drawCountBuffer)
        {
            return false;
        }

        const GPUCullingAccessSnapshots& accessSnapshots =
            recordedState->GetAccessSnapshots();
        GPUDrivenCullPassData handles;
        handles.constants = m_renderGraph->ImportBuffer(
            constantsBuffer, accessSnapshots.constants);
        handles.instances = m_renderGraph->ImportBuffer(
            instanceBuffer, accessSnapshots.instances);
        handles.instanceIndices = m_renderGraph->ImportBuffer(
            instanceIndexBuffer, accessSnapshots.instanceIndices);
        handles.visibility = m_renderGraph->ImportBuffer(
            visibilityBuffer, accessSnapshots.visibility);
        handles.visibleInstances = m_renderGraph->ImportBuffer(
            visibleInstanceBuffer, accessSnapshots.visibleInstances);
        handles.indirectDraws = m_renderGraph->ImportBuffer(
            indirectDrawBuffer, accessSnapshots.indirectDraws);
        handles.drawCount = m_renderGraph->ImportBuffer(
            drawCountBuffer, accessSnapshots.drawCount);

        m_renderGraph->SetExportAccess(
            handles.constants,
            MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                  RHIShaderStage::Compute,
                                  computeExecutionDomain));
        m_renderGraph->SetExportAccess(
            handles.instances,
            MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                  RHIShaderStage::AllGraphics,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.instanceIndices,
            MakeRHIAccessSnapshot(RHIResourceState::VertexBuffer,
                                  RHIShaderStage::Vertex,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.visibility,
            MakeRHIAccessSnapshot(RHIResourceState::UnorderedAccess,
                                  RHIShaderStage::Compute,
                                  computeExecutionDomain));
        m_renderGraph->SetExportAccess(
            handles.visibleInstances,
            MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                  RHIShaderStage::AllGraphics,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.indirectDraws,
            MakeRHIAccessSnapshot(RHIResourceState::IndirectArgument,
                                  RHIShaderStage::None,
                                  graphicsDomain));
        m_renderGraph->SetExportAccess(
            handles.drawCount,
            MakeRHIAccessSnapshot(RHIResourceState::IndirectArgument,
                                  RHIShaderStage::None,
                                  graphicsDomain));
        outHandles = {handles.constants,
                      handles.instances,
                      handles.instanceIndices,
                      handles.visibility,
                      handles.visibleInstances,
                      handles.indirectDraws,
                      handles.drawCount};
        outRecordedState = recordedState;

        m_renderGraph->AddPass<GPUDrivenCullPassData>(
            name,
            RenderGraphPassType::Compute,
            [handles, computeExecutionDomain](RenderGraphBuilder& builder,
                                               GPUDrivenCullPassData& data)
            {
                data = handles;
                data.constants = builder.Read(
                    data.constants,
                    MakeRHIAccessSnapshot(RHIResourceState::ConstantBuffer,
                                          RHIShaderStage::Compute,
                                          computeExecutionDomain));
                data.instances = builder.Read(
                    data.instances,
                    MakeRHIAccessSnapshot(RHIResourceState::ShaderResource,
                                          RHIShaderStage::Compute,
                                          computeExecutionDomain));
                const RHIAccessSnapshot unorderedAccess = MakeRHIAccessSnapshot(
                    RHIResourceState::UnorderedAccess,
                    RHIShaderStage::Compute,
                    computeExecutionDomain);
                data.visibility = builder.Write(data.visibility, unorderedAccess);
                data.visibleInstances = builder.Write(
                    data.visibleInstances, unorderedAccess);
                data.indirectDraws = builder.Write(data.indirectDraws, unorderedAccess);
                data.drawCount = builder.Write(data.drawCount, unorderedAccess);
            },
            [recordedState,
             cullViewMatrix,
             cullProjectionMatrix,
             cullingStatsSink](const GPUDrivenCullPassData&, RHICommandContext& ctx)
            {
                recordedState->Cull(ctx, cullViewMatrix, cullProjectionMatrix);
                cullingStatsSink->graphPassRecorded = true;
                cullingStatsSink->executionDecisionAvailable = true;
                cullingStatsSink->executionDecision =
                    recordedState->GetCulling().GetExecutionDecision();
                cullingStatsSink->gpuExecutionRecorded =
                    cullingStatsSink->gpuExecutionRecorded ||
                    recordedState->GetCulling().WasGpuExecutionUsedLastCull();
                cullingStatsSink->fallbackUsed =
                    cullingStatsSink->fallbackUsed ||
                    recordedState->GetCulling().WasCpuFallbackUsedLastCull();
            });
        return true;
    };

    const bool depthAdded = addPass("GPUDrivenDepthCull",
                                    m_depthGPUCulling.get(),
                                    m_depthGPUCullingFramePrepared,
                                    m_depthGPUCullingGraphHandles,
                                    m_depthGPUCullingRecordedState);
    const bool opaqueAdded = addPass("GPUDrivenOpaqueCull",
                                     m_opaqueGPUCulling.get(),
                                     m_opaqueGPUCullingFramePrepared,
                                     m_opaqueGPUCullingGraphHandles,
                                     m_opaqueGPUCullingRecordedState);
    m_gpuDrivenCullingStats.graphPassAdded = depthAdded || opaqueAdded;
    m_gpuDrivenCullingStats.gpuCullingGraphPassCount =
        (depthAdded ? 1u : 0u) + (opaqueAdded ? 1u : 0u);
}

void SceneRenderer::CommitGPUDrivenAccessSnapshots()
{
    if (!m_renderGraph)
    {
        return;
    }
    const auto commit = [this](const std::shared_ptr<GPUCullingRecordedState>& recordedState,
                               const GPUCullingGraphHandles& handles)
    {
        if (!recordedState || !handles.IsValid())
        {
            return;
        }
        GPUCullingAccessSnapshots snapshots;
        snapshots.constants = m_renderGraph->GetRealizedAccess(handles.constants);
        snapshots.instances = m_renderGraph->GetRealizedAccess(handles.instances);
        snapshots.instanceIndices = m_renderGraph->GetRealizedAccess(handles.instanceIndices);
        snapshots.visibility = m_renderGraph->GetRealizedAccess(handles.visibility);
        snapshots.visibleInstances = m_renderGraph->GetRealizedAccess(handles.visibleInstances);
        snapshots.indirectDraws = m_renderGraph->GetRealizedAccess(handles.indirectDraws);
        snapshots.drawCount = m_renderGraph->GetRealizedAccess(handles.drawCount);
        recordedState->CommitAccessSnapshots(snapshots);
    };
    commit(m_depthGPUCullingRecordedState, m_depthGPUCullingGraphHandles);
    commit(m_opaqueGPUCullingRecordedState, m_opaqueGPUCullingGraphHandles);
}

void SceneRenderer::BuildRenderGraph()
{
    // Store RenderGraph and ViewCache pointers in ViewData so passes can access resources
    m_viewData.renderGraph = m_renderGraph.get();
    m_viewData.viewCache = m_resourceViewCache.get();
    m_viewData.velocityTarget = {};
    m_depthGraphHandle = {};
    m_backBufferGraphHandle = {};
    m_activeBackBufferIndex = RVX_INVALID_INDEX;
    const uint64 postProcessFrameCount = m_postProcessStats.frameCount + 1;
    m_postProcessStats = {};
    m_postProcessStats.frameCount = postProcessFrameCount;
    m_externalRenderTargetStats = {};
    m_externalRenderTargetStats.requested =
        m_externalRenderTarget.colorTarget != nullptr || m_externalRenderTarget.depthTarget != nullptr;

    RGTextureHandle backBufferTarget;
    RGTextureHandle sceneColorTarget;

    if (m_externalRenderTarget.IsValid())
    {
        RHITexture* colorTarget = m_externalRenderTarget.colorTarget;
        backBufferTarget = m_renderGraph->ImportTexture(colorTarget, m_externalRenderTarget.colorInitialState);
        m_viewData.colorTarget = backBufferTarget;
        m_renderGraph->SetExportState(backBufferTarget, m_externalRenderTarget.colorFinalState);

        m_externalRenderTargetStats.active = true;
        m_externalRenderTargetStats.importedColor = true;
        m_externalRenderTargetStats.width = colorTarget->GetWidth();
        m_externalRenderTargetStats.height = colorTarget->GetHeight();
        m_externalRenderTargetStats.colorFormat = colorTarget->GetFormat();
        m_externalRenderTargetStats.colorInitialState = m_externalRenderTarget.colorInitialState;
        m_externalRenderTargetStats.colorFinalState = m_externalRenderTarget.colorFinalState;

        if (m_viewData.viewportWidth == 0 || m_viewData.viewportHeight == 0)
        {
            m_viewData.viewportWidth = colorTarget->GetWidth();
            m_viewData.viewportHeight = colorTarget->GetHeight();
        }
    }
    else
    {
        if (m_externalRenderTargetStats.requested)
        {
            m_externalRenderTargetStats.fallbackReason = "External color target is missing or has zero extent";
        }

        // Import back buffer from swap chain
        RHISwapChain* swapChain = m_renderContext ? m_renderContext->GetSwapChain() : nullptr;
        if (swapChain)
        {
            // Track swapchain state - reset if resized or recreated
            uint32_t currentWidth = swapChain->GetWidth();
            uint32_t currentHeight = swapChain->GetHeight();
            uint32_t bufferCount = swapChain->GetBufferCount();

            if (m_backBufferAccessSnapshots.size() != bufferCount ||
                m_lastSwapChainWidth != currentWidth ||
                m_lastSwapChainHeight != currentHeight)
            {
                // Swap chain was recreated, reset all buffer states to Undefined
                m_backBufferAccessSnapshots.assign(
                    bufferCount,
                    MakeRHITextureAccessSnapshot(
                        RHIResourceState::Undefined,
                        RHIShaderStage::None,
                        GPUQueueDomain::Graphics,
                        RHIContentValidity::Invalid));
                m_lastSwapChainWidth = currentWidth;
                m_lastSwapChainHeight = currentHeight;
            }

            RHITexture* backBuffer = m_renderContext->GetCurrentBackBuffer();
            if (backBuffer)
            {
                // Get the current state for this back buffer
                // First use: Undefined, subsequent uses: Present (after presentation)
                uint32_t backBufferIndex = swapChain->GetCurrentBackBufferIndex();
                // Import with the actual lifetime snapshot (invalid on first use,
                // Present after a realized presentation export).
                backBufferTarget = m_renderGraph->ImportTexture(
                    backBuffer,
                    m_backBufferAccessSnapshots[backBufferIndex]);
                m_viewData.colorTarget = backBufferTarget;
                // Export back to Present state for display
                GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
                TryGetGPUQueueDomain(
                    m_renderContext->GetDevice()->GetCapabilities().queueTopology,
                    RHICommandQueueType::Graphics,
                    graphicsDomain);
                m_renderGraph->SetExportAccess(
                    backBufferTarget,
                    MakeRHIAccessSnapshot(RHIResourceState::Present,
                                          RHIShaderStage::None,
                                          graphicsDomain));
                m_backBufferGraphHandle = backBufferTarget;
                m_activeBackBufferIndex = backBufferIndex;
            }
        }
    }

    bool importedExternalDepth = false;
    if (m_externalRenderTargetStats.active && m_externalRenderTarget.depthTarget)
    {
        RHITexture* depthTarget = m_externalRenderTarget.depthTarget;
        const bool matchingExtent =
            depthTarget->GetWidth() == m_externalRenderTargetStats.width &&
            depthTarget->GetHeight() == m_externalRenderTargetStats.height &&
            depthTarget->GetWidth() > 0 &&
            depthTarget->GetHeight() > 0;
        if (matchingExtent)
        {
            m_viewData.depthTarget = m_renderGraph->ImportTexture(depthTarget, m_externalRenderTarget.depthInitialState);
            m_renderGraph->SetExportState(m_viewData.depthTarget, m_externalRenderTarget.depthFinalState);
            m_externalRenderTargetStats.importedDepth = true;
            m_externalRenderTargetStats.depthFormat = depthTarget->GetFormat();
            m_externalRenderTargetStats.depthInitialState = m_externalRenderTarget.depthInitialState;
            m_externalRenderTargetStats.depthFinalState = m_externalRenderTarget.depthFinalState;
            importedExternalDepth = true;
        }
        else
        {
            m_externalRenderTargetStats.fallbackReason =
                "External depth target dimensions do not match the color target";
        }
    }

    // Ensure we have a depth buffer and import it into the RenderGraph
    if (!importedExternalDepth)
    {
        EnsureDepthBuffer(m_viewData.viewportWidth, m_viewData.viewportHeight);
        if (m_depthTexture)
        {
            // Import the existing depth buffer so RenderGraph can manage its barriers
            m_viewData.depthTarget = m_renderGraph->ImportTexture(
                m_depthTexture.Get(),
                m_depthAccessSnapshot);
            GPUQueueDomain graphicsDomain = GPUQueueDomain::Graphics;
            TryGetGPUQueueDomain(
                m_renderContext->GetDevice()->GetCapabilities().queueTopology,
                RHICommandQueueType::Graphics,
                graphicsDomain);
            m_renderGraph->SetExportAccess(
                m_viewData.depthTarget,
                MakeRHIAccessSnapshot(RHIResourceState::DepthWrite,
                                      RHIShaderStage::None,
                                      graphicsDomain));
            m_depthGraphHandle = m_viewData.depthTarget;
        }
    }

    const bool velocityTargetRequested =
        (m_cameraVelocityPass && m_cameraVelocityPass->IsEnabled()) ||
        (m_objectVelocityPass && m_objectVelocityPass->IsEnabled());
    if (velocityTargetRequested &&
        m_viewData.depthTarget.IsValid() &&
        m_viewData.viewportWidth > 0 &&
        m_viewData.viewportHeight > 0)
    {
        RHITextureDesc velocityDesc = RHITextureDesc::Texture2D(
            m_viewData.viewportWidth,
            m_viewData.viewportHeight,
            RHIFormat::RG16_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        velocityDesc.debugName = "SceneCameraVelocity";
        m_viewData.velocityTarget = m_renderGraph->CreateTexture(velocityDesc);
    }
    if (m_postProcessStack)
    {
        m_postProcessStats.stackStats = m_postProcessStack->EvaluateEffects();
    }

    const RHITextureDesc* backBufferDesc = backBufferTarget.IsValid()
                                               ? m_renderGraph->GetTextureDesc(backBufferTarget)
                                               : nullptr;
    const RHIFormat backBufferFormat = backBufferDesc ? backBufferDesc->format : RHIFormat::Unknown;
    const bool postProcessActive = m_postProcessStats.stackStats.enabledEffectCount > 0 &&
                                   m_postProcessStats.stackStats.toneMappingBoundaryValid;
    m_sceneColorFormatPolicy = ResolveSceneColorFormatPolicy(backBufferFormat, postProcessActive);
    m_postProcessSettings.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;
    if (m_postProcessStack)
    {
        m_postProcessStack->ApplySettings(m_postProcessSettings);
        m_postProcessStats.stackStats = m_postProcessStack->EvaluateEffects();
    }

    m_postProcessStats.requestedSceneColorFormat = m_sceneColorFormatPolicy.requestedSceneColorFormat;
    m_postProcessStats.actualSceneColorFormat = m_sceneColorFormatPolicy.actualSceneColorFormat;
    m_postProcessStats.backBufferFormat = m_sceneColorFormatPolicy.backBufferFormat;
    m_postProcessStats.toneMappingOutputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
    m_postProcessStats.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;
    m_postProcessStats.hdrSceneColorEnabled = m_sceneColorFormatPolicy.hdrSceneColorEnabled;
    m_postProcessStats.hdrFallbackReason = m_sceneColorFormatPolicy.hdrFallbackReason;
    if (m_pipelineCache)
    {
        m_pipelineCache->SetRenderTargetFormats(m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.actualSceneColorFormat,
                                                m_sceneColorFormatPolicy.toneMappingOutputFormat);
    }
    m_postProcessStats.frameInputDepthAvailable = m_viewData.depthTarget.IsValid();
    m_postProcessStats.frameInputVelocityAvailable = m_viewData.velocityTarget.IsValid();
    m_postProcessStats.frameInputTemporalHistoryAvailable =
        !m_viewData.resetTemporalHistory && m_viewData.previousViewProjectionValid != 0;

    if (backBufferTarget.IsValid() && m_postProcessStats.stackStats.enabledEffectCount > 0)
    {
        if (backBufferDesc)
        {
            RHITextureDesc sceneColorDesc = *backBufferDesc;
            sceneColorDesc.format = m_sceneColorFormatPolicy.actualSceneColorFormat;
            sceneColorDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
            sceneColorDesc.SetOptimizedClearColor(RVX_SCENE_COLOR_CLEAR_VALUE);
            sceneColorDesc.debugName = "SceneColorPostProcessInput";

            sceneColorTarget = m_renderGraph->CreateTexture(sceneColorDesc);
            m_viewData.colorTarget = sceneColorTarget;
            m_postProcessStats.sceneColorStagingUsed = true;
            m_postProcessStats.directToBackBuffer = false;
            m_postProcessStats.sceneColorWidth = sceneColorDesc.width;
            m_postProcessStats.sceneColorHeight = sceneColorDesc.height;
            m_postProcessStats.sceneColorFormat = sceneColorDesc.format;
        }
        else
        {
            RVX_CORE_WARN("SceneRenderer: post-process requested but back buffer description is unavailable");
        }
    }

    AddRayTracingSceneBuildPass();

    // All main-chain raster passes record from this immutable, graph-specific
    // snapshot.  The epoch advances after every RenderGraph::Clear() / build;
    // a GPU slice from an older recording cannot be accepted by Depth/Opaque.
    RenderPassRecordContext passRecordContext;
    passRecordContext.view = m_viewData;
    passRecordContext.identity.graph = m_renderGraph.get();
    passRecordContext.identity.graphIdentity = m_renderGraph->GetGraphIdentity();
    passRecordContext.identity.graphRecordingGeneration =
        m_renderGraph->GetRecordingGeneration();
    passRecordContext.identity.frameSequence =
        m_viewData.renderFrameExecutionPlan != nullptr
            ? m_viewData.renderFrameExecutionPlan->frameSequence
            : m_renderScene.GetAcceptedHeader().sequence;
    passRecordContext.identity.viewOrdinal =
        m_viewData.renderFrameExecutionPlan != nullptr
            ? m_viewData.renderFrameExecutionPlan->viewOrdinal
            : 0;
    ++m_renderPassRecordEpoch;
    if (m_renderPassRecordEpoch == 0)
    {
        ++m_renderPassRecordEpoch;
    }
    passRecordContext.identity.recordEpoch = m_renderPassRecordEpoch;
    AddGPUDrivenCullingPass(passRecordContext.identity);
    passRecordContext.executionPlan = m_viewData.renderFrameExecutionPlan;
    passRecordContext.meshPassPreparation = m_viewData.meshPassPreparation;
    passRecordContext.visibility = m_viewData.renderVisibility;
    passRecordContext.executionReport = m_viewData.renderFrameExecutionReport;
    passRecordContext.renderScene = &m_renderScene;
    passRecordContext.opaqueDrawItems = &m_opaqueDrawItems;
    passRecordContext.maskedDrawItems = &m_maskedDrawItems;
    passRecordContext.results = std::make_shared<RenderPassRecordResults>();
    passRecordContext.results->identity = passRecordContext.identity;
    passRecordContext.results->directionalShadowOutput = {};
    passRecordContext.results->directionalShadowOutput.identity =
        passRecordContext.identity;
    passRecordContext.results->shadowStats = {};
    passRecordContext.frameSnapshot = MakeRenderPassFrameSnapshot(
        passRecordContext, *passRecordContext.results);
    m_activeRenderPassResults = passRecordContext.results;
    m_activeRenderPassIdentity = passRecordContext.identity;
    const auto makeGPUDrivenInputs =
        [&passRecordContext](const std::shared_ptr<GPUCullingRecordedState>& recordedState,
                             const GPUCullingGraphHandles& handles)
    {
        RenderPassGPUDrivenInputs inputs;
        inputs.identity = passRecordContext.identity;
        inputs.recordedState = handles.IsValid() ? recordedState : nullptr;
        inputs.instances = handles.instances;
        inputs.instanceIndices = handles.instanceIndices;
        inputs.indirectDraws = handles.indirectDraws;
        inputs.drawCount = handles.drawCount;
        return inputs;
    };
    passRecordContext.depthGPUDriven = makeGPUDrivenInputs(
        m_depthGPUCullingRecordedState, m_depthGPUCullingGraphHandles);
    passRecordContext.opaqueGPUDriven = makeGPUDrivenInputs(
        m_opaqueGPUCullingRecordedState, m_opaqueGPUCullingGraphHandles);

    // Register each render pass with the RenderGraph
    // Passes are sorted by priority, so they will be added in correct order
    if (!m_passRegistry)
        return;

    m_passChainStats.frameCount++;
    m_passChainStats.passStatuses = m_passRegistry->GetPassStatuses();
    m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();
    m_passChainStats.graphPassCount = 0;
    m_passChainStats.skippedDisabledPassCount = 0;
    m_passChainStats.skippedUnsupportedPassCount = 0;

    for (auto& pass : m_passRegistry->GetPasses())
    {
        if (!pass)
            continue;

        const RenderPassStatus status = pass->GetStatus();
        if (!status.requestedEnabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        if (!status.supported)
        {
            m_passChainStats.skippedUnsupportedPassCount++;
            const bool alreadyLogged = std::find(m_loggedUnsupportedPassNames.begin(),
                                                 m_loggedUnsupportedPassNames.end(),
                                                 status.name) != m_loggedUnsupportedPassNames.end();
            if (!alreadyLogged)
            {
                RVX_CORE_WARN("SceneRenderer: Skipping unsupported requested pass '{}': {}",
                              status.name,
                              status.unsupportedReason.empty() ? "Unsupported" : status.unsupportedReason);
                m_loggedUnsupportedPassNames.push_back(status.name);
            }
            continue;
        }

        if (!status.enabled)
        {
            m_passChainStats.skippedDisabledPassCount++;
            continue;
        }

        // The Shadow producer has registered its graph writes by the time the
        // opaque pass is reached. Consume only this recording's graph-owned
        // output; Opaque callbacks never query a mutable ShadowPass instance.
        if (pass.get() == m_opaquePass)
        {
            passRecordContext.directionalShadow =
                passRecordContext.results->directionalShadowOutput;
            if (passRecordContext.directionalShadow.identity !=
                passRecordContext.identity)
            {
                passRecordContext.directionalShadow = {};
                passRecordContext.directionalShadow.identity =
                    passRecordContext.identity;
            }

            passRecordContext.rayTracedShadow =
                passRecordContext.results->rayTracedShadowOutput;
            if (passRecordContext.rayTracedShadow.identity !=
                passRecordContext.identity)
            {
                passRecordContext.rayTracedShadow = {};
                passRecordContext.rayTracedShadow.identity =
                    passRecordContext.identity;
            }
        }

        // Shadow/Depth/Opaque consume the explicit graph-owned record context;
        // remaining passes retain the Task 9B compatibility adapter.
        if (pass.get() == m_shadowPass || pass.get() == m_depthPrepass ||
            pass.get() == m_rayTracedShadowPass || pass.get() == m_opaquePass)
        {
            pass->AddToGraph(*m_renderGraph, passRecordContext);
        }
        else
        {
            pass->AddToGraph(*m_renderGraph, m_viewData);
        }
        m_passChainStats.graphPassCount++;
    }

    if (m_postProcessStats.sceneColorStagingUsed && sceneColorTarget.IsValid() && backBufferTarget.IsValid() &&
        m_postProcessStack)
    {
        PostProcessFrameInputs frameInputs;
        frameInputs.sceneColor = sceneColorTarget;
        frameInputs.depth = m_viewData.depthTarget;
        frameInputs.velocity = m_viewData.velocityTarget;
        frameInputs.outputFormat = m_sceneColorFormatPolicy.toneMappingOutputFormat;
        frameInputs.frameIndex = m_viewData.frameNumber;
        frameInputs.currentViewProjection = m_viewData.viewProjectionMatrix;
        frameInputs.previousViewProjection = m_viewData.previousViewProjectionMatrix;
        frameInputs.currentViewProjectionValid = true;
        frameInputs.previousViewProjectionValid = m_viewData.previousViewProjectionValid != 0;
        frameInputs.resetTemporalHistory = m_viewData.resetTemporalHistory;
        frameInputs.submissionResourceBatch =
            m_viewData.submissionResourceBatch;
        m_postProcessStack->Execute(*m_renderGraph, frameInputs, backBufferTarget);
        m_postProcessStats.stackStats = m_postProcessStack->GetLastExecuteStats();
    }
}

void SceneRenderer::AddPass(std::unique_ptr<IRenderPass> pass)
{
    InvalidateRenderFramePlan();
    if (!m_passRegistry)
        m_passRegistry = std::make_unique<RenderPassRegistry>();

    m_passRegistry->AddPass(std::move(pass), m_renderContext ? m_renderContext->GetDevice() : nullptr);
}

void SceneRenderer::RefreshFrameDiagnosticsForTesting(bool graphBuilt)
{
    m_passChainStats = {};
    if (m_passRegistry)
    {
        m_passChainStats.passStatuses = m_passRegistry->GetPassStatuses();
        m_passChainStats.registeredPassCount = m_passChainStats.passStatuses.size();

        for (const RenderPassStatus& status : m_passChainStats.passStatuses)
        {
            if (!status.requestedEnabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            if (!status.supported)
            {
                ++m_passChainStats.skippedUnsupportedPassCount;
                continue;
            }

            if (!status.enabled)
            {
                ++m_passChainStats.skippedDisabledPassCount;
                continue;
            }

            ++m_passChainStats.graphPassCount;
        }
    }

    RefreshFrameDiagnostics(false, false, graphBuilt, false, nullptr);
}

bool SceneRenderer::RemovePass(const char* name)
{
    InvalidateRenderFramePlan();
    return m_passRegistry ? m_passRegistry->RemovePass(name) : false;
}

void SceneRenderer::ClearPasses()
{
    InvalidateRenderFramePlan();
    if (m_passRegistry)
        m_passRegistry->Clear();

    m_depthPrepass = nullptr;
    m_opaquePass = nullptr;
    m_shadowPass = nullptr;
    m_rayTracedShadowPass = nullptr;
    m_cameraVelocityPass = nullptr;
    m_objectVelocityPass = nullptr;
    m_rayTracedReflectionPass = nullptr;
    m_rayTracedReflectionDenoisePass = nullptr;
    m_rayTracedReflectionCompositePass = nullptr;
    m_transparentPass = nullptr;
    m_skyboxPass = nullptr;
    m_passChainStats = {};
    m_loggedUnsupportedPassNames.clear();
}

size_t SceneRenderer::GetPassCount() const
{
    return m_passRegistry ? m_passRegistry->GetPassCount() : 0;
}

bool SceneRenderer::AddPreGraphPrepareCallback(const void* owner, PreGraphPrepareCallback callback)
{
    if (!owner || !callback)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it != m_preGraphPrepareCallbacks.end())
        return false;

    InvalidateRenderFramePlan();

    PreGraphPrepareCallbackEntry entry;
    entry.owner = owner;
    entry.callback = std::move(callback);
    m_preGraphPrepareCallbacks.push_back(std::move(entry));
    return true;
}

bool SceneRenderer::RemovePreGraphPrepareCallback(const void* owner)
{
    if (!owner)
        return false;

    const auto it = std::find_if(
        m_preGraphPrepareCallbacks.begin(),
        m_preGraphPrepareCallbacks.end(),
        [owner](const PreGraphPrepareCallbackEntry& entry)
        {
            return entry.owner == owner;
        });
    if (it == m_preGraphPrepareCallbacks.end())
        return false;

    InvalidateRenderFramePlan();
    m_preGraphPrepareCallbacks.erase(it);
    return true;
}

void SceneRenderer::RunPreGraphPrepareCallbacks()
{
    const auto callbacks = m_preGraphPrepareCallbacks;
    for (const PreGraphPrepareCallbackEntry& entry : callbacks)
    {
        if (!entry.owner || !entry.callback)
            continue;

        const auto stillRegistered = std::find_if(
            m_preGraphPrepareCallbacks.begin(),
            m_preGraphPrepareCallbacks.end(),
            [&entry](const PreGraphPrepareCallbackEntry& current)
            {
                return current.owner == entry.owner;
            });
        if (stillRegistered == m_preGraphPrepareCallbacks.end())
            continue;

        entry.callback(m_viewData);
    }
}

void SceneRenderer::SetupDefaultPostProcess()
{
    if (!m_renderContext || !m_renderContext->GetDevice())
        return;

    m_postProcessStack = std::make_unique<PostProcessStack>();
    m_postProcessStack->Initialize(m_renderContext->GetDevice());
    m_bloomPostProcess = m_postProcessStack->AddEffect<BloomPass>();
    m_ssaoPostProcess = m_postProcessStack->AddEffect<SSAOPass>();
    m_toneMappingPostProcess = m_postProcessStack->AddEffect<ToneMappingPass>();
    m_colorGradingPostProcess = m_postProcessStack->AddEffect<ColorGradingPass>();
    m_chromaticAberrationPostProcess = m_postProcessStack->AddEffect<ChromaticAberrationPass>();
    m_vignettePostProcess = m_postProcessStack->AddEffect<VignettePass>();
    m_filmGrainPostProcess = m_postProcessStack->AddEffect<FilmGrainPass>();
    m_fxaaPostProcess = m_postProcessStack->AddEffect<FXAAPass>();

    if (m_bloomPostProcess)
    {
        m_bloomPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_ssaoPostProcess)
    {
        m_ssaoPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_toneMappingPostProcess)
    {
        m_toneMappingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_colorGradingPostProcess)
    {
        m_colorGradingPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_chromaticAberrationPostProcess)
    {
        m_chromaticAberrationPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_vignettePostProcess)
    {
        m_vignettePostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_filmGrainPostProcess)
    {
        m_filmGrainPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    if (m_fxaaPostProcess)
    {
        m_fxaaPostProcess->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    }

    ApplyPostProcessSettings(MakeDefaultRuntimePostProcessSettings());
}

void SceneRenderer::SetupDefaultPasses()
{
    auto depthPrepass = std::make_unique<DepthPrepass>();
    depthPrepass->SetResources(m_pipelineCache.get());
    depthPrepass->SetMaterialSystem(m_materialSystem.get());
    depthPrepass->SetResourceRegistry(m_renderResourceRegistry);
    m_depthPrepass = depthPrepass.get();
    AddPass(std::move(depthPrepass));

    auto shadowPass = std::make_unique<ShadowPass>();
    shadowPass->SetResources(m_pipelineCache.get());
    shadowPass->SetResourceRegistry(m_renderResourceRegistry);
    shadowPass->SetConfig(m_shadowPassConfig);
    m_shadowPass = shadowPass.get();
    AddPass(std::move(shadowPass));

    auto rayTracedShadowPass = std::make_unique<RayTracedShadowPass>();
    rayTracedShadowPass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedShadowPass->SetResourceRegistry(m_renderResourceRegistry);
    rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    rayTracedShadowPass->SetSubmissionTracker(
        m_renderContext
            ? RenderContextInternalAccess::GetSubmissionTracker(*m_renderContext)
            : nullptr);
    rayTracedShadowPass->SetConfig(m_shadowPassConfig);
    m_rayTracedShadowPass = rayTracedShadowPass.get();
    AddPass(std::move(rayTracedShadowPass));

    auto cameraVelocityPass = std::make_unique<CameraVelocityPass>();
    cameraVelocityPass->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());
    m_cameraVelocityPass = cameraVelocityPass.get();
    AddPass(std::move(cameraVelocityPass));
    auto objectVelocityPass = std::make_unique<ObjectVelocityPass>();
    objectVelocityPass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get(),
        m_materialSystem.get());
    objectVelocityPass->SetResourceRegistry(m_renderResourceRegistry);
    m_objectVelocityPass = objectVelocityPass.get();
    AddPass(std::move(objectVelocityPass));

    auto rayTracedReflectionPass = std::make_unique<RayTracedReflectionPass>();
    rayTracedReflectionPass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionPass->SetResourceRegistry(m_renderResourceRegistry);
    rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());
    m_rayTracedReflectionPass = rayTracedReflectionPass.get();
    AddPass(std::move(rayTracedReflectionPass));

    auto rayTracedReflectionDenoisePass = std::make_unique<RayTracedReflectionDenoisePass>();
    rayTracedReflectionDenoisePass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);
    m_rayTracedReflectionDenoisePass = rayTracedReflectionDenoisePass.get();
    AddPass(std::move(rayTracedReflectionDenoisePass));

    auto rayTracedReflectionCompositePass = std::make_unique<RayTracedReflectionCompositePass>();
    rayTracedReflectionCompositePass->SetResources(
        m_pipelineCache.get(),
        m_resourceViewCache.get());
    rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);
    rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);
    m_rayTracedReflectionCompositePass = rayTracedReflectionCompositePass.get();
    AddPass(std::move(rayTracedReflectionCompositePass));

    auto opaquePass = std::make_unique<OpaquePass>();
    opaquePass->SetResources(m_pipelineCache.get(),
                             m_materialSystem.get(),
                             m_lightManager.get(),
                             m_clusteredLighting.get());
    opaquePass->SetResourceRegistry(m_renderResourceRegistry);
    m_opaquePass = opaquePass.get();
    AddPass(std::move(opaquePass));

    auto skyboxPass = std::make_unique<SkyboxPass>();
    skyboxPass->SetResources(m_pipelineCache.get());
    m_skyboxPass = skyboxPass.get();
    AddPass(std::move(skyboxPass));

    auto transparentPass = std::make_unique<TransparentPass>();
    transparentPass->SetResources(m_pipelineCache.get(),
                                  m_materialSystem.get(),
                                  m_lightManager.get(),
                                  m_clusteredLighting.get());
    transparentPass->SetResourceRegistry(m_renderResourceRegistry);
    m_transparentPass = transparentPass.get();
    AddPass(std::move(transparentPass));

    auto particleFeaturePass = std::make_unique<ParticleFeaturePass>();
    particleFeaturePass->SetSnapshot(&m_featureSnapshot.particles);
    m_particleFeaturePass = particleFeaturePass.get();
    AddPass(std::move(particleFeaturePass));

    RVX_CORE_DEBUG("SceneRenderer: Default passes setup complete");
}

} // namespace RVX
