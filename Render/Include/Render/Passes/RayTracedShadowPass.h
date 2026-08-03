#pragma once

/**
 * @file RayTracedShadowPass.h
 * @brief Ray-traced directional shadow mask pass
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Passes/ShadowPass.h"

#include <memory>

namespace RVX
{
    class PipelineCache;
    class RayTracingSceneManager;
    class RenderRetirementQueue;
    class ResourceViewCache;
    class RenderResourceRegistry;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;
    struct RayTracedShadowFrameState;
    struct RayTracedShadowHistoryOwner;

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
        void AddToGraph(RenderGraph& graph, const ViewData& view) override;
        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        /** @brief Commit one recorded temporal-history reservation after submission. */
        void NotifySubmission(const RenderPassRecordIdentity& identity,
                              const GPUCompletionToken& completion);
        /** @brief Discard one recorded temporal-history reservation before submission. */
        void ReleaseUnsubmittedFrame(const RenderPassRecordIdentity& identity);
        /** @brief Bounded legacy adapter; only forwards a sole legacy recording. */
        void NotifySubmission(const GPUCompletionToken& completion);
        void ReleaseUnsubmittedFrame();
        void RetireOwnerSnapshots(const GPUCompletionToken& completion,
                                  RenderRetirementQueue& retirement);
        /** @brief Non-blocking completion poll for submitted timing diagnostics. */
        void RefreshCompletionDiagnostics();

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
        void SetSubmissionTracker(RenderSubmissionTracker* submissionTracker)
        {
            m_submissionTracker = submissionTracker;
        }
        void SetConfig(const ShadowPassConfig& config) { m_config = config; }
        const ShadowPassConfig& GetConfig() const { return m_config; }
        void SetDirectionalLight(const Vec3& direction, const Vec3& color, float intensity);

        /** @brief Last submitted diagnostics; graph consumers must use record results. */
        const RayTracedShadowPassStats& GetStats() const { return m_lastSubmittedStats; }

    private:
        IRHIDevice* m_device = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        RayTracingSceneManager* m_sceneManager = nullptr;
        RenderSubmissionTracker* m_submissionTracker = nullptr;
        ShadowPassConfig m_config;
        Vec3 m_lightDirection{0.0f, -1.0f, 0.0f};
        Vec3 m_lightColor{1.0f, 1.0f, 1.0f};
        float m_lightIntensity = 0.0f;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Ray traced shadows have not been requested";

        std::shared_ptr<RayTracedShadowHistoryOwner> m_historyOwner;
        RayTracedShadowPassStats m_lastSubmittedStats;

        bool CreateFrameConstantBuffer(RayTracedShadowFrameState& state) const;
        bool CreateFrameTimingResources(RayTracedShadowFrameState& state) const;
        bool CreateFrameFallbackTextures(RayTracedShadowFrameState& state) const;
        void PollCompletedTimingSamples();
        bool UpdateConstants(RayTracedShadowFrameState& state) const;
    };

} // namespace RVX
