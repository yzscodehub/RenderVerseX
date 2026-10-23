#pragma once

/**
 * @file RenderQualityQualificationWriter.h
 * @brief Fail-closed writer for completed RenderQualityLab attachment bundles.
 *
 * This is a host-side artifact boundary.  It never asks the renderer to
 * capture, poll, or wait: callers must supply an immutable completed bundle
 * and the exact presentation-qualified receipt that owns it.
 */

#include "Render/Features/RenderFeatureTypes.h"
#include "RenderContracts/QualificationCapture.h"

#include <filesystem>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_QUALITY_ATTACHMENT_BUNDLE_SCHEMA_VERSION =
        2;

    /**
     * @brief Host metadata not represented by the renderer-owned bundle.
     *
     * `pipeline`, `quality`, `backend`, source commit, preset, recipe, and
     * profile hash are derived from the supplied receipt. Render path and
     * instancing mode must be the actual settings retained by the Engine, not
     * the command-line request; the writer rejects omitted values.
     */
    struct RenderQualityQualificationWriteRequest
    {
        std::filesystem::path outputRoot{};
        std::string sampleId{};
        std::string cameraBookmarkId{};
        std::string runId{};
        std::string renderPath{};
        std::string instancingMode{};
        uint64 captureTick = 0;
        uint32 visualContractVersion = 0;
        /** Frame counters observed by the host after exact submission/completion. */
        uint64 submittedFrame = 0;
        uint64 completedFrame = 0;
    };

    /** @brief Runtime-owned configuration frozen across every series tick. */
    struct RenderQualityAttachmentSeriesConfigurationIdentity
    {
        std::string renderPath{};
        std::string instancingMode{};
        std::string requestedRecipe{};
        std::string resolvedRecipe{};
        uint64 profileHash = 0;
        std::string renderConfigurationHash{};

        friend bool operator==(
            const RenderQualityAttachmentSeriesConfigurationIdentity&,
            const RenderQualityAttachmentSeriesConfigurationIdentity&) = default;
    };

    /** @brief Immutable result of a successfully published artifact bundle. */
    struct RenderQualityQualificationWriteResult
    {
        std::filesystem::path bundlePath{};
        std::string bundleSha256{};
        std::string renderExecutionReceiptSha256{};
        RenderQualityAttachmentSeriesConfigurationIdentity
            renderConfigurationIdentity{};
    };

    inline constexpr uint32 RVX_RENDER_QUALITY_ATTACHMENT_SERIES_SCHEMA_VERSION =
        2;

    /** @brief Capture identity shared by every bundle in one series. */
    struct RenderQualityAttachmentSeriesIdentity
    {
        std::string sampleId{};
        std::string pipeline{};
        std::string quality{};
        std::string backend{};
        std::string view{};
        uint32 visualContractVersion = 0;
    };

    /** @brief Real host identity frozen before the first series capture. */
    struct RenderQualityAttachmentSeriesHardwareIdentity
    {
        std::string backend{};
        std::string adapterName{};
        std::string driverVersion{};
        std::string osBuild{};
        std::string capabilityHash{};
    };

    /** @brief One manifest-last bundle already completed by the Render owner. */
    struct RenderQualityAttachmentSeriesTick
    {
        uint64 captureTick = 0;
        uint64 bundleId = 0;
        /** POSIX path relative to the series root, always ending in bundle.json. */
        std::string bundlePath{};
        std::string bundleSha256{};
        std::string receiptSha256{};
        uint64 frameSequence = 0;
        uint64 submittedFrame = 0;
        uint64 completedFrame = 0;
    };

    /** @brief Immutable request to atomically publish an attachment series. */
    struct RenderQualityAttachmentSeriesWriteRequest
    {
        std::filesystem::path outputPath{};
        std::string sourceCommit{};
        std::string runId{};
        std::string preset{};
        RenderQualityAttachmentSeriesIdentity qualificationIdentity{};
        RenderQualityAttachmentSeriesHardwareIdentity hardwareIdentity{};
        RenderQualityAttachmentSeriesConfigurationIdentity
            renderConfigurationIdentity{};
        std::string manifestSha256{};
        std::vector<RenderQualityAttachmentSeriesTick> ticks{};
    };

    struct RenderQualityAttachmentSeriesWriteResult
    {
        std::filesystem::path seriesPath{};
        std::string seriesSha256{};
    };

    /**
     * @brief Write seven canonical raw surfaces plus `bundle.json`.
     *
     * Output data paths are always package-relative fixed names.  Every raw
     * file is atomically renamed before the manifest; the manifest is the
     * success marker and is atomically published last.  On failure this
     * function never leaves a successful manifest behind.
     */
    [[nodiscard]] bool WriteRenderQualityQualificationBundle(
        const RenderQualityQualificationWriteRequest& request,
        const QualificationCaptureBundle& bundle,
        const RenderExecutionReceipt& receipt,
        RenderQualityQualificationWriteResult& outResult,
        std::string* outError = nullptr);

    /**
     * @brief Atomically publish a same-process RenderQuality attachment series.
     *
     * `seriesSha256` is SHA-256 of the fixed length-prefixed UTF-8 binding
     * stream: `name:<byte-length>:<value>\\n`.  The field order is schema,
     * schemaVersion, sourceCommit, runId, preset; identity sampleId, pipeline,
     * quality, backend, view, visualContractVersion; configuration renderPath,
     * instancingMode, requestedRecipe, resolvedRecipe, profileHash,
     * renderConfigurationHash; hardware backend, adapterName, driverVersion,
     * osBuild, capabilityHash; manifestSha256;
     * then every tick in array order with captureTick, bundleId, bundlePath,
     * bundleSha256, receiptSha256, frameSequence,
     * submittedFrame, completedFrame. Numeric values are decimal without
     * leading zeroes. The writer verifies every referenced bundle hash before
     * publishing `series.json` last.
     */
    [[nodiscard]] bool WriteRenderQualityAttachmentSeries(
        const RenderQualityAttachmentSeriesWriteRequest& request,
        RenderQualityAttachmentSeriesWriteResult& outResult,
        std::string* outError = nullptr);
} // namespace RVX
