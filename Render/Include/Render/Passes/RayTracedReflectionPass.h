#pragma once

/**
 * @file RayTracedReflectionPass.h
 * @brief Ray-traced reflection texture pass
 */

#include "Core/MathTypes.h"
#include "Render/Passes/IRenderPass.h"

#include <array>
#include <vector>

namespace RVX
{
    class PipelineCache;
    class RayTracingSceneManager;
    class RenderRetirementQueue;
    class ResourceViewCache;
    class RenderResourceRegistry;
    struct GPUCompletionToken;

    struct RayTracedReflectionPassStats
    {
        bool requested = false;
        bool supported = false;
        bool tlasAvailable = false;
        bool materialMetadataAvailable = false;
        bool materialTextureTableAvailable = false;
        bool geometryMetadataAvailable = false;
        bool geometryTableAvailable = false;
        bool sceneColorAvailable = false;
        bool depthAvailable = false;
        bool velocityAvailable = false;
        bool historyAvailable = false;
        bool depthHistoryAvailable = false;
        bool normalHistoryAvailable = false;
        bool historyReset = false;
        bool historyRecreated = false;
        bool historyResolutionChanged = false;
        bool historyConfigChanged = false;
        bool temporalAccumulated = false;
        bool outputDeclared = false;
        bool resourceViewsAvailable = false;
        bool descriptorSetAvailable = false;
        bool constantsUploaded = false;
        bool dispatchRecorded = false;
        bool gpuTimingSupported = false;
        bool gpuTimingQueriesRecorded = false;
        bool gpuTimingResolveRecorded = false;
        bool gpuTimingReadbackBufferAvailable = false;
        bool gpuTimingResultAvailable = false;
        uint32 gpuTimingStartQueryIndex = 0;
        uint32 gpuTimingEndQueryIndex = 1;
        uint64 gpuTimestampFrequency = 0;
        uint64 gpuTimingReadbackBytes = 0;
        uint64 gpuTimingStartTimestamp = 0;
        uint64 gpuTimingEndTimestamp = 0;
        uint64 gpuTimingElapsedTicks = 0;
        float gpuTimingElapsedMs = 0.0f;
        uint32 gpuTimingReadbackBufferCount = 0;
        uint32 gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
        float resolutionScale = 1.0f;
        uint32 samplesPerPixel = 1;
        float intensity = 1.0f;
        float maxTraceDistance = 0.0f;
        float maxRoughness = 1.0f;
        float distanceFadeStart = 0.8f;
        float roughnessConeSpread = 1.0f;
        float normalBias = 0.02f;
        float rayMinT = 0.001f;
        float fireflyClamp = 64.0f;
        float temporalBlendFactor = 0.0f;
        float historyDepthThreshold = 0.01f;
        float historyNormalThreshold = 0.85f;
        float historyLuminanceTolerance = 4.0f;
        float historyConfidenceThreshold = 0.05f;
        float historyVelocityRejectionScale = 0.0f;
        uint32 materialTextureCount = 0;
        uint32 materialTexturesBound = 0;
        uint32 geometryIndexBufferCount = 0;
        uint32 geometryUVBufferCount = 0;
        uint32 geometryNormalBufferCount = 0;
        uint32 geometryTangentBufferCount = 0;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedRayCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    struct RayTracedReflectionPassConfig
    {
        float intensity = 1.0f;
        float resolutionScale = 1.0f;
        float maxRoughness = 1.0f;
        float maxTraceDistance = 50.0f;
        float distanceFadeStart = 0.8f;
        uint32 instanceMask = 0xFF;
        uint32 samplesPerPixel = 1;
        float roughnessConeSpread = 1.0f;
        float normalBias = 0.02f;
        float rayMinT = 0.001f;
        float fireflyClamp = 64.0f;
        bool temporalAccumulation = true;
        float temporalBlendFactor = 0.85f;
        float historyDepthThreshold = 0.01f;
        float historyNormalThreshold = 0.85f;
        float historyLuminanceTolerance = 4.0f;
        float historyConfidenceThreshold = 0.05f;
        float historyVelocityRejectionScale = 8.0f;
    };

    class RayTracedReflectionPass : public IRenderPass
    {
    public:
        RayTracedReflectionPass() = default;
        ~RayTracedReflectionPass() override = default;

        const char* GetName() const override { return "RayTracedReflectionPass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 150; }
        RenderGraphPassType GetPassType() const override { return RenderGraphPassType::RayTracing; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

        void SetResources(PipelineCache* pipelineCache,
                          ResourceViewCache* viewCache);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }
        void SetRayTracingScene(RayTracingSceneManager* sceneManager) { m_sceneManager = sceneManager; }
        void SetConfig(const RayTracedReflectionPassConfig& config) { m_config = config; }
        const RayTracedReflectionPassConfig& GetConfig() const { return m_config; }

        RHITexture* GetReflectionTexture() const { return m_reflectionTexture.Get(); }
        RGTextureHandle GetReflectionHandle() const { return m_reflectionHandle; }
        RGTextureHandle GetCurrentNormalHistoryHandle() const { return m_historyNormalWriteHandle; }
        const RayTracedReflectionPassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        RayTracingSceneManager* m_sceneManager = nullptr;
        RayTracedReflectionPassConfig m_config;
        RayTracedReflectionPassConfig m_lastHistoryConfig;
        bool m_lastHistoryConfigValid = false;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Ray traced reflections have not been requested";

        RGTextureHandle m_reflectionHandle;
        RGTextureHandle m_sceneColorReadHandle;
        RGTextureHandle m_depthReadHandle;
        RGTextureHandle m_velocityReadHandle;
        RGTextureHandle m_historyReadHandle;
        RGTextureHandle m_historyDepthReadHandle;
        RGTextureHandle m_historyDepthWriteHandle;
        RGTextureHandle m_historyNormalReadHandle;
        RGTextureHandle m_historyNormalWriteHandle;
        RHITextureRef m_reflectionTexture;
        RHITextureRef m_fallbackVelocityTexture;
        RHIQueryPoolRef m_timingQueryPool;
        std::array<RHIBufferRef, RVX_MAX_FRAME_COUNT> m_timingReadbackBuffers;
        std::array<bool, RVX_MAX_FRAME_COUNT> m_timingReadbackValid{};
        std::array<RHITextureRef, 2> m_historyTextures;
        std::array<RHITextureRef, 2> m_historyDepthTextures;
        std::array<RHITextureRef, 2> m_historyNormalTextures;
        std::array<RHIResourceState, 2> m_historyTextureStates{
            RHIResourceState::Common,
            RHIResourceState::Common
        };
        std::array<RHIResourceState, 2> m_historyDepthTextureStates{
            RHIResourceState::Common,
            RHIResourceState::Common
        };
        std::array<RHIResourceState, 2> m_historyNormalTextureStates{
            RHIResourceState::Common,
            RHIResourceState::Common
        };
        RHIBufferRef m_constantBuffer;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        RayTracedReflectionPassStats m_stats;
        uint32 m_outputWidth = 0;
        uint32 m_outputHeight = 0;
        uint32 m_historyWriteIndex = 0;
        uint32 m_currentHistoryReadIndex = 1;
        uint32 m_currentHistoryWriteIndex = 0;
        bool m_historyValid = false;
        bool m_historyViewValid = false;
        bool m_pendingHistoryViewValid = false;
        bool m_temporalAccumulatedThisFrame = false;
        Mat4 m_lastHistoryViewProjection = Mat4Identity();
        Mat4 m_pendingHistoryViewProjection = Mat4Identity();

        uint32 GetTimingFrameIndex() const;
        RHIBuffer* GetTimingReadbackBuffer(uint32 frameIndex) const;
        void TryReadbackTimingResult(uint32 frameIndex);
        bool EnsureHistoryTextures(uint32 width, uint32 height);
        bool EnsureFallbackVelocityTexture();
        void ResetHistoryTextures();
        bool EnsureConstantBuffer();
        bool UpdateConstants(const ViewData& view);
        bool ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const;
        bool ResolveGeometryBuffers(std::vector<RHIBuffer*>& outIndexBuffers,
                                    std::vector<RHIBuffer*>& outUVBuffers,
                                    std::vector<RHIBuffer*>& outNormalBuffers,
                                    std::vector<RHIBuffer*>& outTangentBuffers) const;
    };

} // namespace RVX
