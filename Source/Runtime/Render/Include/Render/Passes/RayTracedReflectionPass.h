#pragma once

/**
 * @file RayTracedReflectionPass.h
 * @brief Ray-traced reflection texture pass
 */

#include "Core/MathTypes.h"
#include "Render/Passes/IRenderPass.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <array>
#include <memory>
#include <vector>

namespace RVX
{
    class PipelineCache;
    class RayTracingSceneManager;
    class RenderRetirementQueue;
    class ResourceViewCache;
    class RenderResourceRegistry;
    struct GPUCompletionToken;
    struct RayTracedReflectionHistoryOwner;
    struct RayTracedReflectionHistoryReservation;

    struct RayTracedReflectionPassConfig
    {
        float intensity = 1.0f;
        float resolutionScale = 1.0f;
        /**
         * Hybrid RT uses a fixed ceil(full / 2) reflection extent.  Legacy
         * recipes retain the configurable scale until their composition opts
         * into the HybridRayTracingFramePlan contract.
         */
        bool fixedHalfResolution = false;
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
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view) override;
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);
        /**
         * @brief Bind one executed RT history candidate to exact submission.
         *
         * This advances only physical resource-access provenance. Logical
         * history remains unavailable until CommitHistoryAfterPresent() is
         * called with this same view identity after Present succeeds.
         */
        [[nodiscard]] bool NotifySubmission(
            const RenderPassRecordIdentity& identity,
            const RenderViewHistoryIdentity& historyIdentity,
            const GPUCompletionToken& completion);
        /**
         * @brief Compatibility submission path that never publishes history.
         *
         * A legacy ViewData recording does not carry a canonical
         * RenderViewHistoryIdentity, so it cannot become temporal input for a
         * later frame.
         */
        void NotifySubmission(const RenderPassRecordIdentity& identity,
                              const GPUCompletionToken& completion);
        /** @brief Discard an identity-addressable recording that was not submitted. */
        void ReleaseUnsubmittedFrame(const RenderPassRecordIdentity& identity);
        /** Bounded compatibility adapters for legacy ViewData recordings. */
        void NotifySubmission(const GPUCompletionToken& completion);
        void ReleaseUnsubmittedFrame();

        /** @brief True when this pass owns an executed candidate awaiting Present. */
        [[nodiscard]] bool RequiresHistoryCommit() const noexcept;
        /** @brief Preflight an exact submitted candidate without mutating it. */
        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        /** @brief Promote only an exact submitted and successfully presented candidate. */
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        /** @brief Discard only an exact submitted candidate that did not present. */
        void AbandonSubmittedHistory(
            const RenderViewHistoryIdentity& identity) noexcept;

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
        RGBufferHandle m_exposureStateReadHandle;
        RGTextureHandle m_historyReadHandle;
        RGTextureHandle m_historyDepthReadHandle;
        RGTextureHandle m_historyDepthWriteHandle;
        RGTextureHandle m_historyNormalReadHandle;
        RGTextureHandle m_historyNormalWriteHandle;
        RGTextureViewHandle m_reflectionViewHandle;
        RGTextureViewHandle m_sceneColorViewHandle;
        RGTextureViewHandle m_depthViewHandle;
        RGTextureViewHandle m_velocityViewHandle;
        RGTextureViewHandle m_historyViewHandle;
        RGTextureViewHandle m_historyDepthReadViewHandle;
        RGTextureViewHandle m_historyDepthWriteViewHandle;
        RGTextureViewHandle m_historyNormalReadViewHandle;
        RGTextureViewHandle m_historyNormalWriteViewHandle;
        RHITextureRef m_reflectionTexture;
        RHITextureRef m_fallbackVelocityTexture;
        RHIQueryPoolRef m_timingQueryPool;
        std::array<RHIBufferRef, RVX_MAX_FRAME_COUNT> m_timingReadbackBuffers;
        std::array<bool, RVX_MAX_FRAME_COUNT> m_timingReadbackValid{};
        std::array<RHITextureRef, 2> m_historyTextures;
        std::array<RHITextureRef, 2> m_historyDepthTextures;
        std::array<RHITextureRef, 2> m_historyNormalTextures;
        std::array<RHIResourceState, 2> m_historyTextureStates{
            RHIResourceState::Undefined,
            RHIResourceState::Undefined
        };
        std::array<RHIResourceState, 2> m_historyDepthTextureStates{
            RHIResourceState::Undefined,
            RHIResourceState::Undefined
        };
        std::array<RHIResourceState, 2> m_historyNormalTextureStates{
            RHIResourceState::Undefined,
            RHIResourceState::Undefined
        };
        RHIBufferRef m_constantBuffer;
        std::vector<Ref<RefCounted>> m_pendingOwnerRetirements;
        std::shared_ptr<RayTracedReflectionHistoryOwner> m_historyOwner;
        std::shared_ptr<RayTracedReflectionHistoryReservation>
            m_activeHistoryReservation;
        RHIAccelerationStructureRef m_activeTopLevelAS;
        RGBufferHandle m_activeTopLevelASStorageHandle{};
        RHIResourceInstanceId m_activeTopLevelASStorageId{};
        bool m_activeTopLevelASDependencyRequired = false;
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
        uint64 m_historyAllocationGeneration = 1;

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
