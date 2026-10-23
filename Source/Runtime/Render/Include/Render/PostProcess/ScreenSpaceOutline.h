#pragma once

/**
 * @file ScreenSpaceOutline.h
 * @brief Typed HDR toon silhouette and crease post-process pass.
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "RHI/RHIResources.h"

#include <optional>
#include <string>

namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;
    class IRHIDevice;
    class RenderSubmissionTracker;
    struct GPUCompletionToken;

    /** Renderer-owned controls shared by every ToonLit material outline. */
    struct ScreenSpaceOutlineConfig
    {
        float depthThreshold = 0.015f;
        float normalThreshold = 0.20f;
    };

    /** Exact frame/view identity of one successfully recorded outline draw. */
    struct ScreenSpaceOutlineExecutionIdentity
    {
        uint64 frameIndex = 0;
        RenderViewHistoryIdentity historyIdentity{};
        RenderOutputExtent extent{};
        uint64 settingsHash = 0;
        uint64 materialTableContentHash = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return frameIndex != 0 && historyIdentity.IsValid() &&
                   extent.IsValid() && extent == historyIdentity.extent &&
                   settingsHash != 0 && materialTableContentHash != 0;
        }

        friend bool operator==(
            const ScreenSpaceOutlineExecutionIdentity&,
            const ScreenSpaceOutlineExecutionIdentity&) = default;
    };

    /** The qualification probe observes the same exact visible draw identity. */
    using ScreenSpaceOutlineQualificationProbeIdentity =
        ScreenSpaceOutlineExecutionIdentity;

    /** CPU-readable result of the qualification-only GPU mask observation. */
    struct ScreenSpaceOutlineQualificationProbeStats
    {
        bool requested = false;
        bool outlineDrawRecorded = false;
        bool verifierDrawRecorded = false;
        bool copyRecorded = false;
        bool submissionAccepted = false;
        bool presented = false;
        bool gpuCompleted = false;
        bool ready = false;
        bool resolutionAware = false;
        ScreenSpaceOutlineQualificationProbeIdentity identity{};
        uint64 sampleCount = 0;
        uint64 outlinedPixelCount = 0;
        uint64 nonToonAffectedPixelCount = 0;
        std::string failure;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return requested && outlineDrawRecorded && verifierDrawRecorded &&
                   copyRecorded && submissionAccepted && presented &&
                   gpuCompleted && ready && resolutionAware &&
                   identity.IsValid() && sampleCount != 0 &&
                   outlinedPixelCount <= sampleCount &&
                   nonToonAffectedPixelCount <= sampleCount;
        }
    };

    /**
     * @brief Full-resolution material-aware outline in pre-exposed HDR.
     *
     * The pass never infers a stylized surface from color. It consumes the
     * canonical depth, oct-normal, material-id, and MaterialGPUConstants
     * table produced by opaque geometry, then only darkens pixels whose exact
     * material row is ToonLit with a positive outline width.
     */
    class ScreenSpaceOutlinePass final : public IPostProcessPass
    {
    public:
        ScreenSpaceOutlinePass();
        ~ScreenSpaceOutlinePass() override = default;

        [[nodiscard]] PostProcessPassDescriptor Describe() const override;
        [[nodiscard]] const char* GetName() const override
        {
            return "ScreenSpaceOutline";
        }
        [[nodiscard]] int32 GetPriority() const override { return 800; }
        void Configure(const PostProcessSettings& settings) override;
        [[nodiscard]] PostProcessFrameInputRequirements
        GetFrameInputRequirements() const override;
        [[nodiscard]] bool ValidateFrameInputs(
            const PostProcessFrameInputs& inputs,
            std::string& outReason) const override;
        void AddToGraph(RenderGraph& graph,
                        RGTextureHandle input,
                        RGTextureHandle output) override;
        void AddToGraph(RenderGraph& graph,
                        const PostProcessFrameInputs& frameInputs,
                        RGTextureHandle output) override;

        /** Bind cooked pipeline/layout resources at the renderer lifecycle boundary. */
        void SetResources(PipelineCache* pipelineCache,
                          ResourceViewCache* viewCache);

        [[nodiscard]] const ScreenSpaceOutlineConfig& GetConfig() const noexcept
        {
            return m_config;
        }
        /** Value-only hash projected into normal execution receipts. */
        [[nodiscard]] uint64 GetSettingsHash() const noexcept;
        /**
         * @brief Return the exact identity only after a visible draw was encoded.
         *
         * Planning or adding a graph node is deliberately insufficient.  The
         * identity is published by the execution callback after Draw(), and is
         * cleared before every subsequent recording attempt.
         */
        [[nodiscard]] std::optional<ScreenSpaceOutlineExecutionIdentity>
        GetLastExecutionIdentity() const noexcept;

        /**
         * @brief Arm one qualification-only physical mask readback.
         *
         * The request intentionally has no predicted view identity. The next
         * successful AddToGraph recording locks the exact frame/view/extent,
         * settings, and material-table identity; requests that never reach a
         * real draw/copy cannot become ready.
         */
        [[nodiscard]] bool ArmQualificationProbe() noexcept;
        [[nodiscard]] bool NotifyQualificationProbeSubmission(
            const ScreenSpaceOutlineQualificationProbeIdentity& identity,
            const GPUCompletionToken& completion,
            const RenderSubmissionTracker& tracker) noexcept;
        [[nodiscard]] bool MarkQualificationProbePresented(
            const ScreenSpaceOutlineQualificationProbeIdentity& identity) noexcept;
        [[nodiscard]] bool PollQualificationProbe(
            const RenderSubmissionTracker& tracker,
            ScreenSpaceOutlineQualificationProbeStats& outStats) noexcept;
        [[nodiscard]] bool HasPendingQualificationProbe() const noexcept;
        void AbandonQualificationProbe() noexcept;
        [[nodiscard]] std::optional<ScreenSpaceOutlineQualificationProbeIdentity>
        GetPendingQualificationProbeIdentity() const noexcept;

    private:
        void RefreshSupportState();
        [[nodiscard]] bool EnsureRuntimeResources();
        [[nodiscard]] bool EnsureQualificationReadback(
            uint32 width, uint32 height) noexcept;
        void BeginExecutionRecording() noexcept;
        void ClearQualificationProbeState(bool retainReadback) noexcept;

        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHISamplerRef m_pointClampSampler;
        ScreenSpaceOutlineConfig m_config{};
        ScreenSpaceOutlineExecutionIdentity m_lastExecutionIdentity{};
        uint64 m_executionGeneration = 0;
        RHIBufferRef m_probeReadback;
        ScreenSpaceOutlineQualificationProbeIdentity m_probeIdentity{};
        uint64 m_probeGeneration = 0;
        uint64 m_probeCompletionValue = 0;
        uint32 m_probeWidth = 0;
        uint32 m_probeHeight = 0;
        uint32 m_probeRowPitch = 0;
        bool m_probeRequested = false;
        bool m_probeIdentityLocked = false;
        bool m_probeOutlineDrawRecorded = false;
        bool m_probeVerifierDrawRecorded = false;
        bool m_probeCopyRecorded = false;
        bool m_probeSubmissionAccepted = false;
        bool m_probePresented = false;
        bool m_probeAbandoned = false;
        ScreenSpaceOutlineQualificationProbeStats m_lastProbeStats{};
    };
} // namespace RVX
