#pragma once

/**
 * @file RenderQualityPerformance.h
 * @brief Backend-neutral, fail-closed GFX24 performance evidence contracts.
 */

#include "Core/Types.h"
#include "Core/Hash/SHA256.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "RenderContracts/RenderPipelineProfile.h"

#include <cmath>
#include <string>
#include <vector>

namespace RVX
{
    // This internal telemetry schema is intentionally versioned separately
    // from the host-authored JSON report.  V2 replaces the former
    // name-classified imported-history metric with the actual imported
    // history inventory produced by RenderGraph.
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION = 2;
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT =
        60;
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT =
        300;
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT = 8;

    [[nodiscard]] inline bool IsRenderQualityPerformanceReceiptDigestValid(
        const Hash::SHA256Digest& digest) noexcept
    {
        for (const uint8 value : digest)
        {
            if (value != 0)
                return true;
        }
        return false;
    }

    [[nodiscard]] inline bool IsRenderQualityPerformanceSourceCommitValid(
        const std::string& sourceCommit) noexcept
    {
        if (sourceCommit.size() != 40U && sourceCommit.size() != 64U)
            return false;
        for (const char value : sourceCommit)
        {
            if (!((value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool IsRenderQualityPerformancePresetValid(
        const std::string& preset) noexcept
    {
        if (preset.empty() || preset.size() > 128U)
            return false;
        for (const char value : preset)
        {
            if (!((value >= 'a' && value <= 'z') ||
                  (value >= 'A' && value <= 'Z') ||
                  (value >= '0' && value <= '9') || value == '-' ||
                  value == '_' || value == '.'))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] inline bool IsRenderQualityPerformanceBackendValid(
        const std::string& backend) noexcept
    {
        return backend == "dx11" || backend == "dx12" ||
               backend == "vulkan" || backend == "metal" ||
               backend == "opengl";
    }

    /**
     * @brief Validate one source-bound Graphics prelude-to-terminal span.
     *
     * The arithmetic deliberately remains in RenderContracts so immutable
     * evidence can validate timestamp rollover without depending on a
     * concrete RHI implementation.
     */
    [[nodiscard]] inline bool IsRenderQualityPerformanceGraphicsTerminalSpanValid(
        uint64 startTimestamp,
        uint64 endTimestamp,
        uint64 elapsedTimestampTicks,
        uint64 timestampFrequency,
        uint8 timestampValidBits,
        float64 milliseconds) noexcept
    {
        if (timestampFrequency == 0 || timestampValidBits == 0 ||
            timestampValidBits > 64 ||
            (startTimestamp == 0 && endTimestamp == 0) ||
            !std::isfinite(milliseconds) || milliseconds < 0.0)
        {
            return false;
        }

        const uint64 rawDelta = endTimestamp - startTimestamp;
        const uint64 expectedTicks = timestampValidBits == 64
            ? rawDelta
            : rawDelta & ((uint64{1} << timestampValidBits) - 1U);
        if (elapsedTimestampTicks != expectedTicks)
            return false;

        const float64 expectedMilliseconds =
            (static_cast<float64>(expectedTicks) * 1000.0) /
            static_cast<float64>(timestampFrequency);
        const float64 tolerance = expectedMilliseconds > 1.0
            ? expectedMilliseconds * 1.0e-9
            : 1.0e-9;
        return std::abs(milliseconds - expectedMilliseconds) <= tolerance;
    }

    enum class RenderQualityPerformanceSessionState : uint8
    {
        Idle = 0,
        Armed,
        Collecting,
        Draining,
        Completed,
        Rejected,
    };

    enum class RenderQualityPerformanceFailure : uint8
    {
        None = 0,
        AlreadyActive,
        InvalidHardwareIdentity,
        InvalidFrameIdentity,
        TimestampQueriesUnavailable,
        SlotInitializationFailed,
        SlotSaturated,
        GraphRecordingFailed,
        SubmissionRejected,
        CompletionLost,
        QueryReadbackFailed,
        PassSchemaChanged,
        ReceiptUnavailable,
        ReceiptMismatch,
        PresentationRejected,
        CpuTimingUnavailable,
        ExtractionTimingUnavailable,
        VisibilityTimingUnavailable,
        TelemetryUnavailable,
        UploadCounterDecreased,
        ResourceCountsChanged,
        TrackedEstimatedFootprintChanged,
        FrameOrdinalInvalid,
        SampleCountMismatch,
        DeviceLost,
    };

    [[nodiscard]] inline const char* GetRenderQualityPerformanceFailureName(
        RenderQualityPerformanceFailure failure) noexcept
    {
        switch (failure)
        {
            case RenderQualityPerformanceFailure::None: return "None";
            case RenderQualityPerformanceFailure::AlreadyActive:
                return "AlreadyActive";
            case RenderQualityPerformanceFailure::InvalidHardwareIdentity:
                return "InvalidHardwareIdentity";
            case RenderQualityPerformanceFailure::InvalidFrameIdentity:
                return "InvalidFrameIdentity";
            case RenderQualityPerformanceFailure::TimestampQueriesUnavailable:
                return "TimestampQueriesUnavailable";
            case RenderQualityPerformanceFailure::SlotInitializationFailed:
                return "SlotInitializationFailed";
            case RenderQualityPerformanceFailure::SlotSaturated:
                return "SlotSaturated";
            case RenderQualityPerformanceFailure::GraphRecordingFailed:
                return "GraphRecordingFailed";
            case RenderQualityPerformanceFailure::SubmissionRejected:
                return "SubmissionRejected";
            case RenderQualityPerformanceFailure::CompletionLost:
                return "CompletionLost";
            case RenderQualityPerformanceFailure::QueryReadbackFailed:
                return "QueryReadbackFailed";
            case RenderQualityPerformanceFailure::PassSchemaChanged:
                return "PassSchemaChanged";
            case RenderQualityPerformanceFailure::ReceiptUnavailable:
                return "ReceiptUnavailable";
            case RenderQualityPerformanceFailure::ReceiptMismatch:
                return "ReceiptMismatch";
            case RenderQualityPerformanceFailure::PresentationRejected:
                return "PresentationRejected";
            case RenderQualityPerformanceFailure::CpuTimingUnavailable:
                return "CpuTimingUnavailable";
            case RenderQualityPerformanceFailure::ExtractionTimingUnavailable:
                return "ExtractionTimingUnavailable";
            case RenderQualityPerformanceFailure::VisibilityTimingUnavailable:
                return "VisibilityTimingUnavailable";
            case RenderQualityPerformanceFailure::TelemetryUnavailable:
                return "TelemetryUnavailable";
            case RenderQualityPerformanceFailure::UploadCounterDecreased:
                return "UploadCounterDecreased";
            case RenderQualityPerformanceFailure::ResourceCountsChanged:
                return "ResourceCountsChanged";
            case RenderQualityPerformanceFailure::TrackedEstimatedFootprintChanged:
                return "TrackedEstimatedFootprintChanged";
            case RenderQualityPerformanceFailure::FrameOrdinalInvalid:
                return "FrameOrdinalInvalid";
            case RenderQualityPerformanceFailure::SampleCountMismatch:
                return "SampleCountMismatch";
            case RenderQualityPerformanceFailure::DeviceLost:
                return "DeviceLost";
        }
        return "Unknown";
    }

    /** @brief One measured CPU duration. Zero remains a valid available value. */
    struct RenderQualityPerformanceDuration
    {
        bool available = false;
        uint64 nanoseconds = 0;

        [[nodiscard]] static constexpr RenderQualityPerformanceDuration
        Available(uint64 value) noexcept
        {
            return {true, value};
        }

        [[nodiscard]] static constexpr RenderQualityPerformanceDuration
        Unavailable() noexcept
        {
            return {};
        }

        friend constexpr bool operator==(
            const RenderQualityPerformanceDuration&,
            const RenderQualityPerformanceDuration&) = default;
    };

    /**
     * @brief Exact-frame CPU measurements owned by the rendering pipeline.
     *
     * Extraction is deliberately separate from frameApply. Render must never
     * claim update-thread extraction time from an apply or queue-wait proxy.
     */
    struct RenderQualityPerformanceCpuTiming
    {
        RenderQualityPerformanceDuration extraction{};
        RenderQualityPerformanceDuration visibilityAndDrawPlanning{};
        RenderQualityPerformanceDuration frameApply{};
        RenderQualityPerformanceDuration frameBeginAcquire{};
        RenderQualityPerformanceDuration renderPrepare{};
        RenderQualityPerformanceDuration policy{};
        RenderQualityPerformanceDuration graphBuild{};
        RenderQualityPerformanceDuration graphCompile{};
        RenderQualityPerformanceDuration graphRealizeRecord{};
        RenderQualityPerformanceDuration frameEndSubmit{};
        RenderQualityPerformanceDuration present{};
        RenderQualityPerformanceDuration acceptedFrameTotal{};

        [[nodiscard]] constexpr bool HasAllRequiredMeasurements() const noexcept
        {
            return extraction.available &&
                   visibilityAndDrawPlanning.available && frameApply.available &&
                   frameBeginAcquire.available && renderPrepare.available &&
                   policy.available && graphBuild.available &&
                   graphCompile.available && graphRealizeRecord.available &&
                   frameEndSubmit.available && present.available &&
                   acceptedFrameTotal.available;
        }
    };

    /** @brief Stable identity of the exact accepted RenderExecutionReceipt. */
    struct RenderQualityPerformanceReceiptBinding
    {
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        uint32 receiptSchemaVersion = 0;
        /** SHA-256 over every semantic field in the exact receipt. */
        Hash::SHA256Digest receiptDigest{};
        /** Immutable build/backend provenance from the exact receipt. */
        std::string sourceCommit{};
        std::string preset{};
        std::string backend{};
        RenderPipelineRecipe recipe = RenderPipelineRecipe::ForwardStandard;
        RenderQualityTier quality = RenderQualityTier::Medium;
        uint64 profileHash = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return frameIdentity.IsValid() && viewIdentity.IsValid() &&
                   receiptSchemaVersion != 0 &&
                   IsRenderQualityPerformanceReceiptDigestValid(receiptDigest) &&
                   IsRenderQualityPerformanceSourceCommitValid(sourceCommit) &&
                   IsRenderQualityPerformancePresetValid(preset) &&
                   IsRenderQualityPerformanceBackendValid(backend) &&
                   IsDeclaredRenderPipelineRecipe(recipe) &&
                   IsDeclaredRenderQualityTier(quality) && profileHash != 0 &&
                   frameIdentity.configurationRevision != 0;
        }

        friend constexpr bool operator==(
            const RenderQualityPerformanceReceiptBinding&,
            const RenderQualityPerformanceReceiptBinding&) = default;
    };

    /**
     * @brief Immutable hardware capability provenance captured when the run arms.
     *
     * Adapter, driver and host OS identity are intentionally not synthesized by
     * Render. A host writer may add those independently only when it can bind
     * them to this backend and capability hash.
     */
    struct RenderQualityPerformanceHardwareIdentity
    {
        std::string backend{};
        std::string capabilityReportSha256{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            if (!IsRenderQualityPerformanceBackendValid(backend) ||
                capabilityReportSha256.size() != 64U)
            {
                return false;
            }
            for (const char value : capabilityReportSha256)
            {
                if (!((value >= '0' && value <= '9') ||
                      (value >= 'a' && value <= 'f')))
                {
                    return false;
                }
            }
            return true;
        }

        friend bool operator==(
            const RenderQualityPerformanceHardwareIdentity&,
            const RenderQualityPerformanceHardwareIdentity&) = default;
    };

    /**
     * @brief The deliberately bounded upload sources represented in GFX24.
     *
     * This is not a claim about every upload submitted by the process.  The
     * performance run has exact accounting only for the completion-qualified
     * mutation streams plus the particle and water frame uploads needed by the
     * RenderQualityLab scene.  A writer must preserve this mask with every
     * sample instead of labelling the value as total GPU upload traffic.
     */
    enum class RenderQualityPerformanceTrackedUploadCoverage : uint32
    {
        None = 0,
        CompletionQualifiedMutationStreams = 1U << 0U,
        ParticleFrameUpload = 1U << 1U,
        WaterFrameUpload = 1U << 2U,
    };

    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE =
        static_cast<uint32>(
            RenderQualityPerformanceTrackedUploadCoverage::
                CompletionQualifiedMutationStreams) |
        static_cast<uint32>(
            RenderQualityPerformanceTrackedUploadCoverage::
                ParticleFrameUpload) |
        static_cast<uint32>(
            RenderQualityPerformanceTrackedUploadCoverage::WaterFrameUpload);

    /**
     * @brief The deliberately bounded estimated footprint sources in GFX24.
     *
     * No field in this structure represents total GPU memory.  The coverage
     * mask records precisely which renderer inventories contributed to the
     * estimate, distinct imported Persistent and History inventories, and the
     * resource registry.
     */
    enum class RenderQualityPerformanceTrackedFootprintCoverage : uint32
    {
        None = 0,
        TransientPoolPhysicalEstimate = 1U << 0U,
        ImportedHistoryResourceEstimate = 1U << 1U,
        ResourceRegistryResidentEstimate = 1U << 2U,
        ImportedPersistentResourceEstimate = 1U << 3U,
    };

    inline constexpr uint32
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE =
            static_cast<uint32>(
                RenderQualityPerformanceTrackedFootprintCoverage::
                    TransientPoolPhysicalEstimate) |
            static_cast<uint32>(
                RenderQualityPerformanceTrackedFootprintCoverage::
                    ImportedHistoryResourceEstimate) |
            static_cast<uint32>(
                RenderQualityPerformanceTrackedFootprintCoverage::
                    ResourceRegistryResidentEstimate) |
            static_cast<uint32>(
                RenderQualityPerformanceTrackedFootprintCoverage::
                    ImportedPersistentResourceEstimate);

    /**
     * @brief Same-frame tracked estimated footprint, not total GPU memory.
     *
     * The schema and coverage are machine-readable so writer-side evidence
     * cannot silently reinterpret this partial inventory as a full allocator
     * or driver memory report.
     */
    struct RenderQualityPerformanceTrackedEstimatedFootprint
    {
        uint32 schemaVersion = RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION;
        uint32 coverageMask = 0;
        uint64 transientPoolPhysicalEstimatedBytes = 0;
        uint64 importedPersistentEstimatedBytes = 0;
        uint64 importedHistoryEstimatedBytes = 0;
        uint64 resourceRegistryResidentEstimatedBytes = 0;
        bool available = false;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return available &&
                   schemaVersion ==
                       RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION &&
                   coverageMask ==
                       RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_FOOTPRINT_COVERAGE;
        }

        friend constexpr bool operator==(
            const RenderQualityPerformanceTrackedEstimatedFootprint&,
            const RenderQualityPerformanceTrackedEstimatedFootprint&) = default;
    };

    /**
     * @brief Stable resource inventory captured with the accepted frame.
     *
     * Transient counts describe actual pooled physical allocations.
     * Stable-imported counts describe only imported backing that has a stable
     * Persistent or History lifetime. Per-frame External imports are
     * intentionally excluded: their graph use can vary frame-to-frame without
     * representing warm-up resource growth.
     */
    struct RenderQualityPerformanceResourceCounts
    {
        uint32 schemaVersion = RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION;
        uint32 transientTextureCount = 0;
        uint32 transientBufferCount = 0;
        uint32 stableImportedTextureCount = 0;
        uint32 stableImportedBufferCount = 0;
        uint32 residentMeshCount = 0;
        uint32 residentTextureCount = 0;
        uint32 residentMaterialCount = 0;
        bool available = false;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return available &&
                   schemaVersion ==
                       RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION;
        }

        friend constexpr bool operator==(
            const RenderQualityPerformanceResourceCounts&,
            const RenderQualityPerformanceResourceCounts&) = default;
    };

    /**
     * @brief Render-owner same-frame telemetry frozen at the Present boundary.
     *
     * cumulativeTrackedUploadBytes is a monotonic total from the declared
     * completion-qualified mutation streams. frameTrackedUploadBytes contains
     * the separately recorded particle and water uploads for this exact frame.
     * The session combines the safe cumulative delta with the frame value
     * before the slot becomes completion-owned.
     */
    struct RenderQualityPerformanceFrameTelemetry
    {
        RenderFrameIdentity frameIdentity{};
        uint32 schemaVersion = RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION;
        uint32 trackedUploadCoverageMask = 0;
        uint64 cumulativeTrackedUploadBytes = 0;
        uint64 frameTrackedUploadBytes = 0;
        bool cumulativeTrackedUploadBytesAvailable = false;
        RenderQualityPerformanceTrackedEstimatedFootprint
            trackedEstimatedFootprint{};
        RenderQualityPerformanceResourceCounts resourceCounts{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return frameIdentity.IsValid() &&
                   schemaVersion ==
                       RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION &&
                   trackedUploadCoverageMask ==
                       RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE &&
                   cumulativeTrackedUploadBytesAvailable &&
                   trackedEstimatedFootprint.IsValid() &&
                   resourceCounts.IsValid();
        }

        friend constexpr bool operator==(
            const RenderQualityPerformanceFrameTelemetry&,
            const RenderQualityPerformanceFrameTelemetry&) = default;
    };

    /** @brief One exact query-pair result from the completed graph submission. */
    struct RenderQualityPerformancePassTiming
    {
        uint32 executionIndex = 0;
        std::string passName{};
        uint64 startTimestamp = 0;
        uint64 endTimestamp = 0;
        uint64 elapsedTimestampTicks = 0;
        float64 durationMilliseconds = 0.0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return !passName.empty() &&
                   (startTimestamp != 0 || endTimestamp != 0) &&
                   endTimestamp >= startTimestamp &&
                   elapsedTimestampTicks == endTimestamp - startTimestamp &&
                   std::isfinite(durationMilliseconds) &&
                   durationMilliseconds >= 0.0;
        }
    };

    /** @brief One completion-owned measured sample; warmup frames are not stored. */
    struct RenderQualityPerformanceSample
    {
        uint32 sampleIndex = 0;
        RenderQualityPerformanceReceiptBinding binding{};
        RenderQualityPerformanceCpuTiming cpu{};
        /**
         * @brief Session-owned ordinal assigned at exact Present/submission.
         *
         * This is deliberately not a RenderFrameIdentity sequence or a GPU
         * fence value. It names the one Graphics submission that owns this
         * evidence across backend timeline domains.
         */
        uint64 submittedFrame = 0;
        /** @brief Same session ordinal only after its Graphics work completes. */
        uint64 completedFrame = 0;
        uint64 completionValue = 0;
        uint64 timestampFrequency = 0;
        uint64 passSchemaHash = 0;
        /** @brief Per-frame bytes from the declared tracked upload coverage. */
        uint64 trackedUploadBytes = 0;
        uint32 trackedUploadCoverageMask = 0;
        RenderQualityPerformanceTrackedEstimatedFootprint
            trackedEstimatedFootprint{};
        RenderQualityPerformanceResourceCounts resourceCounts{};
        /**
         * @brief Exact Graphics prelude-to-terminal command span from RenderContext.
         *
         * This source-bound pair begins at the Graphics prelude and ends at
         * the terminal Graphics gateway. It measures only that Graphics GPU
         * command interval: it excludes CPU work, submission and Present.
         * The RenderGraph envelope below is retained solely to contextualize
         * the per-pass breakdown.
         */
        uint64 graphicsTerminalCommandSpanStartTimestamp = 0;
        uint64 graphicsTerminalCommandSpanEndTimestamp = 0;
        uint64 graphicsTerminalCommandSpanElapsedTimestampTicks = 0;
        uint8 graphicsTerminalCommandSpanTimestampValidBits = 0;
        float64 graphicsTerminalCommandSpanMilliseconds = 0.0;

        /**
         * @brief First-to-last RenderGraph pass timestamp envelope.
         *
         * Pass timings omit command prelude/epilogue work, so this is a
         * diagnostic breakdown only and must never be reported as a whole
         * frame GPU duration.
         */
        uint64 graphStartTimestamp = 0;
        uint64 graphEndTimestamp = 0;
        float64 graphSpanMilliseconds = 0.0;
        std::vector<RenderQualityPerformancePassTiming> passTimings{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            if (!binding.IsValid() || !cpu.HasAllRequiredMeasurements() ||
                submittedFrame == 0 || completedFrame < submittedFrame ||
                completionValue == 0 || timestampFrequency == 0 ||
                passSchemaHash == 0 ||
                trackedUploadCoverageMask !=
                    RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_UPLOAD_COVERAGE ||
                !trackedEstimatedFootprint.IsValid() ||
                !resourceCounts.IsValid() ||
                !IsRenderQualityPerformanceGraphicsTerminalSpanValid(
                    graphicsTerminalCommandSpanStartTimestamp,
                    graphicsTerminalCommandSpanEndTimestamp,
                    graphicsTerminalCommandSpanElapsedTimestampTicks,
                    timestampFrequency,
                    graphicsTerminalCommandSpanTimestampValidBits,
                    graphicsTerminalCommandSpanMilliseconds) ||
                graphEndTimestamp < graphStartTimestamp ||
                !std::isfinite(graphSpanMilliseconds) ||
                graphSpanMilliseconds < 0.0 || passTimings.empty())
            {
                return false;
            }
            for (uint32 index = 0; index < passTimings.size(); ++index)
            {
                if (passTimings[index].executionIndex != index ||
                    !passTimings[index].IsValid())
                {
                    return false;
                }
            }
            return true;
        }
    };

    /** @brief Immutable evidence published only after exactly 60 + 300 completions. */
    struct RenderQualityPerformanceEvidence
    {
        uint32 schemaVersion = RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION;
        uint32 requiredWarmupSamples =
            RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT;
        uint32 requiredMeasuredSamples =
            RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT;
        uint32 completedWarmupSamples = 0;
        uint32 completedMeasuredSamples = 0;
        uint32 slotCapacity = RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT;
        uint32 slotHighWaterMark = 0;
        RenderQualityPerformanceHardwareIdentity hardware{};
        uint64 passSchemaHash = 0;
        std::vector<RenderQualityPerformanceSample> samples{};

        [[nodiscard]] bool IsComplete() const noexcept
        {
            if (schemaVersion != RVX_RENDER_QUALITY_PERFORMANCE_SCHEMA_VERSION ||
                requiredWarmupSamples !=
                    RVX_RENDER_QUALITY_PERFORMANCE_WARMUP_SAMPLE_COUNT ||
                requiredMeasuredSamples !=
                    RVX_RENDER_QUALITY_PERFORMANCE_MEASURED_SAMPLE_COUNT ||
                completedWarmupSamples != requiredWarmupSamples ||
                completedMeasuredSamples != requiredMeasuredSamples ||
                slotCapacity == 0 || slotHighWaterMark == 0 ||
                slotHighWaterMark > slotCapacity || !hardware.IsValid() ||
                passSchemaHash == 0 ||
                samples.size() != requiredMeasuredSamples)
            {
                return false;
            }
            uint64 previousSubmittedFrame = 0;
            uint64 previousCompletedFrame = 0;
            for (uint32 index = 0; index < samples.size(); ++index)
            {
                if (samples[index].sampleIndex != index ||
                    samples[index].passSchemaHash != passSchemaHash ||
                    samples[index].binding.backend != hardware.backend ||
                    samples[index].submittedFrame <= previousSubmittedFrame ||
                    samples[index].completedFrame <= previousCompletedFrame ||
                    !samples[index].IsValid())
                {
                    return false;
                }
                previousSubmittedFrame = samples[index].submittedFrame;
                previousCompletedFrame = samples[index].completedFrame;
            }
            return true;
        }
    };

    /** @brief Lightweight status projected while the Render-owned session runs. */
    struct RenderQualityPerformanceSessionDiagnostics
    {
        RenderQualityPerformanceSessionState state =
            RenderQualityPerformanceSessionState::Idle;
        RenderQualityPerformanceFailure failure =
            RenderQualityPerformanceFailure::None;
        uint32 completedWarmupSamples = 0;
        uint32 completedMeasuredSamples = 0;
        uint32 inFlightSlots = 0;
        uint32 slotCapacity = RVX_RENDER_QUALITY_PERFORMANCE_SLOT_COUNT;
        uint32 slotHighWaterMark = 0;
        uint64 passSchemaHash = 0;
        std::string diagnostic{};

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == RenderQualityPerformanceSessionState::Completed ||
                   state == RenderQualityPerformanceSessionState::Rejected;
        }
    };
} // namespace RVX
