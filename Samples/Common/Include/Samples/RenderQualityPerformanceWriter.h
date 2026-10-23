#pragma once

/**
 * @file RenderQualityPerformanceWriter.h
 * @brief Fail-closed host writer for completion-owned RenderQualityLab timing evidence.
 *
 * This is deliberately a serialization boundary.  The caller must project the
 * immutable, Render-owned session result into the value types below before
 * calling it; the writer never consults a later Render diagnostics snapshot.
 */

#include "Core/Types.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_QUALITY_PERFORMANCE_REPORT_SCHEMA_VERSION =
        3;
    inline constexpr uint32
        RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION = 2;
    inline constexpr uint32
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_UPLOAD_COVERAGE =
            (1U << 0U) | (1U << 1U) | (1U << 2U);
    inline constexpr uint32
        RVX_RENDER_QUALITY_PERFORMANCE_REQUIRED_TRACKED_FOOTPRINT_COVERAGE =
            (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 3U);

    struct RenderQualityPerformanceReportIdentity
    {
        std::string sampleId{};
        std::string pipeline{};
        std::string quality{};
        std::string backend{};
        std::string view{};
        uint32 qualificationWidth = 0;
        uint32 qualificationHeight = 0;
        uint64 captureTick = 0;
        uint32 visualContractVersion = 0;
    };

    /**
     * @brief Hardware provenance frozen when the Render-owned session is armed.
     *
     * Adapter, driver, OS and capabilities are not reconstructed at write
     * time: a later device recovery or adapter change would otherwise create a
     * report whose samples and hardware provenance belong to different runs.
     */
    struct SampleRenderQualityPerformanceHardwareIdentity
    {
        std::string backend{};
        std::string adapterName{};
        std::string driverVersion{};
        std::string osBuild{};
        std::string capabilityHash{};
    };

    /**
     * @brief Machine-readable scope for bounded GFX24 telemetry.
     *
     * Upload values cover only completion-qualified mutations plus particle
     * and water frame uploads. Footprint values are estimates from the
     * transient pool, imported Persistent and History resources, and the
     * resource registry; they
     * are not allocator- or driver-wide GPU-memory counters.
     */
    struct RenderQualityPerformanceTelemetryCoverage
    {
        uint32 schemaVersion =
            RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION;
        uint32 trackedUploadCoverageMask = 0;
        uint32 trackedEstimatedFootprintCoverageMask = 0;
    };

    /** @brief Same-frame, deliberately bounded GPU footprint estimate. */
    struct SampleRenderQualityPerformanceTrackedEstimatedFootprint
    {
        uint32 schemaVersion =
            RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION;
        uint32 coverageMask = 0;
        uint64 transientPoolPhysicalEstimatedBytes = 0;
        uint64 importedPersistentEstimatedBytes = 0;
        uint64 importedHistoryEstimatedBytes = 0;
        uint64 resourceRegistryResidentEstimatedBytes = 0;
    };

    /**
     * @brief One ordered RenderGraph pass timing in a performance report.
     *
     * RenderGraph pass names are descriptive, not unique identifiers. The
     * execution index is therefore the stable identity within one graph
     * recording and permits multiple passes with the same name.
     */
    struct RenderQualityPerformanceReportPassTiming
    {
        uint32 executionIndex = 0;
        std::string name{};
        float64 durationMilliseconds = 0.0;
    };

    /** @brief Exact completion-owned metrics for one measured frame. */
    struct RenderQualityPerformanceReportSample
    {
        uint64 frameSequence = 0;
        uint64 submittedFrame = 0;
        uint64 completedFrame = 0;
        std::string renderExecutionReceiptSha256{};
        bool completionOwned = false;

        float64 extractionMilliseconds = 0.0;
        float64 visibilityMilliseconds = 0.0;
        float64 graphCompileMilliseconds = 0.0;
        float64 submissionMilliseconds = 0.0;

        /**
         * @brief GPU timestamp span from the Graphics prelude through the
         * terminal Graphics command for this qualified submission.
         *
         * This is intentionally not a whole-frame, CPU submit, or Present
         * measurement. It is retained as one input to the conservative
         * qualification span below.
         */
        float64 graphicsTerminalCommandSpanMilliseconds = 0.0;

        /**
         * @brief Observed RenderGraph pass envelope in the same completed
         * Graphics submission.
         *
         * A general RenderGraph DAG can contain branches outside the terminal
         * critical path, so this need not fit inside the terminal command
         * span. It neither includes CPU submit/Present time nor alone defines
         * the frame-performance threshold.
         */
        float64 renderGraphPassEnvelopeMilliseconds = 0.0;

        /**
         * @brief Conservative completion-owned GPU qualification span.
         *
         * This is exactly max(Graphics prelude-to-terminal command span,
         * RenderGraph pass envelope). The two raw spans are not nested for a
         * general RenderGraph DAG, so this conservative maximum is the only
         * GPU span constrained by the GFX24 16.67 ms policy. It remains
         * distinct from CPU submit, Present, and a multi-queue busy-time sum.
         */
        float64 graphicsQualificationSpanMilliseconds = 0.0;
        std::vector<RenderQualityPerformanceReportPassTiming> passGpuTimings{};
        uint64 trackedUploadBytes = 0;
        uint32 trackedUploadCoverageMask = 0;
        SampleRenderQualityPerformanceTrackedEstimatedFootprint
            trackedEstimatedFootprint{};
        uint32 resourceCountsSchemaVersion =
            RVX_RENDER_QUALITY_PERFORMANCE_TELEMETRY_COVERAGE_SCHEMA_VERSION;
        std::map<std::string, uint64> resourceCounts{};
    };

    /**
     * @brief All host metadata plus a frozen projection of one Render session.
     *
     * `baselineIdentity` is an opaque, explicit input.  It has no implied
     * approval status; the checker compares it to the separately approved
     * policy and baseline artifact.
     */
    struct RenderQualityPerformanceWriteRequest
    {
        std::filesystem::path outputPath{};
        std::string sourceCommit{};
        std::string runId{};
        std::string preset{};
        RenderQualityPerformanceReportIdentity qualificationIdentity{};
        SampleRenderQualityPerformanceHardwareIdentity hardwareIdentity{};
        RenderQualityPerformanceTelemetryCoverage telemetryCoverage{};
        std::string baselineIdentity{};
        uint32 completedWarmupFrames = 0;
        std::vector<RenderQualityPerformanceReportSample> samples{};
    };

    struct RenderQualityPerformanceWriteResult
    {
        std::filesystem::path reportPath{};
    };

    /**
     * @brief Atomically write an exact 60-warmup, 300-sample report.
     *
     * The output must be named `performance.json`; existing targets are never
     * overwritten.  On failure this function removes its temporary file and
     * leaves no successful report marker behind.
     */
    [[nodiscard]] bool WriteRenderQualityPerformanceReport(
        const RenderQualityPerformanceWriteRequest& request,
        RenderQualityPerformanceWriteResult& outResult,
        std::string* outError = nullptr);
} // namespace RVX
