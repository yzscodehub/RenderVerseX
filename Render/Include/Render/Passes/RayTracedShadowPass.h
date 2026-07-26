#pragma once

/**
 * @file RayTracedShadowPass.h
 * @brief Ray-traced directional shadow mask pass
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/ShadowPass.h"

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

    struct RayTracedShadowPassStats
    {
        bool requested = false;
        bool supported = false;
        bool tlasAvailable = false;
        bool materialMetadataAvailable = false;
        bool materialTextureTableAvailable = false;
        bool alphaMetadataAvailable = false;
        bool alphaTextureTableAvailable = false;
        bool alphaGeometryTableAvailable = false;
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
        uint32 samplesPerPixel = 1;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedRayCount = 0;
        uint32 materialTextureCount = 0;
        uint32 materialTexturesBound = 0;
        uint32 alphaTextureCount = 0;
        uint32 alphaTexturesBound = 0;
        uint32 alphaIndexBufferCount = 0;
        uint32 alphaUVBufferCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    /**
     * @brief Feature-gated ray traced directional shadow pass.
     *
     * The pass is wired into the main renderer once a TLAS is available, but
     * remains unsupported until the backend ray tracing pipeline and shader
     * table creation path is implemented. This keeps hybrid RT shadow support
     * observable without claiming a fake DispatchRays implementation.
     */
    class RayTracedShadowPass : public IRenderPass
    {
    public:
        RayTracedShadowPass() = default;
        ~RayTracedShadowPass() override = default;

        const char* GetName() const override { return "RayTracedShadowPass"; }
        int32_t GetPriority() const override { return PassPriority::Shadow + 50; }
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

        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }
        void SetRayTracingScene(RayTracingSceneManager* sceneManager) { m_sceneManager = sceneManager; }
        void SetConfig(const ShadowPassConfig& config) { m_config = config; }
        const ShadowPassConfig& GetConfig() const { return m_config; }
        void SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity);

        RHITexture* GetShadowMask() const { return m_shadowMaskTexture; }
        RGTextureHandle GetShadowMaskHandle() const { return m_shadowMaskHandle; }
        const RayTracedShadowPassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        RayTracingSceneManager* m_sceneManager = nullptr;
        ShadowPassConfig m_config;
        ShadowPassConfig m_lastHistoryConfig;
        bool m_lastHistoryConfigValid = false;
        Vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
        Vec3 m_lightColor{1.0f, 1.0f, 1.0f};
        float m_lightIntensity = 0.0f;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Ray traced shadows have not been requested";

        RGTextureHandle m_shadowMaskHandle;
        RGTextureHandle m_depthReadHandle;
        RGTextureHandle m_velocityReadHandle;
        RGTextureHandle m_historyReadHandle;
        RGTextureHandle m_historyDepthReadHandle;
        RGTextureHandle m_historyDepthWriteHandle;
        RGTextureHandle m_historyNormalReadHandle;
        RGTextureHandle m_historyNormalWriteHandle;
        RHITexture* m_shadowMaskTexture = nullptr;
        RHIBufferRef m_constantBuffer;
        std::array<RHITextureRef, 2> m_historyTextures;
        std::array<RHITextureRef, 2> m_historyDepthTextures;
        std::array<RHITextureRef, 2> m_historyNormalTextures;
        RHITextureRef m_fallbackVelocityTexture;
        RHIQueryPoolRef m_timingQueryPool;
        std::array<RHIBufferRef, RVX_MAX_FRAME_COUNT> m_timingReadbackBuffers;
        std::array<bool, RVX_MAX_FRAME_COUNT> m_timingReadbackValid{};
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
        uint32 m_historyWidth = 0;
        uint32 m_historyHeight = 0;
        uint32 m_historyWriteIndex = 0;
        uint32 m_currentHistoryReadIndex = 1;
        uint32 m_currentHistoryWriteIndex = 0;
        bool m_historyValid = false;
        bool m_historyViewValid = false;
        bool m_pendingHistoryViewValid = false;
        bool m_temporalAccumulatedThisFrame = false;
        Mat4 m_lastHistoryViewProjection = Mat4Identity();
        Mat4 m_pendingHistoryViewProjection = Mat4Identity();
        Vec3 m_lastHistoryRayDirection{0.0f, 1.0f, 0.0f};
        Vec3 m_pendingHistoryRayDirection{0.0f, 1.0f, 0.0f};
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        RayTracedShadowPassStats m_stats;

        uint32 GetTimingFrameIndex() const;
        RHIBuffer* GetTimingReadbackBuffer(uint32 frameIndex) const;
        void TryReadbackTimingResult(uint32 frameIndex);
        bool EnsureConstantBuffer();
        bool EnsureHistoryTextures(uint32 width, uint32 height);
        bool EnsureFallbackVelocityTexture();
        void ResetHistoryTextures();
        bool UpdateConstants(const ViewData& view);
        bool ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const;
        bool ResolveAlphaTextureViews(std::vector<RHITextureView*>& outViews) const;
        bool ResolveAlphaGeometryBuffers(std::vector<RHIBuffer*>& outIndexBuffers,
                                         std::vector<RHIBuffer*>& outUVBuffers) const;
    };

} // namespace RVX
