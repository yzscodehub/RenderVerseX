#include "Render/Passes/RayTracedShadowPass.h"

#include "RHI/RHIRayTracing.h"
#include "Render/GPUResourceManager.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX
{
    namespace
    {
        namespace RTShadowBindings = RayTracingResourceBindings::Shadow;

        constexpr uint64 RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT = 256;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME = 2;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_START_QUERY_OFFSET = 0;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_END_QUERY_OFFSET = 1;
        constexpr uint64 RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES = sizeof(uint64) * 2;

        RenderResourceHandle UnpackRenderResourceHandle(uint64 value)
        {
            return RenderResourceHandle{
                static_cast<uint32>(value >> 32U),
                static_cast<uint32>(value)};
        }

        uint32 GetRayTracedShadowTimingStartQuery(uint32 frameIndex)
        {
            return frameIndex * RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME +
                   RVX_RAY_TRACED_SHADOW_TIMING_START_QUERY_OFFSET;
        }

        uint32 GetRayTracedShadowTimingEndQuery(uint32 frameIndex)
        {
            return frameIndex * RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME +
                   RVX_RAY_TRACED_SHADOW_TIMING_END_QUERY_OFFSET;
        }

        uint64 AlignRayTracedShadowConstantBufferSize(uint64 size)
        {
            return (size + RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback)
        {
            const float lenSq = dot(value, value);
            if (!std::isfinite(lenSq) || lenSq <= 1.0e-8f)
            {
                return fallback;
            }
            return value * (1.0f / std::sqrt(lenSq));
        }

        float ClampFiniteNonNegative(float value, float fallback)
        {
            return std::isfinite(value) ? std::max(0.0f, value) : fallback;
        }

        struct RayTracedShadowGPUConstants
        {
            Mat4 inverseViewProjection = Mat4Identity();
            Mat4 previousViewProjection = Mat4Identity();
            Vec4 lightDirectionAndTMax{0.0f, 1.0f, 0.0f, 1000.0f};
            Vec4 viewportSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
            Vec4 depthAndBiasParams{0.0f, 0.02f, 0.0f, 0.75f};
            Vec4 historyReprojectionParams{0.01f, 0.85f, 0.0f, 0.0f};
            Vec4 softShadowParams{0.00465f, 0.0f, 1.0f, 0.0f};
            Vec4 rayOptions{255.0f, 0.0f, 0.0f, 0.0f};
        };

        bool NearlyEqual(const Vec3& a, const Vec3& b, float epsilon)
        {
            return std::abs(a.x - b.x) <= epsilon &&
                   std::abs(a.y - b.y) <= epsilon &&
                   std::abs(a.z - b.z) <= epsilon;
        }
        bool ConfigFloatChanged(float lhs, float rhs, float epsilon = 1.0e-6f)
        {
            return std::abs(lhs - rhs) > epsilon;
        }

        bool ShadowHistoryConfigChanged(const ShadowPassConfig& lhs, const ShadowPassConfig& rhs)
        {
            return ConfigFloatChanged(lhs.normalBias, rhs.normalBias) ||
                   ConfigFloatChanged(lhs.rayTracedLightAngularRadius, rhs.rayTracedLightAngularRadius) ||
                   lhs.rayTracedSamplesPerPixel != rhs.rayTracedSamplesPerPixel ||
                   lhs.rayTracedInstanceMask != rhs.rayTracedInstanceMask;
        }

    } // namespace

    void RayTracedShadowPass::OnAdd(IRHIDevice* device)
    {
        if (m_device != device)
        {
            ResetHistoryTextures();
            m_shadowMaskTexture = nullptr;
            m_constantBuffer.Reset();
            m_retainedDescriptorSets.clear();
            m_fallbackVelocityTexture.Reset();
            m_timingQueryPool.Reset();
            for (RHIBufferRef& readbackBuffer : m_timingReadbackBuffers)
            {
                readbackBuffer.Reset();
            }
            m_timingReadbackValid.fill(false);
        }
        m_device = device;

        if (m_device && m_device->GetCapabilities().supportsTimestampQueries)
        {
            RHIQueryPoolDesc queryDesc;
            queryDesc.type = RHIQueryType::Timestamp;
            queryDesc.count = RVX_MAX_FRAME_COUNT * RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME;
            queryDesc.debugName = "RayTracedShadowTimingQueries";
            m_timingQueryPool = m_device->CreateQueryPool(queryDesc);
            if (m_timingQueryPool)
            {
                for (uint32 frameIndex = 0; frameIndex < RVX_MAX_FRAME_COUNT; ++frameIndex)
                {
                    RHIBufferDesc readbackDesc;
                    readbackDesc.size = RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES;
                    readbackDesc.usage = RHIBufferUsage::CopyDst;
                    readbackDesc.memoryType = RHIMemoryType::Readback;
                    readbackDesc.debugName = "RayTracedShadowTimingReadback";
                    m_timingReadbackBuffers[frameIndex] = m_device->CreateBuffer(readbackDesc);
                }
            }
        }
    }

    void RayTracedShadowPass::OnRemove()
    {
        ResetHistoryTextures();
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_gpuResources = nullptr;
        m_resourceRegistry = nullptr;
        m_sceneManager = nullptr;
        m_shadowMaskHandle = {};
        m_depthReadHandle = {};
        m_velocityReadHandle = {};
        m_historyReadHandle = {};
        m_historyDepthReadHandle = {};
        m_historyDepthWriteHandle = {};
        m_historyNormalReadHandle = {};
        m_historyNormalWriteHandle = {};
        m_shadowMaskTexture = nullptr;
        m_fallbackVelocityTexture.Reset();
        m_timingQueryPool.Reset();
        for (RHIBufferRef& readbackBuffer : m_timingReadbackBuffers)
        {
            readbackBuffer.Reset();
        }
        m_timingReadbackValid.fill(false);
        m_constantBuffer.Reset();
        m_retainedDescriptorSets.clear();
        m_stats = {};
        m_enabled = false;
    }

    void RayTracedShadowPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
    {
        if (m_pipelineCache != pipelineCache)
        {
            m_fallbackVelocityTexture.Reset();
        }
        m_gpuResources = nullptr;
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    void RayTracedShadowPass::SetResources(GPUResourceManager* gpuResources,
                                           PipelineCache* pipelineCache,
                                           ResourceViewCache* viewCache)
    {
        if (m_pipelineCache != pipelineCache)
        {
            m_fallbackVelocityTexture.Reset();
        }
        m_gpuResources = gpuResources;
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool RayTracedShadowPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Ray traced shadows have not been requested";
            return false;
        }

        if (!m_device)
        {
            m_unsupportedReason = "Ray traced shadows require an RHI device";
            return false;
        }

        const RHICapabilities& caps = m_device->GetCapabilities();
        if (!caps.supportsRaytracing)
        {
            m_unsupportedReason = "RHI device does not support ray tracing";
            return false;
        }

        if (!caps.supportsRaytracingPipeline)
        {
            m_unsupportedReason = "RHI device does not support ray tracing pipelines";
            return false;
        }

        if (!m_sceneManager || !m_sceneManager->GetTopLevelAS())
        {
            m_unsupportedReason = "Ray tracing scene TLAS is not available";
            return false;
        }

        if (!m_sceneManager->GetInstanceAlphaMetadataBuffer())
        {
            m_unsupportedReason = "Ray tracing instance alpha metadata is not available";
            return false;
        }

        if (!m_sceneManager->GetInstanceMaterialMetadataBuffer())
        {
            m_unsupportedReason = "Ray tracing instance material metadata is not available";
            return false;
        }

        const size_t materialTextureCount = m_sceneManager->GetInstanceMaterialTextureTable().size();
        if (materialTextureCount > RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES)
        {
            m_unsupportedReason = "Ray tracing material texture table exceeds the supported descriptor count";
            return false;
        }

        if (materialTextureCount > 0 &&
            !m_gpuResources && !m_resourceRegistry)
        {
            m_unsupportedReason = "Ray tracing material textures require an exact or legacy resource resolver";
            return false;
        }

        const size_t alphaTextureCount = m_sceneManager->GetInstanceAlphaTextureTable().size();
        if (alphaTextureCount > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES)
        {
            m_unsupportedReason = "Ray tracing alpha texture table exceeds the supported descriptor count";
            return false;
        }

        if (m_sceneManager->GetInstanceAlphaIndexBufferTable().size() >
                RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS ||
            m_sceneManager->GetInstanceAlphaUVBufferTable().size() >
                RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS)
        {
            m_unsupportedReason = "Ray tracing alpha geometry buffer table exceeds the supported descriptor count";
            return false;
        }

        if (alphaTextureCount > 0 &&
            !m_gpuResources && !m_resourceRegistry)
        {
            m_unsupportedReason = "Ray tracing alpha textures require an exact or legacy resource resolver";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced shadows require an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced shadows require an initialized ResourceViewCache";
            return false;
        }

        if (!m_pipelineCache->GetRayTracedShadowPipeline() ||
            !m_pipelineCache->GetRayTracedShadowShaderTable() ||
            !m_pipelineCache->GetRayTracedShadowSetLayout())
        {
            m_unsupportedReason = "Ray traced shadow pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void RayTracedShadowPass::SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity)
    {
        m_lightDirection = direction;
        m_lightColor = color;
        m_lightIntensity = intensity;
    }

    void RayTracedShadowPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.tlasAvailable = m_sceneManager && m_sceneManager->GetTopLevelAS();
        m_stats.materialMetadataAvailable = m_sceneManager && m_sceneManager->GetInstanceMaterialMetadataBuffer();
        const size_t sceneMaterialTextureCount =
            m_sceneManager ? m_sceneManager->GetInstanceMaterialTextureTable().size() : 0u;
        m_stats.materialTextureCount = static_cast<uint32>(
            std::min<size_t>(sceneMaterialTextureCount, RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES));
        m_stats.materialTextureTableAvailable =
            m_stats.materialTextureCount == sceneMaterialTextureCount &&
            (m_stats.materialTextureCount == 0u || m_gpuResources != nullptr ||
             m_resourceRegistry != nullptr);
        m_stats.alphaMetadataAvailable = m_sceneManager && m_sceneManager->GetInstanceAlphaMetadataBuffer();
        const size_t sceneAlphaTextureCount =
            m_sceneManager ? m_sceneManager->GetInstanceAlphaTextureTable().size() : 0u;
        m_stats.alphaTextureCount = static_cast<uint32>(
            std::min<size_t>(sceneAlphaTextureCount, RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES));
        m_stats.alphaTextureTableAvailable =
            m_stats.alphaTextureCount == sceneAlphaTextureCount &&
            (m_stats.alphaTextureCount == 0u || m_gpuResources != nullptr ||
             m_resourceRegistry != nullptr);
        m_stats.alphaIndexBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaIndexBufferTable().size(),
                      RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.alphaUVBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaUVBufferTable().size(),
                      RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.alphaGeometryTableAvailable =
            !m_sceneManager ||
            (m_stats.alphaIndexBufferCount == m_sceneManager->GetInstanceAlphaIndexBufferTable().size() &&
             m_stats.alphaUVBufferCount == m_sceneManager->GetInstanceAlphaUVBufferTable().size());
        m_stats.depthAvailable = view.depthTarget.IsValid();
        m_stats.velocityAvailable = view.velocityTarget.IsValid();
        m_stats.historyReset = view.resetTemporalHistory;
        if (view.resetTemporalHistory)
        {
            m_historyValid = false;
            m_historyViewValid = false;
            m_pendingHistoryViewValid = false;
            m_temporalAccumulatedThisFrame = false;
        }
        const bool historyConfigChanged =
            m_lastHistoryConfigValid && ShadowHistoryConfigChanged(m_lastHistoryConfig, m_config);
        m_stats.historyConfigChanged = historyConfigChanged;
        if (historyConfigChanged)
        {
            m_stats.historyReset = true;
            m_historyValid = false;
            m_historyViewValid = false;
            m_pendingHistoryViewValid = false;
            m_temporalAccumulatedThisFrame = false;
        }
        m_stats.historyAvailable = m_historyValid;
        m_stats.depthHistoryAvailable = m_historyDepthTextures[0] && m_historyDepthTextures[1];
        m_stats.normalHistoryAvailable = m_historyNormalTextures[0] && m_historyNormalTextures[1];
        const uint32 timingFrameIndex = GetTimingFrameIndex();
        RHIBuffer* timingReadbackBuffer = GetTimingReadbackBuffer(timingFrameIndex);
        uint32 timingReadbackBufferCount = 0;
        for (const RHIBufferRef& readbackBuffer : m_timingReadbackBuffers)
        {
            if (readbackBuffer)
                ++timingReadbackBufferCount;
        }
        m_stats.gpuTimingSupported = m_timingQueryPool != nullptr;
        m_stats.gpuTimingReadbackBufferAvailable = timingReadbackBuffer != nullptr;
        m_stats.gpuTimingReadbackBytes = static_cast<uint64>(timingReadbackBufferCount) *
                                          RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES;
        m_stats.gpuTimingReadbackBufferCount = timingReadbackBufferCount;
        m_stats.gpuTimingReadbackFrameIndex = timingFrameIndex;
        m_stats.gpuTimingStartQueryIndex = GetRayTracedShadowTimingStartQuery(timingFrameIndex);
        m_stats.gpuTimingEndQueryIndex = GetRayTracedShadowTimingEndQuery(timingFrameIndex);
        m_stats.gpuTimestampFrequency = m_timingQueryPool ? m_timingQueryPool->GetTimestampFrequency() : 0;
        TryReadbackTimingResult(timingFrameIndex);
        m_stats.samplesPerPixel = std::min(std::max(m_config.rayTracedSamplesPerPixel, 1u), 8u);
        m_stats.width = view.viewportWidth;
        m_stats.height = view.viewportHeight;
        if (!TryGetRHIRayTracingDispatchRayCount(m_stats.width, m_stats.height, 1, m_stats.dispatchPixelCount))
        {
            m_stats.dispatchPixelCount = RVX_RAY_TRACING_MAX_RAY_COUNT;
        }
        if (!TryMultiplyRHIRayTracingCount(m_stats.dispatchPixelCount,
                                           static_cast<uint64>(m_stats.samplesPerPixel),
                                           m_stats.estimatedRayCount))
        {
            m_stats.estimatedRayCount = RVX_RAY_TRACING_MAX_RAY_COUNT;
        }
        m_shadowMaskHandle = {};
        m_depthReadHandle = {};
        m_velocityReadHandle = {};
        m_historyReadHandle = {};
        m_historyDepthReadHandle = {};
        m_historyDepthWriteHandle = {};
        m_historyNormalReadHandle = {};
        m_historyNormalWriteHandle = {};
        m_shadowMaskTexture = nullptr;
        m_temporalAccumulatedThisFrame = false;
        m_pendingHistoryViewValid = false;

        if (!m_stats.supported || !m_stats.depthAvailable ||
            view.viewportWidth == 0 || view.viewportHeight == 0 || !view.renderGraph)
            return;

        if (!EnsureHistoryTextures(view.viewportWidth, view.viewportHeight))
            return;



        RGTextureHandle depthHandle = view.depthTarget;
        depthHandle.hasSubresourceRange = true;
        depthHandle.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
        m_depthReadHandle = builder.Read(depthHandle, RHIShaderStage::AllRayTracing);
        if (view.velocityTarget.IsValid())
        {
            m_velocityReadHandle = builder.Read(view.velocityTarget, RHIShaderStage::AllRayTracing);
        }

        m_currentHistoryWriteIndex = m_historyWriteIndex;
        m_currentHistoryReadIndex = 1u - m_currentHistoryWriteIndex;

        m_historyReadHandle = view.renderGraph->ImportTexture(
            m_historyTextures[m_currentHistoryReadIndex].Get(),
            m_historyTextureStates[m_currentHistoryReadIndex]);
        m_historyReadHandle = builder.Read(m_historyReadHandle, RHIShaderStage::AllRayTracing);

        m_historyDepthReadHandle = view.renderGraph->ImportTexture(
            m_historyDepthTextures[m_currentHistoryReadIndex].Get(),
            m_historyDepthTextureStates[m_currentHistoryReadIndex]);
        m_historyDepthReadHandle = builder.Read(m_historyDepthReadHandle, RHIShaderStage::AllRayTracing);

        m_historyNormalReadHandle = view.renderGraph->ImportTexture(
            m_historyNormalTextures[m_currentHistoryReadIndex].Get(),
            m_historyNormalTextureStates[m_currentHistoryReadIndex]);
        m_historyNormalReadHandle = builder.Read(m_historyNormalReadHandle, RHIShaderStage::AllRayTracing);

        m_shadowMaskHandle = view.renderGraph->ImportTexture(
            m_historyTextures[m_currentHistoryWriteIndex].Get(),
            m_historyTextureStates[m_currentHistoryWriteIndex]);
        builder.Write(m_shadowMaskHandle, RHIResourceState::UnorderedAccess);
        view.renderGraph->SetExportState(m_shadowMaskHandle, RHIResourceState::ShaderResource);

        m_historyDepthWriteHandle = view.renderGraph->ImportTexture(
            m_historyDepthTextures[m_currentHistoryWriteIndex].Get(),
            m_historyDepthTextureStates[m_currentHistoryWriteIndex]);
        builder.Write(m_historyDepthWriteHandle, RHIResourceState::UnorderedAccess);
        view.renderGraph->SetExportState(m_historyDepthWriteHandle, RHIResourceState::ShaderResource);

        m_historyNormalWriteHandle = view.renderGraph->ImportTexture(
            m_historyNormalTextures[m_currentHistoryWriteIndex].Get(),
            m_historyNormalTextureStates[m_currentHistoryWriteIndex]);
        builder.Write(m_historyNormalWriteHandle, RHIResourceState::UnorderedAccess);
        view.renderGraph->SetExportState(m_historyNormalWriteHandle, RHIResourceState::ShaderResource);
        m_stats.outputDeclared = true;
    }

    void RayTracedShadowPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        if (!m_stats.outputDeclared ||
            !m_pipelineCache ||
            !m_sceneManager ||
            !view.renderGraph ||
            !m_viewCache ||
            !m_depthReadHandle.IsValid() ||
            !m_historyReadHandle.IsValid() ||
            !m_historyDepthReadHandle.IsValid() ||
            !m_historyDepthWriteHandle.IsValid() ||
            !m_historyNormalReadHandle.IsValid() ||
            !m_historyNormalWriteHandle.IsValid())
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHIPipeline* pipeline = m_pipelineCache->GetRayTracedShadowPipeline();
        RHIShaderTable* shaderTable = m_pipelineCache->GetRayTracedShadowShaderTable();
        RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetRayTracedShadowSetLayout();
        IRHIDevice* device = m_pipelineCache->GetDevice();
        if (!pipeline || !shaderTable || !setLayout || !device)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHIAccelerationStructure* tlas = m_sceneManager->GetTopLevelAS();
        RHIBuffer* materialMetadataBuffer = m_sceneManager->GetInstanceMaterialMetadataBuffer();
        RHIBuffer* alphaMetadataBuffer = m_sceneManager->GetInstanceAlphaMetadataBuffer();
        RHITexture* depthTexture = view.renderGraph->GetTexture(m_depthReadHandle);
        RHITexture* sceneVelocity = m_velocityReadHandle.IsValid() ? view.renderGraph->GetTexture(m_velocityReadHandle) : nullptr;
        RHITexture* previousShadowMask = view.renderGraph->GetTexture(m_historyReadHandle);
        RHITexture* previousDepthHistory = view.renderGraph->GetTexture(m_historyDepthReadHandle);
        RHITexture* currentDepthHistory = view.renderGraph->GetTexture(m_historyDepthWriteHandle);
        RHITexture* previousNormalHistory = view.renderGraph->GetTexture(m_historyNormalReadHandle);
        RHITexture* currentNormalHistory = view.renderGraph->GetTexture(m_historyNormalWriteHandle);
        RHITexture* shadowMask = view.renderGraph->GetTexture(m_shadowMaskHandle);
        if (!tlas || !materialMetadataBuffer || !alphaMetadataBuffer || !depthTexture || !previousShadowMask || !previousDepthHistory ||
            !currentDepthHistory || !previousNormalHistory || !currentNormalHistory || !shadowMask)
        {
            m_stats.dispatchRecorded = false;
            return;
        }
        if (!sceneVelocity)
        {
            if (!EnsureFallbackVelocityTexture())
            {
                m_stats.dispatchRecorded = false;
                return;
            }
            sceneVelocity = m_fallbackVelocityTexture.Get();
        }

        RHITextureView* shadowMaskUAV = m_viewCache->GetDefaultUAV(shadowMask);
        if (!shadowMaskUAV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* depthSRV = m_viewCache->GetDefaultSRV(depthTexture);
        if (!depthSRV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* sceneVelocitySRV = m_viewCache->GetDefaultSRV(sceneVelocity);
        if (!sceneVelocitySRV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* previousShadowMaskSRV = m_viewCache->GetDefaultSRV(previousShadowMask);
        if (!previousShadowMaskSRV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* previousDepthHistorySRV = m_viewCache->GetDefaultSRV(previousDepthHistory);
        if (!previousDepthHistorySRV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* currentDepthHistoryUAV = m_viewCache->GetDefaultUAV(currentDepthHistory);
        if (!currentDepthHistoryUAV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* previousNormalHistorySRV = m_viewCache->GetDefaultSRV(previousNormalHistory);
        if (!previousNormalHistorySRV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* currentNormalHistoryUAV = m_viewCache->GetDefaultUAV(currentNormalHistory);
        if (!currentNormalHistoryUAV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        std::vector<RHITextureView*> alphaTextureViews;
        std::vector<RHITextureView*> materialTextureViews;
        if (!ResolveMaterialTextureViews(materialTextureViews))
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        if (!ResolveAlphaTextureViews(alphaTextureViews))
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        std::vector<RHIBuffer*> alphaIndexBuffers;
        std::vector<RHIBuffer*> alphaUVBuffers;
        if (!ResolveAlphaGeometryBuffers(alphaIndexBuffers, alphaUVBuffers))
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        m_stats.resourceViewsAvailable = true;
        if (!EnsureConstantBuffer() || !UpdateConstants(view))
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = setLayout;
        descriptorDesc.debugName = "RayTracedShadowDescriptorSet";
        descriptorDesc.BindAccelerationStructure(RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING, tlas);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING, shadowMaskUAV);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, depthSRV);
        descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING,
                                  m_constantBuffer.Get(),
                                  0,
                                  AlignRayTracedShadowConstantBufferSize(sizeof(RayTracedShadowGPUConstants)));
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, previousShadowMaskSRV);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, previousDepthHistorySRV);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, currentDepthHistoryUAV);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, previousNormalHistorySRV);
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, currentNormalHistoryUAV);
        descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, alphaMetadataBuffer);
        for (uint32 textureIndex = 0; textureIndex < alphaTextureViews.size(); ++textureIndex)
        {
            descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING, alphaTextureViews[textureIndex], textureIndex);
        }
        for (uint32 bufferIndex = 0; bufferIndex < alphaIndexBuffers.size(); ++bufferIndex)
        {
            descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING, alphaIndexBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);
        }
        for (uint32 bufferIndex = 0; bufferIndex < alphaUVBuffers.size(); ++bufferIndex)
        {
            descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING, alphaUVBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);
        }
        descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, materialMetadataBuffer);
        for (uint32 textureIndex = 0; textureIndex < materialTextureViews.size(); ++textureIndex)
        {
            descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING, materialTextureViews[textureIndex], textureIndex);
        }
        descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, sceneVelocitySRV);

        RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            m_stats.dispatchRecorded = false;
            return;
        }
        m_stats.descriptorSetAvailable = true;

        m_retainedDescriptorSets.push_back(descriptorSet);
        while (m_retainedDescriptorSets.size() > RVX_MAX_FRAME_COUNT + 1)
        {
            m_retainedDescriptorSets.pop_front();
        }

        ctx.SetPipeline(pipeline);
        ctx.SetDescriptorSet(0, descriptorSet.Get());
        RHIDispatchRaysDesc dispatchDesc;
        dispatchDesc.shaderTable = shaderTable;
        dispatchDesc.width = shadowMask->GetWidth();
        dispatchDesc.height = shadowMask->GetHeight();
        dispatchDesc.depth = 1;
        const uint32 timingFrameIndex = GetTimingFrameIndex();
        const uint32 timingStartQuery = GetRayTracedShadowTimingStartQuery(timingFrameIndex);
        const uint32 timingEndQuery = GetRayTracedShadowTimingEndQuery(timingFrameIndex);
        RHIBuffer* timingReadbackBuffer = GetTimingReadbackBuffer(timingFrameIndex);
        if (m_timingQueryPool)
        {
            ctx.WriteTimestamp(m_timingQueryPool.Get(), timingStartQuery);
        }
        ctx.DispatchRays(dispatchDesc);
        if (m_timingQueryPool)
        {
            ctx.WriteTimestamp(m_timingQueryPool.Get(), timingEndQuery);
            m_stats.gpuTimingQueriesRecorded = true;
            if (timingReadbackBuffer)
            {
                ctx.ResolveQueries(m_timingQueryPool.Get(),
                                   timingStartQuery,
                                   RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME,
                                   timingReadbackBuffer,
                                   0);
                m_timingReadbackValid[timingFrameIndex] = true;
                m_stats.gpuTimingResolveRecorded = true;
            }
        }
        m_shadowMaskTexture = shadowMask;
        m_historyTextureStates[m_currentHistoryReadIndex] = RHIResourceState::ShaderResource;
        m_historyTextureStates[m_currentHistoryWriteIndex] = RHIResourceState::ShaderResource;
        m_historyDepthTextureStates[m_currentHistoryReadIndex] = RHIResourceState::ShaderResource;
        m_historyDepthTextureStates[m_currentHistoryWriteIndex] = RHIResourceState::ShaderResource;
        m_historyNormalTextureStates[m_currentHistoryReadIndex] = RHIResourceState::ShaderResource;
        m_historyNormalTextureStates[m_currentHistoryWriteIndex] = RHIResourceState::ShaderResource;
        m_historyValid = true;
        m_lastHistoryConfig = m_config;
        m_lastHistoryConfigValid = true;
        m_historyViewValid = m_pendingHistoryViewValid;
        if (m_pendingHistoryViewValid)
        {
            m_lastHistoryViewProjection = m_pendingHistoryViewProjection;
            m_lastHistoryRayDirection = m_pendingHistoryRayDirection;
        }
        m_historyWriteIndex = 1u - m_currentHistoryWriteIndex;
        m_stats.historyAvailable = true;
        m_stats.depthHistoryAvailable = true;
        m_stats.normalHistoryAvailable = true;
        m_stats.temporalAccumulated = m_temporalAccumulatedThisFrame;
        m_stats.materialTexturesBound = static_cast<uint32>(materialTextureViews.size());
        m_stats.alphaTexturesBound = static_cast<uint32>(alphaTextureViews.size());
        m_stats.alphaIndexBufferCount = static_cast<uint32>(alphaIndexBuffers.size());
        m_stats.alphaUVBufferCount = static_cast<uint32>(alphaUVBuffers.size());
        m_stats.dispatchRecorded = true;
    }

    bool RayTracedShadowPass::EnsureConstantBuffer()
    {
        if (m_constantBuffer)
            return true;

        if (!m_device)
            return false;

        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignRayTracedShadowConstantBufferSize(sizeof(RayTracedShadowGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "RayTracedShadowConstants";

        m_constantBuffer = m_device->CreateBuffer(bufferDesc);
        return m_constantBuffer != nullptr;
    }

    uint32 RayTracedShadowPass::GetTimingFrameIndex() const
    {
        return m_device ? (m_device->GetCurrentFrameIndex() % RVX_MAX_FRAME_COUNT) : 0;
    }

    RHIBuffer* RayTracedShadowPass::GetTimingReadbackBuffer(uint32 frameIndex) const
    {
        if (frameIndex >= RVX_MAX_FRAME_COUNT)
            return nullptr;

        return m_timingReadbackBuffers[frameIndex].Get();
    }

    void RayTracedShadowPass::TryReadbackTimingResult(uint32 frameIndex)
    {
        if (frameIndex >= RVX_MAX_FRAME_COUNT || !m_timingReadbackValid[frameIndex])
            return;

        RHIBuffer* readbackBuffer = GetTimingReadbackBuffer(frameIndex);
        if (!readbackBuffer || readbackBuffer->GetSize() < RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES ||
            m_stats.gpuTimestampFrequency == 0)
        {
            m_timingReadbackValid[frameIndex] = false;
            return;
        }

        void* mapped = readbackBuffer->Map();
        if (!mapped)
        {
            m_timingReadbackValid[frameIndex] = false;
            return;
        }

        uint64 timestamps[2] = {};
        std::memcpy(timestamps, mapped, sizeof(timestamps));
        readbackBuffer->Unmap();
        m_timingReadbackValid[frameIndex] = false;

        if (timestamps[1] < timestamps[0])
            return;

        m_stats.gpuTimingResultAvailable = true;
        m_stats.gpuTimingStartTimestamp = timestamps[0];
        m_stats.gpuTimingEndTimestamp = timestamps[1];
        m_stats.gpuTimingElapsedTicks = timestamps[1] - timestamps[0];
        m_stats.gpuTimingElapsedMs = static_cast<float>(
            (static_cast<double>(m_stats.gpuTimingElapsedTicks) * 1000.0) /
            static_cast<double>(m_stats.gpuTimestampFrequency));
    }

    bool RayTracedShadowPass::EnsureHistoryTextures(uint32 width, uint32 height)
    {
        if (!m_device || width == 0 || height == 0)
            return false;

        const bool hasCompleteHistory =
            m_historyTextures[0] && m_historyTextures[1] &&
            m_historyDepthTextures[0] && m_historyDepthTextures[1] &&
            m_historyNormalTextures[0] && m_historyNormalTextures[1];
        const bool historyResolutionChanged =
            hasCompleteHistory && (m_historyWidth != width || m_historyHeight != height);
        if (hasCompleteHistory && !historyResolutionChanged)
        {
            return true;
        }

        m_stats.historyRecreated = true;
        m_stats.historyResolutionChanged = historyResolutionChanged;
        m_stats.historyReset = true;
        m_stats.historyAvailable = false;
        m_stats.depthHistoryAvailable = false;
        m_stats.normalHistoryAvailable = false;

        ResetHistoryTextures();

        RHITextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.format = RHIFormat::R8_UNORM;
        desc.dimension = RHITextureDimension::Texture2D;
        desc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;

        desc.debugName = "RayTracedShadowHistory0";
        m_historyTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedShadowHistory1";
        m_historyTextures[1] = m_device->CreateTexture(desc);

        desc.format = RHIFormat::R32_FLOAT;
        desc.debugName = "RayTracedShadowDepthHistory0";
        m_historyDepthTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedShadowDepthHistory1";
        m_historyDepthTextures[1] = m_device->CreateTexture(desc);

        desc.format = RHIFormat::RGBA16_FLOAT;
        desc.debugName = "RayTracedShadowNormalHistory0";
        m_historyNormalTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedShadowNormalHistory1";
        m_historyNormalTextures[1] = m_device->CreateTexture(desc);

        if (!m_historyTextures[0] || !m_historyTextures[1] ||
            !m_historyDepthTextures[0] || !m_historyDepthTextures[1] ||
            !m_historyNormalTextures[0] || !m_historyNormalTextures[1])
        {
            ResetHistoryTextures();
            return false;
        }

        m_historyWidth = width;
        m_historyHeight = height;
        m_historyTextureStates[0] = RHIResourceState::Common;
        m_historyTextureStates[1] = RHIResourceState::Common;
        m_historyDepthTextureStates[0] = RHIResourceState::Common;
        m_historyDepthTextureStates[1] = RHIResourceState::Common;
        m_historyNormalTextureStates[0] = RHIResourceState::Common;
        m_historyNormalTextureStates[1] = RHIResourceState::Common;
        return true;
    }

    bool RayTracedShadowPass::EnsureFallbackVelocityTexture()
    {
        if (m_fallbackVelocityTexture)
            return true;

        IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : m_device;
        if (!device)
            return false;

        RHITextureDesc desc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RG16_FLOAT);
        desc.debugName = "RayTracedShadowFallbackVelocity";
        m_fallbackVelocityTexture = device->CreateTexture(desc);
        return m_fallbackVelocityTexture != nullptr;
    }

    void RayTracedShadowPass::ResetHistoryTextures()
    {
        if (m_viewCache)
        {
            for (RHITextureRef& texture : m_historyTextures)
            {
                if (texture)
                {
                    m_viewCache->InvalidateTexture(texture.Get());
                }
            }
            for (RHITextureRef& texture : m_historyDepthTextures)
            {
                if (texture)
                {
                    m_viewCache->InvalidateTexture(texture.Get());
                }
            }
            for (RHITextureRef& texture : m_historyNormalTextures)
            {
                if (texture)
                {
                    m_viewCache->InvalidateTexture(texture.Get());
                }
            }
        }

        m_historyTextures[0].Reset();
        m_historyTextures[1].Reset();
        m_historyDepthTextures[0].Reset();
        m_historyDepthTextures[1].Reset();
        m_historyNormalTextures[0].Reset();
        m_historyNormalTextures[1].Reset();
        m_historyTextureStates[0] = RHIResourceState::Common;
        m_historyTextureStates[1] = RHIResourceState::Common;
        m_historyDepthTextureStates[0] = RHIResourceState::Common;
        m_historyDepthTextureStates[1] = RHIResourceState::Common;
        m_historyNormalTextureStates[0] = RHIResourceState::Common;
        m_historyNormalTextureStates[1] = RHIResourceState::Common;
        m_historyWidth = 0;
        m_historyHeight = 0;
        m_historyWriteIndex = 0;
        m_currentHistoryReadIndex = 1;
        m_currentHistoryWriteIndex = 0;
        m_historyDepthReadHandle = {};
        m_historyDepthWriteHandle = {};
        m_historyNormalReadHandle = {};
        m_historyNormalWriteHandle = {};
        m_historyValid = false;
        m_historyViewValid = false;
        m_pendingHistoryViewValid = false;
        m_lastHistoryConfigValid = false;
        m_temporalAccumulatedThisFrame = false;
    }

    bool RayTracedShadowPass::UpdateConstants(const ViewData& view)
    {
        if (!m_constantBuffer)
            return false;

        const Vec3 shadowRayDirection = NormalizeOr(-m_lightDirection, Vec3(0.0f, 1.0f, 0.0f));
        const float maxTraceDistance = std::max(1.0f, ClampFiniteNonNegative(view.farPlane, 1000.0f));
        const float originBias = std::max(0.001f, ClampFiniteNonNegative(m_config.normalBias, 0.02f));
        const bool reverseZ = m_pipelineCache && m_pipelineCache->GetConfig().reverseZ;
        const float width = static_cast<float>(std::max(1u, view.viewportWidth));
        const float height = static_cast<float>(std::max(1u, view.viewportHeight));
        const bool stableHistoryLight = m_historyViewValid &&
                                        NearlyEqual(m_lastHistoryRayDirection, shadowRayDirection, 1.0e-4f);
        const bool useHistory = m_historyValid &&
                                m_config.rayTracedTemporalAccumulation &&
                                stableHistoryLight;
        const float temporalBlend = std::min(
            ClampFiniteNonNegative(m_config.rayTracedTemporalBlendFactor, 0.75f),
            0.95f);
        const float depthRejectionThreshold = std::min(
            ClampFiniteNonNegative(m_config.rayTracedHistoryDepthThreshold, 0.01f),
            0.1f);
        const float normalRejectionThreshold = std::min(
            ClampFiniteNonNegative(m_config.rayTracedHistoryNormalThreshold, 0.85f),
            1.0f);
        const float velocityRejectionScale = std::min(
            ClampFiniteNonNegative(m_config.rayTracedHistoryVelocityRejectionScale, 8.0f),
            64.0f);
        const float lightAngularRadius = std::min(
            ClampFiniteNonNegative(m_config.rayTracedLightAngularRadius, 0.00465f),
            0.25f);
        const uint32 samplesPerPixel = std::min(std::max(m_config.rayTracedSamplesPerPixel, 1u), 8u);
        const uint32 rayMask = std::min<uint32>(m_config.rayTracedInstanceMask, 0xFFu);
        const float frameSeed = static_cast<float>(view.frameNumber & 0x00FFFFFFull);

        RayTracedShadowGPUConstants constants;
        constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;
        constants.previousViewProjection = m_historyViewValid ? m_lastHistoryViewProjection : view.viewProjectionMatrix;
        constants.lightDirectionAndTMax = Vec4(shadowRayDirection, maxTraceDistance);
        constants.viewportSizeAndInvSize = Vec4(width, height, 1.0f / width, 1.0f / height);
        constants.depthAndBiasParams = Vec4(
            reverseZ ? 1.0f : 0.0f,
            originBias,
            useHistory ? 1.0f : 0.0f,
            temporalBlend);
        constants.historyReprojectionParams = Vec4(
            depthRejectionThreshold,
            normalRejectionThreshold,
            view.velocityTarget.IsValid() ? 1.0f : 0.0f,
            velocityRejectionScale);
        constants.softShadowParams = Vec4(lightAngularRadius, frameSeed, static_cast<float>(samplesPerPixel), 0.0f);
        constants.rayOptions = Vec4(static_cast<float>(rayMask), 0.0f, 0.0f, 0.0f);

        void* mapped = m_constantBuffer->Map();
        if (!mapped)
            return false;

        std::memcpy(mapped, &constants, sizeof(constants));
        m_constantBuffer->Unmap();
        m_pendingHistoryViewProjection = view.viewProjectionMatrix;
        m_pendingHistoryRayDirection = shadowRayDirection;
        m_pendingHistoryViewValid = true;
        m_temporalAccumulatedThisFrame = useHistory;
        m_stats.constantsUploaded = true;
        return true;
    }

    bool RayTracedShadowPass::ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const
    {
        outViews.clear();
        if (!m_sceneManager)
            return false;

        const std::vector<uint64>& textureIds =
            m_sceneManager->GetInstanceMaterialTextureTable();
        if (textureIds.empty())
            return true;

        if ((!m_gpuResources && !m_resourceRegistry) || !m_viewCache)
            return false;

        if (textureIds.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES)
            return false;

        outViews.reserve(textureIds.size());
        for (uint64 textureId : textureIds)
        {
            RHITexture* texture = m_resourceRegistry
                ? m_resourceRegistry->ResolveTextureObject(
                      UnpackRenderResourceHandle(textureId))
                : m_gpuResources->GetTexture(textureId);
            if (!texture)
                return false;

            RHITextureView* view = m_viewCache->GetDefaultSRV(texture);
            if (!view)
                return false;

            outViews.push_back(view);
        }

        return true;
    }

    bool RayTracedShadowPass::ResolveAlphaTextureViews(std::vector<RHITextureView*>& outViews) const
    {
        outViews.clear();
        if (!m_sceneManager)
            return false;

        const std::vector<uint64>& textureIds =
            m_sceneManager->GetInstanceAlphaTextureTable();
        if (textureIds.empty())
            return true;

        if ((!m_gpuResources && !m_resourceRegistry) || !m_viewCache)
            return false;

        if (textureIds.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES)
            return false;

        outViews.reserve(textureIds.size());
        for (uint64 textureId : textureIds)
        {
            RHITexture* texture = m_resourceRegistry
                ? m_resourceRegistry->ResolveTextureObject(
                      UnpackRenderResourceHandle(textureId))
                : m_gpuResources->GetTexture(textureId);
            if (!texture)
                return false;

            RHITextureView* view = m_viewCache->GetDefaultSRV(texture);
            if (!view)
                return false;

            outViews.push_back(view);
        }

        return true;
    }

    bool RayTracedShadowPass::ResolveAlphaGeometryBuffers(
        std::vector<RHIBuffer*>& outIndexBuffers,
        std::vector<RHIBuffer*>& outUVBuffers) const
    {
        outIndexBuffers.clear();
        outUVBuffers.clear();
        if (!m_sceneManager)
            return false;

        const std::vector<RHIBuffer*>& indexBuffers = m_sceneManager->GetInstanceAlphaIndexBufferTable();
        const std::vector<RHIBuffer*>& uvBuffers = m_sceneManager->GetInstanceAlphaUVBufferTable();
        if (indexBuffers.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS ||
            uvBuffers.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS)
        {
            return false;
        }

        outIndexBuffers.reserve(indexBuffers.size());
        for (RHIBuffer* buffer : indexBuffers)
        {
            if (!buffer)
                return false;

            outIndexBuffers.push_back(buffer);
        }

        outUVBuffers.reserve(uvBuffers.size());
        for (RHIBuffer* buffer : uvBuffers)
        {
            if (!buffer)
                return false;

            outUVBuffers.push_back(buffer);
        }

        return true;
    }

} // namespace RVX
