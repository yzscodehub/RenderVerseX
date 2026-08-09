#pragma once

/** @file SampleLifetimeQualification.h @brief Sustained sample lifetime gate. */

#include "Core/Types.h"
#include "Render/RenderDiagnostics.h"
#include "RHI/RHIDefinitions.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr const char* RVX_SAMPLE_LIFETIME_REPORT_SCHEMA_ID =
        "RVX.SampleLifetimeQualification";
    inline constexpr uint32 RVX_SAMPLE_LIFETIME_REPORT_SCHEMA_VERSION = 1;

    struct SampleLifetimeQualificationConfig
    {
        uint32 warmupFrames = 120;
        uint32 observationFrames = 480;
        uint32 minimumDurationMs = 0;
        uint32 resizeIntervalFrames = 0;
        uint32 resizeSettleFrames = 30;
    };

    struct SampleLifetimeQualificationMetadata
    {
        std::string sampleName;
        std::string assetId;
        RHIBackendType backend = RHIBackendType::None;
        std::string renderPath;
        uint32 width = 0;
        uint32 height = 0;
        bool deterministicOrbit = false;
    };

    struct SampleProcessMemorySnapshot
    {
        bool available = false;
        uint64 privateBytes = 0;
    };

    struct SampleLifetimeMetricSnapshot
    {
        uint64 frameSequence = 0;
        uint64 planHash = 0;
        uint32 physicalRealizationCount = 0;
        uint32 physicalTextureAllocationCount = 0;
        uint32 physicalBufferAllocationCount = 0;
        uint64 totalPooledMemoryBytes = 0;
        uint32 transientViewCount = 0;
        uint64 surfaceIncompatibleFrameDrops = 0;
        uint32 inFlightTextureLeases = 0;
        uint32 inFlightBufferLeases = 0;
        uint64 leaseCommitCount = 0;
        uint64 leaseAbortCount = 0;
        uint64 completionRetirementCount = 0;
        uint32 renderTargetActiveDescriptors = 0;
        uint32 renderTargetPeakDescriptors = 0;
        uint32 depthStencilActiveDescriptors = 0;
        uint32 depthStencilPeakDescriptors = 0;
        bool nativeValidationAvailable = false;
        bool nativeValidationEnabled = false;
        bool nativeValidationReadComplete = true;
        uint64 nativeValidationWarningCount = 0;
        uint64 nativeValidationErrorCount = 0;
        uint64 nativeValidationCorruptionCount = 0;
        uint64 processPrivateBytes = 0;
        uint64 gpuUsedMemoryBytes = 0;
    };

    struct SampleLifetimeTrend
    {
        bool available = false;
        uint32 sampleCount = 0;
        uint64 firstWindowAverageBytes = 0;
        uint64 lastWindowAverageBytes = 0;
        uint64 peakBytes = 0;
        int64 netGrowthBytes = 0;
        float64 slopeBytesPerFrame = 0.0;
    };

    struct SampleLifetimeQualificationReport
    {
        std::string schemaId = RVX_SAMPLE_LIFETIME_REPORT_SCHEMA_ID;
        uint32 schemaVersion = RVX_SAMPLE_LIFETIME_REPORT_SCHEMA_VERSION;
        SampleLifetimeQualificationMetadata metadata{};
        SampleLifetimeQualificationConfig config{};
        uint32 readyFrames = 0;
        uint32 warmupFramesObserved = 0;
        uint32 observationFramesObserved = 0;
        uint32 resizeCount = 0;
        uint32 plateauCount = 0;
        uint32 planHashChangeCount = 0;
        uint64 elapsedMs = 0;
        SampleLifetimeMetricSnapshot baseline{};
        SampleLifetimeMetricSnapshot final{};
        SampleLifetimeMetricSnapshot peak{};
        SampleLifetimeTrend processPrivateMemory{};
        SampleLifetimeTrend gpuMemory{};
        std::vector<std::string> failures{};
        bool complete = false;
        bool pass = false;
    };

    /** @brief Constant-memory plateau validator for sustained product samples. */
    class SampleLifetimeQualification final
    {
    public:
        SampleLifetimeQualification(
            const SampleLifetimeQualificationConfig& config,
            SampleLifetimeQualificationMetadata metadata);

        /** @brief Observe one post-submit frame after sample RenderReady. */
        void Observe(const RenderDiagnosticsSnapshot& diagnostics,
                     const SampleProcessMemorySnapshot& processMemory,
                     uint64 elapsedMs);

        /** @brief Allow a new resource plateau after a formal resize request. */
        void NotifyResize();
        /** @brief Fail closed when the host cannot complete an external action. */
        void Fail(std::string reason);

        [[nodiscard]] bool IsComplete() const noexcept;
        [[nodiscard]] bool HasFailed() const noexcept;
        [[nodiscard]] bool ShouldRequestResize() const noexcept;
        [[nodiscard]] const SampleLifetimeQualificationReport& GetReport() const noexcept;

    private:
        struct TrendState
        {
            static constexpr uint32 WindowSize = 32;
            std::array<uint64, WindowSize> firstWindow{};
            std::array<uint64, WindowSize> lastWindow{};
            uint32 firstCount = 0;
            uint32 lastCount = 0;
            uint32 lastIndex = 0;
            uint32 count = 0;
            float64 sumX = 0.0;
            float64 sumY = 0.0;
            float64 sumXX = 0.0;
            float64 sumXY = 0.0;
            uint64 peak = 0;

            void Add(uint64 value);
            [[nodiscard]] SampleLifetimeTrend Build() const;
        };

        void CapturePlateau(const SampleLifetimeMetricSnapshot& metrics);
        void ValidateObservation(
            const RenderGraphLifetimeDiagnostics& graph,
            const SampleLifetimeMetricSnapshot& metrics);
        void CompleteIfSatisfied(uint64 elapsedMs);

        SampleLifetimeQualificationReport m_report{};
        SampleLifetimeMetricSnapshot m_plateauBaseline{};
        uint32 m_warmupMaxInFlightTextures = 0;
        uint32 m_warmupMaxInFlightBuffers = 0;
        uint32 m_settleFramesRemaining = 0;
        uint32 m_framesSinceResize = 0;
        bool m_plateauReady = false;
        TrendState m_processTrend{};
        TrendState m_gpuTrend{};
    };

    [[nodiscard]] SampleProcessMemorySnapshot CaptureSampleProcessMemory();
    bool WriteSampleLifetimeQualificationReport(
        const SampleLifetimeQualificationReport& report,
        const std::filesystem::path& path,
        std::string* outError = nullptr);
} // namespace RVX
