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

namespace RVX
{
    inline constexpr uint32 RVX_RENDER_QUALITY_ATTACHMENT_BUNDLE_SCHEMA_VERSION =
        1;

    /**
     * @brief Host metadata not represented by the renderer-owned bundle.
     *
     * `pipeline`, `quality`, `backend`, source commit, and preset are derived
     * from the supplied receipt.  The remaining values originate in the
     * scenario manifest or validation runner and are rejected when absent.
     */
    struct RenderQualityQualificationWriteRequest
    {
        std::filesystem::path outputRoot{};
        std::string sampleId{};
        std::string cameraBookmarkId{};
        std::string runId{};
        uint64 captureTick = 0;
        uint32 visualContractVersion = 0;
        /** Frame counters observed by the host after exact submission/completion. */
        uint64 submittedFrame = 0;
        uint64 completedFrame = 0;
    };

    /** @brief Immutable result of a successfully published artifact bundle. */
    struct RenderQualityQualificationWriteResult
    {
        std::filesystem::path bundlePath{};
        std::string renderExecutionReceiptSha256{};
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
} // namespace RVX
