/**
 * @file TAA.h
 * @brief History-safe temporal anti-aliasing post-process pass.
 */

#pragma once

#include "Core/Types.h"
#include "Render/PostProcess/PostProcessStack.h"

#include <string>

namespace RVX
{
    class IRHIDevice;
    class PipelineCache;
    class ResourceViewCache;
    /** @brief Tunable, backend-neutral temporal resolve parameters. */
    struct TAAConfig
    {
        float feedbackMin = 0.88f;
        float feedbackMax = 0.97f;
        float motionScale = 1.0f;
        float velocityWeight = 1000.0f;
        float depthRejectScale = 0.01f;
        float reactiveMaskScale = 1.0f;
        bool sharpen = true;
        float sharpness = 0.25f;
    };

    /** @brief Receipt for the last exact TAA candidate or presentation commit. */
    struct TAAResolveStats
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool executed = false;
        bool historyAvailableBefore = false;
        bool historySampled = false;
        bool historyConsumed = false;
        bool historyCandidatePrepared = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        uint64 exposureStateContentHash = 0;
        uint64 frameIndex = 0;
        Vec2 currentJitterPixels = Vec2(0.0f);
        Vec2 previousJitterPixels = Vec2(0.0f);
        uint32 resolveGraphPassCount = 0;
        /** Independent history-store passes; fused resolve requires zero. */
        uint32 historyStoreGraphPassCount = 0;
        RenderViewHistoryIdentity historyIdentity{};
        RenderHistoryResetReason historyResetReasons =
            RenderHistoryResetReason::HistoryUnavailable;
        std::string fallbackReason;
    };

    /**
     * @brief HDR temporal resolve with explicit candidate-history presentation.
     *
     * One resolve graph pass writes the post-process output and both persistent
     * history candidates through three MRT color attachments. The class never
     * promotes a written history target itself; composition must call
     * CommitHistoryAfterPresent() for the matching successfully presented view.
     */
    class TAA final : public IPostProcessPass
    {
    public:
        TAA();
        ~TAA() override;

        TAA(const TAA&) = delete;
        TAA& operator=(const TAA&) = delete;

        // =========================================================================
        // IPostProcessPass
        // =========================================================================

        PostProcessPassDescriptor Describe() const override;
        const char* GetName() const override { return "TemporalAntiAliasing"; }
        int32 GetPriority() const override { return 0; }
        void Configure(const PostProcessSettings& settings) override;
        PostProcessFrameInputRequirements GetFrameInputRequirements() const override;
        void AddToGraph(RenderGraph& graph,
                        RGTextureHandle input,
                        RGTextureHandle output) override;
        void AddToGraph(RenderGraph& graph,
                        const PostProcessFrameInputs& frameInputs,
                        RGTextureHandle output) override;

        // =========================================================================
        // Resources and configuration
        // =========================================================================

        void Initialize(IRHIDevice* device, uint32 width, uint32 height);
        void Shutdown();
        void Resize(uint32 width, uint32 height);
        bool IsInitialized() const { return m_device != nullptr; }

        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);
        const TAAConfig& GetConfig() const { return m_config; }
        void SetConfig(const TAAConfig& config);

        bool IsRequestedEnabled() const { return m_enabled; }
        const TAAResolveStats& GetLastResolveStats() const { return m_lastResolveStats; }
        bool HasValidHistory() const { return m_historyValid; }

        // =========================================================================
        // History lifecycle
        // =========================================================================

        /**
         * @brief Returns whether the exact pending candidate may be promoted.
         *
         * This is intentionally side-effect-free so composition can establish
         * the full dual-history commit precondition before mutating either
         * history owner after a successful Present.
         */
        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) const noexcept;
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& identity) noexcept;
        void AbandonPendingHistory() noexcept;
        void ResetHistory();

    private:
        bool EnsureRuntimeResources(uint32 width, uint32 height);
        bool HasPipelineResources() const noexcept;
        void ResetRuntimeResources();
        void RecordUnsupported(const char* reason,
                               const PostProcessFrameInputs* frameInputs = nullptr);

        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        TAAConfig m_config{};
        uint32 m_width = 0;
        uint32 m_height = 0;

        RHITextureRef m_historyColor[2];
        RHITextureRef m_historyDepth[2];
        uint32 m_committedHistoryIndex = 0;
        uint32 m_pendingHistoryIndex = 0;
        /** True after the graph has established SRV layouts for all four histories. */
        bool m_historyResourceStatesInitialized = false;
        bool m_historyValid = false;
        bool m_hasPendingHistory = false;
        RenderViewHistoryIdentity m_pendingHistoryIdentity{};
        Vec2 m_previousJitterPixels = Vec2(0.0f);
        Vec2 m_pendingJitterPixels = Vec2(0.0f);
        RHISamplerRef m_sampler;
        TAAResolveStats m_lastResolveStats{};
    };
} // namespace RVX
