#include "Render/Passes/RayTracedReflectionPass.h"

#include "RHI/RHIRayTracing.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderSubmissionResourceBatch.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX
{
    void RayTracedReflectionPass::RetireOwnerSnapshots(
        const GPUCompletionToken& completion,
        RenderRetirementQueue& retirement)
    {
        FlushRenderOwnerRetirements(
            m_pendingOwnerRetirements, completion, retirement);
    }

    namespace
    {
        namespace RTReflectionBindings = RayTracingResourceBindings::Reflection;

        constexpr uint64 RVX_RAY_TRACED_REFLECTION_CONSTANT_BUFFER_ALIGNMENT = 256;
        constexpr uint32 RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME = 2;
        constexpr uint32 RVX_RAY_TRACED_REFLECTION_TIMING_START_QUERY_OFFSET = 0;
        constexpr uint32 RVX_RAY_TRACED_REFLECTION_TIMING_END_QUERY_OFFSET = 1;
        constexpr uint64 RVX_RAY_TRACED_REFLECTION_TIMING_READBACK_BYTES = sizeof(uint64) * 2;

        RenderResourceHandle UnpackRenderResourceHandle(uint64 value)
        {
            return RenderResourceHandle{
                static_cast<uint32>(value >> 32U),
                static_cast<uint32>(value)};
        }

        uint32 GetRayTracedReflectionTimingStartQuery(uint32 frameIndex)
        {
            return frameIndex * RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME +
                   RVX_RAY_TRACED_REFLECTION_TIMING_START_QUERY_OFFSET;
        }

        uint32 GetRayTracedReflectionTimingEndQuery(uint32 frameIndex)
        {
            return frameIndex * RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME +
                   RVX_RAY_TRACED_REFLECTION_TIMING_END_QUERY_OFFSET;
        }

        uint64 AlignRayTracedReflectionConstantBufferSize(uint64 size)
        {
            return (size + RVX_RAY_TRACED_REFLECTION_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_RAY_TRACED_REFLECTION_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct RayTracedReflectionGPUConstants
        {
            Mat4 inverseViewProjection = Mat4Identity();
            Mat4 previousViewProjection = Mat4Identity();
            Vec4 cameraPositionAndTMax{0.0f, 0.0f, 0.0f, 1000.0f};
            Vec4 outputSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
            Vec4 sceneSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
            Vec4 reflectionOptions{1.0f, 1.0f, 255.0f, 0.0f};
            Vec4 historyParams{0.0f, 0.85f, 0.01f, 0.85f};
            Vec4 stochasticParams{1.0f, 0.0f, 1.0f, 0.0f};
            Vec4 rayBiasParams{0.02f, 0.001f, 64.0f, 0.0f};
            Vec4 historyClampParams{4.0f, 0.05f, 0.0f, 8.0f};
        };

        float ClampFiniteNonNegative(float value, float fallback)
        {
            return std::isfinite(value) ? std::max(0.0f, value) : fallback;
        }

        float ClampFiniteRange(float value, float fallback, float minValue, float maxValue)
        {
            return std::isfinite(value) ? std::clamp(value, minValue, maxValue) : fallback;
        }

        float ResolveReflectionMaxTraceDistance(float distance, float farPlane)
        {
            const float finiteFarPlane = std::isfinite(farPlane) ? farPlane : 50.0f;
            const float maxDistance = std::max(1.0f, finiteFarPlane);
            return ClampFiniteRange(distance, 50.0f, 1.0f, maxDistance);
        }

        uint32 ResolveReflectionSamplesPerPixel(uint32 samplesPerPixel)
        {
            return std::clamp(samplesPerPixel, 1u, 4u);
        }

        float ResolveReflectionResolutionScale(float scale)
        {
            if (!std::isfinite(scale))
                return 1.0f;

            return std::clamp(scale, 0.25f, 1.0f);
        }

        uint32 ResolveScaledReflectionDimension(uint32 dimension, float scale)
        {
            const float scaled = std::ceil(static_cast<float>(std::max(1u, dimension)) * scale);
            return std::max(1u, static_cast<uint32>(scaled));
        }
        bool ConfigFloatChanged(float lhs, float rhs, float epsilon = 1.0e-6f)
        {
            return std::abs(lhs - rhs) > epsilon;
        }

        bool ReflectionHistoryConfigChanged(const RayTracedReflectionPassConfig& lhs,
                                            const RayTracedReflectionPassConfig& rhs)
        {
            return ConfigFloatChanged(lhs.intensity, rhs.intensity) ||
                   ConfigFloatChanged(lhs.maxRoughness, rhs.maxRoughness) ||
                   ConfigFloatChanged(lhs.maxTraceDistance, rhs.maxTraceDistance) ||
                   ConfigFloatChanged(lhs.distanceFadeStart, rhs.distanceFadeStart) ||
                   lhs.instanceMask != rhs.instanceMask ||
                   lhs.samplesPerPixel != rhs.samplesPerPixel ||
                   ConfigFloatChanged(lhs.roughnessConeSpread, rhs.roughnessConeSpread) ||
                   ConfigFloatChanged(lhs.normalBias, rhs.normalBias) ||
                   ConfigFloatChanged(lhs.rayMinT, rhs.rayMinT) ||
                   ConfigFloatChanged(lhs.fireflyClamp, rhs.fireflyClamp);
        }

    } // namespace

    void RayTracedReflectionPass::OnAdd(IRHIDevice* device)
    {
        if (m_device != device)
        {
            ResetHistoryTextures();
            m_constantBuffer.Reset();
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
            queryDesc.count = RVX_MAX_FRAME_COUNT * RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME;
            queryDesc.debugName = "RayTracedReflectionTimingQueries";
            m_timingQueryPool = m_device->CreateQueryPool(queryDesc);
            if (m_timingQueryPool)
            {
                for (uint32 frameIndex = 0; frameIndex < RVX_MAX_FRAME_COUNT; ++frameIndex)
                {
                    RHIBufferDesc readbackDesc;
                    readbackDesc.size = RVX_RAY_TRACED_REFLECTION_TIMING_READBACK_BYTES;
                    readbackDesc.usage = RHIBufferUsage::CopyDst;
                    readbackDesc.memoryType = RHIMemoryType::Readback;
                    readbackDesc.debugName = "RayTracedReflectionTimingReadback";
                    m_timingReadbackBuffers[frameIndex] = m_device->CreateBuffer(readbackDesc);
                }
            }
        }
    }

    void RayTracedReflectionPass::OnRemove()
    {
        ResetHistoryTextures();
        m_device = nullptr;
        m_resourceRegistry = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_sceneManager = nullptr;
        m_reflectionHandle = {};
        m_sceneColorReadHandle = {};
        m_depthReadHandle = {};
        m_velocityReadHandle = {};
        m_historyReadHandle = {};
        m_historyDepthReadHandle = {};
        m_historyDepthWriteHandle = {};
        m_historyNormalReadHandle = {};
        m_historyNormalWriteHandle = {};
        m_reflectionViewHandle = {};
        m_sceneColorViewHandle = {};
        m_depthViewHandle = {};
        m_velocityViewHandle = {};
        m_historyViewHandle = {};
        m_historyDepthReadViewHandle = {};
        m_historyDepthWriteViewHandle = {};
        m_historyNormalReadViewHandle = {};
        m_historyNormalWriteViewHandle = {};
        m_reflectionTexture.Reset();
        m_fallbackVelocityTexture.Reset();
        m_timingQueryPool.Reset();
        for (RHIBufferRef& readbackBuffer : m_timingReadbackBuffers)
        {
            readbackBuffer.Reset();
        }
        m_timingReadbackValid.fill(false);
        m_constantBuffer.Reset();
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_stats = {};
        m_enabled = false;
    }

    void RayTracedReflectionPass::SetResources(PipelineCache* pipelineCache,
                                               ResourceViewCache* viewCache)
    {
        if (m_pipelineCache != pipelineCache)
        {
            m_fallbackVelocityTexture.Reset();
        }
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool RayTracedReflectionPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Ray traced reflections have not been requested";
            return false;
        }

        if (!m_device)
        {
            m_unsupportedReason = "Ray traced reflections require an RHI device";
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

        if (!m_sceneManager->GetInstanceMaterialMetadataBuffer())
        {
            m_unsupportedReason = "Ray tracing instance material metadata is not available";
            return false;
        }

        if (!m_sceneManager->GetInstanceAlphaMetadataBuffer())
        {
            m_unsupportedReason = "Ray tracing instance geometry metadata is not available";
            return false;
        }

        const size_t materialTextureCount = m_sceneManager->GetInstanceMaterialTextureTable().size();
        if (materialTextureCount > RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES)
        {
            m_unsupportedReason = "Ray tracing material texture table exceeds the supported descriptor count";
            return false;
        }

        if (materialTextureCount > 0 && !m_resourceRegistry)
        {
            m_unsupportedReason = "Ray tracing material textures require an exact resource registry";
            return false;
        }

        if (m_sceneManager->GetInstanceAlphaIndexBufferTable().size() >
                RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            m_sceneManager->GetInstanceAlphaUVBufferTable().size() >
                RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            m_sceneManager->GetInstanceAlphaNormalBufferTable().size() >
                RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            m_sceneManager->GetInstanceAlphaTangentBufferTable().size() >
                RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)
        {
            m_unsupportedReason = "Ray tracing geometry buffer table exceeds the supported descriptor count";
            return false;
        }

        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflections require an initialized PipelineCache";
            return false;
        }

        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason = "Ray traced reflections require an initialized ResourceViewCache";
            return false;
        }

        if (!m_pipelineCache->GetRayTracedReflectionPipeline() ||
            !m_pipelineCache->GetRayTracedReflectionShaderTable() ||
            !m_pipelineCache->GetRayTracedReflectionSetLayout())
        {
            m_unsupportedReason = "Ray traced reflection pipeline resources are not available";
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    void RayTracedReflectionPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        m_stats = {};
        m_stats.requested = m_enabled;
        m_stats.supported = IsSupported();
        m_stats.tlasAvailable = m_sceneManager && m_sceneManager->GetTopLevelAS();
        m_stats.materialMetadataAvailable = m_sceneManager && m_sceneManager->GetInstanceMaterialMetadataBuffer();
        const size_t sceneMaterialTextureCount =
            m_sceneManager ? m_sceneManager->GetInstanceMaterialTextureTable().size() : 0u;
        m_stats.materialTextureCount = static_cast<uint32>(
            std::min<size_t>(sceneMaterialTextureCount, RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES));
        m_stats.materialTextureTableAvailable =
            m_stats.materialTextureCount == sceneMaterialTextureCount &&
            (m_stats.materialTextureCount == 0u || m_resourceRegistry != nullptr);
        m_stats.geometryMetadataAvailable = m_sceneManager && m_sceneManager->GetInstanceAlphaMetadataBuffer();
        m_stats.geometryIndexBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaIndexBufferTable().size(),
                      RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.geometryUVBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaUVBufferTable().size(),
                      RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.geometryNormalBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaNormalBufferTable().size(),
                      RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.geometryTangentBufferCount =
            m_sceneManager
                ? static_cast<uint32>(std::min<size_t>(
                      m_sceneManager->GetInstanceAlphaTangentBufferTable().size(),
                      RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS))
                : 0u;
        m_stats.geometryTableAvailable =
            !m_sceneManager ||
            (m_stats.geometryIndexBufferCount == m_sceneManager->GetInstanceAlphaIndexBufferTable().size() &&
             m_stats.geometryUVBufferCount == m_sceneManager->GetInstanceAlphaUVBufferTable().size() &&
             m_stats.geometryNormalBufferCount == m_sceneManager->GetInstanceAlphaNormalBufferTable().size() &&
             m_stats.geometryTangentBufferCount == m_sceneManager->GetInstanceAlphaTangentBufferTable().size());
        m_stats.sceneColorAvailable = view.colorTarget.IsValid();
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
            m_lastHistoryConfigValid && ReflectionHistoryConfigChanged(m_lastHistoryConfig, m_config);
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
                                          RVX_RAY_TRACED_REFLECTION_TIMING_READBACK_BYTES;
        m_stats.gpuTimingReadbackBufferCount = timingReadbackBufferCount;
        m_stats.gpuTimingReadbackFrameIndex = timingFrameIndex;
        m_stats.gpuTimingStartQueryIndex = GetRayTracedReflectionTimingStartQuery(timingFrameIndex);
        m_stats.gpuTimingEndQueryIndex = GetRayTracedReflectionTimingEndQuery(timingFrameIndex);
        m_stats.gpuTimestampFrequency = m_timingQueryPool ? m_timingQueryPool->GetTimestampFrequency() : 0;
        TryReadbackTimingResult(timingFrameIndex);
        const float resolutionScale = ResolveReflectionResolutionScale(m_config.resolutionScale);
        const uint32 outputWidth = ResolveScaledReflectionDimension(view.viewportWidth, resolutionScale);
        const uint32 outputHeight = ResolveScaledReflectionDimension(view.viewportHeight, resolutionScale);
        m_stats.resolutionScale = resolutionScale;
        m_stats.samplesPerPixel = ResolveReflectionSamplesPerPixel(m_config.samplesPerPixel);
        m_stats.intensity = ClampFiniteRange(m_config.intensity, 1.0f, 0.0f, 1.0f);
        m_stats.maxTraceDistance = ResolveReflectionMaxTraceDistance(m_config.maxTraceDistance, view.farPlane);
        m_stats.maxRoughness = ClampFiniteRange(m_config.maxRoughness, 1.0f, 0.01f, 1.0f);
        m_stats.distanceFadeStart = ClampFiniteRange(m_config.distanceFadeStart, 0.8f, 0.0f, 1.0f);
        m_stats.roughnessConeSpread = ClampFiniteRange(m_config.roughnessConeSpread, 1.0f, 0.0f, 1.0f);
        m_stats.normalBias = std::min(ClampFiniteNonNegative(m_config.normalBias, 0.02f), 1.0f);
        m_stats.rayMinT = std::min(ClampFiniteNonNegative(m_config.rayMinT, 0.001f), 1.0f);
        m_stats.fireflyClamp = ClampFiniteNonNegative(m_config.fireflyClamp, 64.0f);
        m_stats.temporalBlendFactor = ClampFiniteRange(m_config.temporalBlendFactor, 0.85f, 0.0f, 0.95f);
        m_stats.historyDepthThreshold = ClampFiniteRange(m_config.historyDepthThreshold, 0.01f, 0.0f, 0.1f);
        m_stats.historyNormalThreshold = ClampFiniteRange(m_config.historyNormalThreshold, 0.85f, 0.0f, 1.0f);
        m_stats.historyLuminanceTolerance = ClampFiniteNonNegative(m_config.historyLuminanceTolerance, 4.0f);
        m_stats.historyConfidenceThreshold =
            ClampFiniteRange(m_config.historyConfidenceThreshold, 0.05f, 0.0f, 1.0f);
        m_stats.historyVelocityRejectionScale =
            ClampFiniteRange(m_config.historyVelocityRejectionScale, 8.0f, 0.0f, 64.0f);
        if (!TryGetRHIRayTracingDispatchRayCount(outputWidth, outputHeight, 1, m_stats.dispatchPixelCount))
        {
            m_stats.dispatchPixelCount = RVX_RAY_TRACING_MAX_RAY_COUNT;
        }
        if (!TryMultiplyRHIRayTracingCount(m_stats.dispatchPixelCount,
                                           static_cast<uint64>(m_stats.samplesPerPixel),
                                           m_stats.estimatedRayCount))
        {
            m_stats.estimatedRayCount = RVX_RAY_TRACING_MAX_RAY_COUNT;
        }
        m_stats.width = outputWidth;
        m_stats.height = outputHeight;
        m_reflectionHandle = {};
        m_sceneColorReadHandle = {};
        m_depthReadHandle = {};
        m_velocityReadHandle = {};
        m_historyReadHandle = {};
        m_historyDepthReadHandle = {};
        m_historyDepthWriteHandle = {};
        m_historyNormalReadHandle = {};
        m_historyNormalWriteHandle = {};
        m_reflectionViewHandle = {};
        m_sceneColorViewHandle = {};
        m_depthViewHandle = {};
        m_velocityViewHandle = {};
        m_historyViewHandle = {};
        m_historyDepthReadViewHandle = {};
        m_historyDepthWriteViewHandle = {};
        m_historyNormalReadViewHandle = {};
        m_historyNormalWriteViewHandle = {};
        m_temporalAccumulatedThisFrame = false;
        m_pendingHistoryViewValid = false;

        if (!m_stats.supported ||
            !m_stats.sceneColorAvailable ||
            !m_stats.depthAvailable ||
            view.viewportWidth == 0 ||
            view.viewportHeight == 0)
        {
            return;
        }

        if (!EnsureHistoryTextures(outputWidth, outputHeight))
            return;

        m_currentHistoryWriteIndex = m_historyWriteIndex;
        m_currentHistoryReadIndex = 1u - m_currentHistoryWriteIndex;

        const auto createReadView = [&builder](
                                        RGTextureHandle texture,
                                        const char* debugName)
        {
            const RHITextureDesc* desc = builder.GetTextureDesc(texture);
            if (!desc)
                return RGTextureViewHandle{};
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = texture.hasSubresourceRange
                ? texture.subresourceRange : RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            RGTextureViewHandle handle = builder.CreateTextureView(
                texture, viewDesc);
            return builder.Read(
                handle,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::AllRayTracing));
        };
        const auto createWriteView = [&builder](
                                         RGTextureHandle texture,
                                         const char* debugName)
        {
            const RHITextureDesc* desc = builder.GetTextureDesc(texture);
            if (!desc)
                return RGTextureViewHandle{};
            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::UnorderedAccess;
            viewDesc.debugName = debugName;
            RGTextureViewHandle handle = builder.CreateTextureView(
                texture, viewDesc);
            return builder.Write(
                handle,
                MakeRGAccessDesc(
                    RHIResourceState::UnorderedAccess,
                    RHIShaderStage::AllRayTracing));
        };

        m_sceneColorReadHandle = view.colorTarget;
        m_sceneColorViewHandle = createReadView(
            m_sceneColorReadHandle, "RayTracedReflectionSceneColorSRV");

        RGTextureHandle depthHandle = view.depthTarget;
        depthHandle.hasSubresourceRange = true;
        depthHandle.subresourceRange = RHISubresourceRange{
            0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
        m_depthReadHandle = depthHandle;
        m_depthViewHandle = createReadView(
            m_depthReadHandle, "RayTracedReflectionDepthSRV");

        if (view.velocityTarget.IsValid())
        {
            m_velocityReadHandle = view.velocityTarget;
        }
        else
        {
            if (!EnsureFallbackVelocityTexture())
                return;
            m_velocityReadHandle = builder.ImportTexture(
                m_fallbackVelocityTexture,
                RHIResourceState::ShaderResource);
        }
        m_velocityViewHandle = createReadView(
            m_velocityReadHandle, "RayTracedReflectionVelocitySRV");

        m_historyReadHandle = builder.ImportTexture(
            m_historyTextures[m_currentHistoryReadIndex],
            m_historyTextureStates[m_currentHistoryReadIndex]);
        m_historyViewHandle = createReadView(
            m_historyReadHandle, "RayTracedReflectionPreviousSRV");

        m_historyDepthReadHandle = builder.ImportTexture(
            m_historyDepthTextures[m_currentHistoryReadIndex],
            m_historyDepthTextureStates[m_currentHistoryReadIndex]);
        m_historyDepthReadViewHandle = createReadView(
            m_historyDepthReadHandle, "RayTracedReflectionPreviousDepthSRV");

        m_historyNormalReadHandle = builder.ImportTexture(
            m_historyNormalTextures[m_currentHistoryReadIndex],
            m_historyNormalTextureStates[m_currentHistoryReadIndex]);
        m_historyNormalReadViewHandle = createReadView(
            m_historyNormalReadHandle, "RayTracedReflectionPreviousNormalSRV");

        m_reflectionTexture = m_historyTextures[m_currentHistoryWriteIndex];
        m_reflectionHandle = builder.ImportTexture(
            m_historyTextures[m_currentHistoryWriteIndex],
            m_historyTextureStates[m_currentHistoryWriteIndex]);
        m_reflectionViewHandle = createWriteView(
            m_reflectionHandle, "RayTracedReflectionOutputUAV");
        builder.SetExportState(
            m_reflectionHandle, RHIResourceState::ShaderResource);

        m_historyDepthWriteHandle = builder.ImportTexture(
            m_historyDepthTextures[m_currentHistoryWriteIndex],
            m_historyDepthTextureStates[m_currentHistoryWriteIndex]);
        m_historyDepthWriteViewHandle = createWriteView(
            m_historyDepthWriteHandle, "RayTracedReflectionOutputDepthUAV");
        builder.SetExportState(
            m_historyDepthWriteHandle, RHIResourceState::ShaderResource);

        m_historyNormalWriteHandle = builder.ImportTexture(
            m_historyNormalTextures[m_currentHistoryWriteIndex],
            m_historyNormalTextureStates[m_currentHistoryWriteIndex]);
        m_historyNormalWriteViewHandle = createWriteView(
            m_historyNormalWriteHandle, "RayTracedReflectionOutputNormalUAV");
        builder.SetExportState(
            m_historyNormalWriteHandle, RHIResourceState::ShaderResource);

        m_stats.outputDeclared = m_sceneColorViewHandle.IsValid() &&
            m_depthViewHandle.IsValid() && m_velocityViewHandle.IsValid() &&
            m_historyViewHandle.IsValid() &&
            m_historyDepthReadViewHandle.IsValid() &&
            m_historyDepthWriteViewHandle.IsValid() &&
            m_historyNormalReadViewHandle.IsValid() &&
            m_historyNormalWriteViewHandle.IsValid() &&
            m_reflectionViewHandle.IsValid();
    }

    void RayTracedReflectionPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns graph resource realization and execution.
    }

    void RayTracedReflectionPass::Execute(
        RenderGraphPassContext& context,
        const ViewData& view)
    {
        RHICommandContext& ctx = context.Commands();
        if (!m_stats.outputDeclared ||
            !m_pipelineCache ||
            !m_sceneManager ||
            !m_sceneColorReadHandle.IsValid() ||
            !m_depthReadHandle.IsValid() ||
            !m_historyReadHandle.IsValid() ||
            !m_historyDepthReadHandle.IsValid() ||
            !m_historyDepthWriteHandle.IsValid() ||
            !m_historyNormalReadHandle.IsValid() ||
            !m_historyNormalWriteHandle.IsValid() ||
            !m_reflectionHandle.IsValid())
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHIPipeline* pipeline = m_pipelineCache->GetRayTracedReflectionPipeline();
        RHIShaderTable* shaderTable = m_pipelineCache->GetRayTracedReflectionShaderTable();
        RHIDescriptorSetLayout* setLayout = m_pipelineCache->GetRayTracedReflectionSetLayout();
        IRHIDevice* device = m_pipelineCache->GetDevice();
        if (!pipeline || !shaderTable || !setLayout || !device)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHIAccelerationStructure* tlas = m_sceneManager->GetTopLevelAS();
        RHIBuffer* materialMetadataBuffer = m_sceneManager->GetInstanceMaterialMetadataBuffer();
        RHIBuffer* geometryMetadataBuffer = m_sceneManager->GetInstanceAlphaMetadataBuffer();
        RHITexture* sceneColor = context.GetTexture(m_sceneColorReadHandle);
        RHITexture* sceneDepth = context.GetTexture(m_depthReadHandle);
        RHITexture* sceneVelocity = context.GetTexture(m_velocityReadHandle);
        RHITexture* previousReflection = context.GetTexture(m_historyReadHandle);
        RHITexture* previousDepthHistory =
            context.GetTexture(m_historyDepthReadHandle);
        RHITexture* currentDepthHistory =
            context.GetTexture(m_historyDepthWriteHandle);
        RHITexture* previousNormalHistory =
            context.GetTexture(m_historyNormalReadHandle);
        RHITexture* currentNormalHistory =
            context.GetTexture(m_historyNormalWriteHandle);
        RHITexture* reflectionOutput = context.GetTexture(m_reflectionHandle);
        if (!tlas || !materialMetadataBuffer || !geometryMetadataBuffer || !sceneColor || !sceneDepth ||
            !previousReflection || !previousDepthHistory || !currentDepthHistory || !previousNormalHistory ||
            !currentNormalHistory || !reflectionOutput)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        RHITextureView* reflectionUAV =
            context.GetTextureView(m_reflectionViewHandle);
        RHITextureView* sceneColorSRV =
            context.GetTextureView(m_sceneColorViewHandle);
        RHITextureView* sceneDepthSRV =
            context.GetTextureView(m_depthViewHandle);
        RHITextureView* sceneVelocitySRV =
            context.GetTextureView(m_velocityViewHandle);
        RHITextureView* previousReflectionSRV =
            context.GetTextureView(m_historyViewHandle);
        RHITextureView* previousDepthHistorySRV =
            context.GetTextureView(m_historyDepthReadViewHandle);
        RHITextureView* currentDepthHistoryUAV =
            context.GetTextureView(m_historyDepthWriteViewHandle);
        RHITextureView* previousNormalHistorySRV =
            context.GetTextureView(m_historyNormalReadViewHandle);
        RHITextureView* currentNormalHistoryUAV =
            context.GetTextureView(m_historyNormalWriteViewHandle);
        if (!reflectionUAV || !sceneColorSRV || !sceneDepthSRV || !sceneVelocitySRV || !previousReflectionSRV ||
            !previousDepthHistorySRV || !currentDepthHistoryUAV || !previousNormalHistorySRV ||
            !currentNormalHistoryUAV)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        std::vector<RHITextureView*> materialTextureViews;
        if (!ResolveMaterialTextureViews(materialTextureViews))
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        std::vector<RHIBuffer*> geometryIndexBuffers;
        std::vector<RHIBuffer*> geometryUVBuffers;
        std::vector<RHIBuffer*> geometryNormalBuffers;
        std::vector<RHIBuffer*> geometryTangentBuffers;
        if (!ResolveGeometryBuffers(
                geometryIndexBuffers,
                geometryUVBuffers,
                geometryNormalBuffers,
                geometryTangentBuffers))
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
        descriptorDesc.debugName = "RayTracedReflectionDescriptorSet";
        descriptorDesc.BindAccelerationStructure(RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING, tlas);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING, reflectionUAV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_COLOR_BINDING, sceneColorSRV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_DEPTH_BINDING, sceneDepthSRV);
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_CONSTANTS_BINDING,
                                  m_constantBuffer.Get(),
                                  0,
                                  AlignRayTracedReflectionConstantBufferSize(sizeof(RayTracedReflectionGPUConstants)));
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING, materialMetadataBuffer);
        RHITextureView* materialTextureFallback = materialTextureViews.empty()
            ? sceneColorSRV
            : materialTextureViews.front();
        for (uint32 textureIndex = 0;
             textureIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES;
             ++textureIndex)
        {
            RHITextureView* textureView = textureIndex < materialTextureViews.size()
                ? materialTextureViews[textureIndex]
                : materialTextureFallback;
            descriptorDesc.BindTexture(
                RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING,
                textureView,
                textureIndex);
        }
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING, previousReflectionSRV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING, previousDepthHistorySRV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING, currentDepthHistoryUAV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING, previousNormalHistorySRV);
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING, currentNormalHistoryUAV);
        descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING, geometryMetadataBuffer);
        for (uint32 bufferIndex = 0;
             bufferIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;
             ++bufferIndex)
        {
            RHIBuffer* buffer = bufferIndex < geometryIndexBuffers.size()
                ? geometryIndexBuffers[bufferIndex]
                : geometryMetadataBuffer;
            descriptorDesc.BindBuffer(
                RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING,
                buffer,
                0,
                RVX_WHOLE_SIZE,
                bufferIndex);
        }
        for (uint32 bufferIndex = 0;
             bufferIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;
             ++bufferIndex)
        {
            RHIBuffer* buffer = bufferIndex < geometryUVBuffers.size()
                ? geometryUVBuffers[bufferIndex]
                : geometryMetadataBuffer;
            descriptorDesc.BindBuffer(
                RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING,
                buffer,
                0,
                RVX_WHOLE_SIZE,
                bufferIndex);
        }
        for (uint32 bufferIndex = 0;
             bufferIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;
             ++bufferIndex)
        {
            RHIBuffer* buffer = bufferIndex < geometryNormalBuffers.size()
                ? geometryNormalBuffers[bufferIndex]
                : geometryMetadataBuffer;
            descriptorDesc.BindBuffer(
                RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING,
                buffer,
                0,
                RVX_WHOLE_SIZE,
                bufferIndex);
        }
        for (uint32 bufferIndex = 0;
             bufferIndex < RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS;
             ++bufferIndex)
        {
            RHIBuffer* buffer = bufferIndex < geometryTangentBuffers.size()
                ? geometryTangentBuffers[bufferIndex]
                : geometryMetadataBuffer;
            descriptorDesc.BindBuffer(
                RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING,
                buffer,
                0,
                RVX_WHOLE_SIZE,
                bufferIndex);
        }
        descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING, sceneVelocitySRV);

        RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet)
        {
            m_stats.dispatchRecorded = false;
            return;
        }

        m_stats.descriptorSetAvailable = true;

        if (!context.RetainSubmissionResource(descriptorSet) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(m_constantBuffer)))
        {
            RVX_CORE_WARN("RayTracedReflectionPass: submission ownership rejected descriptor set");
            m_stats.dispatchRecorded = false;
            return;
        }

        ctx.SetPipeline(pipeline);
        ctx.SetDescriptorSet(0, descriptorSet.Get());

        RHIDispatchRaysDesc dispatchDesc;
        dispatchDesc.shaderTable = shaderTable;
        dispatchDesc.width = reflectionOutput->GetWidth();
        dispatchDesc.height = reflectionOutput->GetHeight();
        dispatchDesc.depth = 1;
        const uint32 timingFrameIndex = GetTimingFrameIndex();
        const uint32 timingStartQuery = GetRayTracedReflectionTimingStartQuery(timingFrameIndex);
        const uint32 timingEndQuery = GetRayTracedReflectionTimingEndQuery(timingFrameIndex);
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
                                   RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME,
                                   timingReadbackBuffer,
                                   0);
                m_timingReadbackValid[timingFrameIndex] = true;
                m_stats.gpuTimingResolveRecorded = true;
            }
        }

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
        }
        m_historyWriteIndex = 1u - m_currentHistoryWriteIndex;
        m_stats.materialTexturesBound = static_cast<uint32>(materialTextureViews.size());
        m_stats.geometryIndexBufferCount = static_cast<uint32>(geometryIndexBuffers.size());
        m_stats.geometryUVBufferCount = static_cast<uint32>(geometryUVBuffers.size());
        m_stats.geometryNormalBufferCount = static_cast<uint32>(geometryNormalBuffers.size());
        m_stats.geometryTangentBufferCount = static_cast<uint32>(geometryTangentBuffers.size());
        m_stats.historyAvailable = true;
        m_stats.depthHistoryAvailable = true;
        m_stats.normalHistoryAvailable = true;
        m_stats.temporalAccumulated = m_temporalAccumulatedThisFrame;
        m_stats.dispatchRecorded = true;
    }

    uint32 RayTracedReflectionPass::GetTimingFrameIndex() const
    {
        return m_device ? (m_device->GetCurrentFrameIndex() % RVX_MAX_FRAME_COUNT) : 0;
    }

    RHIBuffer* RayTracedReflectionPass::GetTimingReadbackBuffer(uint32 frameIndex) const
    {
        if (frameIndex >= RVX_MAX_FRAME_COUNT)
            return nullptr;

        return m_timingReadbackBuffers[frameIndex].Get();
    }

    void RayTracedReflectionPass::TryReadbackTimingResult(uint32 frameIndex)
    {
        if (frameIndex >= RVX_MAX_FRAME_COUNT || !m_timingReadbackValid[frameIndex])
            return;

        RHIBuffer* readbackBuffer = GetTimingReadbackBuffer(frameIndex);
        if (!readbackBuffer || readbackBuffer->GetSize() < RVX_RAY_TRACED_REFLECTION_TIMING_READBACK_BYTES ||
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

    bool RayTracedReflectionPass::EnsureHistoryTextures(uint32 width, uint32 height)
    {
        if (!m_device || width == 0 || height == 0)
            return false;

        const bool hasCompleteHistory =
            m_historyTextures[0] && m_historyTextures[1] &&
            m_historyDepthTextures[0] && m_historyDepthTextures[1] &&
            m_historyNormalTextures[0] && m_historyNormalTextures[1];
        const bool historyResolutionChanged =
            hasCompleteHistory && (m_outputWidth != width || m_outputHeight != height);
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
        desc.format = RHIFormat::RGBA16_FLOAT;
        desc.dimension = RHITextureDimension::Texture2D;
        desc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
        desc.debugName = "RayTracedReflectionHistory0";
        m_historyTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedReflectionHistory1";
        m_historyTextures[1] = m_device->CreateTexture(desc);

        desc.format = RHIFormat::R32_FLOAT;
        desc.debugName = "RayTracedReflectionDepthHistory0";
        m_historyDepthTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedReflectionDepthHistory1";
        m_historyDepthTextures[1] = m_device->CreateTexture(desc);

        desc.format = RHIFormat::RGBA16_FLOAT;
        desc.debugName = "RayTracedReflectionNormalHistory0";
        m_historyNormalTextures[0] = m_device->CreateTexture(desc);
        desc.debugName = "RayTracedReflectionNormalHistory1";
        m_historyNormalTextures[1] = m_device->CreateTexture(desc);

        if (!m_historyTextures[0] || !m_historyTextures[1] ||
            !m_historyDepthTextures[0] || !m_historyDepthTextures[1] ||
            !m_historyNormalTextures[0] || !m_historyNormalTextures[1])
        {
            ResetHistoryTextures();
            m_outputWidth = 0;
            m_outputHeight = 0;
            return false;
        }

        m_outputWidth = width;
        m_outputHeight = height;
        m_historyTextureStates[0] = RHIResourceState::Common;
        m_historyTextureStates[1] = RHIResourceState::Common;
        m_historyDepthTextureStates[0] = RHIResourceState::Common;
        m_historyDepthTextureStates[1] = RHIResourceState::Common;
        m_historyNormalTextureStates[0] = RHIResourceState::Common;
        m_historyNormalTextureStates[1] = RHIResourceState::Common;
        return true;
    }

    bool RayTracedReflectionPass::EnsureFallbackVelocityTexture()
    {
        if (m_fallbackVelocityTexture)
            return true;

        IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : m_device;
        if (!device)
            return false;

        RHITextureDesc desc = RHITextureDesc::Texture2D(1, 1, RHIFormat::RG16_FLOAT);
        desc.debugName = "RayTracedReflectionFallbackVelocity";
        m_fallbackVelocityTexture = device->CreateTexture(desc);
        return m_fallbackVelocityTexture != nullptr;
    }

    void RayTracedReflectionPass::ResetHistoryTextures()
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

        m_reflectionTexture.Reset();
        QueueRenderOwnerRetirement(
            m_historyTextures[0], m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            m_historyTextures[1], m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            m_historyDepthTextures[0], m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            m_historyDepthTextures[1], m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            m_historyNormalTextures[0], m_pendingOwnerRetirements);
        QueueRenderOwnerRetirement(
            m_historyNormalTextures[1], m_pendingOwnerRetirements);
        m_historyTextureStates[0] = RHIResourceState::Common;
        m_historyTextureStates[1] = RHIResourceState::Common;
        m_historyDepthTextureStates[0] = RHIResourceState::Common;
        m_historyDepthTextureStates[1] = RHIResourceState::Common;
        m_historyNormalTextureStates[0] = RHIResourceState::Common;
        m_historyNormalTextureStates[1] = RHIResourceState::Common;
        m_outputWidth = 0;
        m_outputHeight = 0;
        m_historyWriteIndex = 0;
        m_currentHistoryReadIndex = 1;
        m_currentHistoryWriteIndex = 0;
        m_historyReadHandle = {};
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

    bool RayTracedReflectionPass::EnsureConstantBuffer()
    {
        if (m_constantBuffer)
            return true;

        if (!m_device)
            return false;

        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignRayTracedReflectionConstantBufferSize(sizeof(RayTracedReflectionGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "RayTracedReflectionConstants";

        m_constantBuffer = m_device->CreateBuffer(bufferDesc);
        return m_constantBuffer != nullptr;
    }

    bool RayTracedReflectionPass::UpdateConstants(const ViewData& view)
    {
        if (!m_constantBuffer)
            return false;

        const float outputWidth = static_cast<float>(std::max(1u, m_outputWidth));
        const float outputHeight = static_cast<float>(std::max(1u, m_outputHeight));
        const float sceneWidth = static_cast<float>(std::max(1u, view.viewportWidth));
        const float sceneHeight = static_cast<float>(std::max(1u, view.viewportHeight));
        const float maxTraceDistance = ResolveReflectionMaxTraceDistance(m_config.maxTraceDistance, view.farPlane);
        const float intensity = ClampFiniteRange(m_config.intensity, 1.0f, 0.0f, 1.0f);
        const float maxRoughness = ClampFiniteRange(m_config.maxRoughness, 1.0f, 0.01f, 1.0f);
        const float distanceFadeStart = ClampFiniteRange(m_config.distanceFadeStart, 0.8f, 0.0f, 1.0f);
        const uint32 rayMask = std::min<uint32>(m_config.instanceMask, 0xFFu);
        const uint32 samplesPerPixel = ResolveReflectionSamplesPerPixel(m_config.samplesPerPixel);
        const float roughnessConeSpread =
            ClampFiniteRange(m_config.roughnessConeSpread, 1.0f, 0.0f, 1.0f);
        const float normalBias = std::min(ClampFiniteNonNegative(m_config.normalBias, 0.02f), 1.0f);
        const float rayMinT = std::min(ClampFiniteNonNegative(m_config.rayMinT, 0.001f), 1.0f);
        const float fireflyClamp = ClampFiniteNonNegative(m_config.fireflyClamp, 64.0f);
        const float frameSeed = static_cast<float>(view.frameNumber & 0x00FFFFFFull);
        const bool useHistory = m_historyValid &&
                                m_historyViewValid &&
                                m_config.temporalAccumulation;
        const float temporalBlend = ClampFiniteRange(m_config.temporalBlendFactor, 0.85f, 0.0f, 0.95f);
        const float depthRejectionThreshold = ClampFiniteRange(m_config.historyDepthThreshold, 0.01f, 0.0f, 0.1f);
        const float normalRejectionThreshold =
            ClampFiniteRange(m_config.historyNormalThreshold, 0.85f, 0.0f, 1.0f);
        const float historyLuminanceTolerance =
            ClampFiniteNonNegative(m_config.historyLuminanceTolerance, 4.0f);
        const float historyConfidenceThreshold =
            ClampFiniteRange(m_config.historyConfidenceThreshold, 0.05f, 0.0f, 1.0f);
        const float historyVelocityRejectionScale =
            ClampFiniteRange(m_config.historyVelocityRejectionScale, 8.0f, 0.0f, 64.0f);

        RayTracedReflectionGPUConstants constants;
        constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;
        constants.previousViewProjection = m_historyViewValid ? m_lastHistoryViewProjection : view.viewProjectionMatrix;
        constants.cameraPositionAndTMax = Vec4(view.cameraPosition, maxTraceDistance);
        constants.outputSizeAndInvSize = Vec4(outputWidth, outputHeight, 1.0f / outputWidth, 1.0f / outputHeight);
        constants.sceneSizeAndInvSize = Vec4(sceneWidth, sceneHeight, 1.0f / sceneWidth, 1.0f / sceneHeight);
        constants.reflectionOptions = Vec4(intensity, maxRoughness, static_cast<float>(rayMask), distanceFadeStart);
        constants.historyParams =
            Vec4(useHistory ? 1.0f : 0.0f, temporalBlend, depthRejectionThreshold, normalRejectionThreshold);
        constants.stochasticParams =
            Vec4(static_cast<float>(samplesPerPixel), frameSeed, roughnessConeSpread, 0.0f);
        constants.rayBiasParams = Vec4(normalBias, rayMinT, fireflyClamp, 0.0f);
        constants.historyClampParams = Vec4(
            historyLuminanceTolerance,
            historyConfidenceThreshold,
            view.velocityTarget.IsValid() ? 1.0f : 0.0f,
            historyVelocityRejectionScale);

        void* mapped = m_constantBuffer->Map();
        if (!mapped)
            return false;

        std::memcpy(mapped, &constants, sizeof(constants));
        if (!m_constantBuffer->CommitMappedWrite())
            return false;
        m_pendingHistoryViewProjection = view.viewProjectionMatrix;
        m_pendingHistoryViewValid = true;
        m_temporalAccumulatedThisFrame = useHistory;
        m_stats.constantsUploaded = true;
        return true;
    }

    bool RayTracedReflectionPass::ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const
    {
        outViews.clear();
        if (!m_sceneManager)
            return false;

        const std::vector<uint64>& textureIds =
            m_sceneManager->GetInstanceMaterialTextureTable();
        if (textureIds.empty())
            return true;

        if (!m_resourceRegistry || !m_viewCache)
            return false;

        if (textureIds.size() > RTReflectionBindings::RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES)
            return false;

        outViews.reserve(textureIds.size());
        for (uint64 textureId : textureIds)
        {
            RHITexture* texture = m_resourceRegistry->ResolveTextureObject(
                UnpackRenderResourceHandle(textureId));
            if (!texture)
                return false;

            RHITextureView* view = m_viewCache->GetDefaultSRV(texture);
            if (!view)
                return false;

            outViews.push_back(view);
        }

        return true;
    }

    bool RayTracedReflectionPass::ResolveGeometryBuffers(
        std::vector<RHIBuffer*>& outIndexBuffers,
        std::vector<RHIBuffer*>& outUVBuffers,
        std::vector<RHIBuffer*>& outNormalBuffers,
        std::vector<RHIBuffer*>& outTangentBuffers) const
    {
        outIndexBuffers.clear();
        outUVBuffers.clear();
        outNormalBuffers.clear();
        outTangentBuffers.clear();
        if (!m_sceneManager)
            return false;

        const std::vector<RHIBuffer*>& indexBuffers = m_sceneManager->GetInstanceAlphaIndexBufferTable();
        const std::vector<RHIBuffer*>& uvBuffers = m_sceneManager->GetInstanceAlphaUVBufferTable();
        const std::vector<RHIBuffer*>& normalBuffers = m_sceneManager->GetInstanceAlphaNormalBufferTable();
        const std::vector<RHIBuffer*>& tangentBuffers = m_sceneManager->GetInstanceAlphaTangentBufferTable();
        if (indexBuffers.size() > RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            uvBuffers.size() > RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            normalBuffers.size() > RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS ||
            tangentBuffers.size() > RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)
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

        outNormalBuffers.reserve(normalBuffers.size());
        for (RHIBuffer* buffer : normalBuffers)
        {
            if (!buffer)
                return false;

            outNormalBuffers.push_back(buffer);
        }

        outTangentBuffers.reserve(tangentBuffers.size());
        for (RHIBuffer* buffer : tangentBuffers)
        {
            if (!buffer)
                return false;

            outTangentBuffers.push_back(buffer);
        }

        return true;
    }

} // namespace RVX
