#pragma once

/**
 * @file SampleManifest.h
 * @brief Strict machine-readable contracts for product-facing sample scenes.
 */

#include "Core/Types.h"
#include "Samples/FrameworkAssessment.h"
#include "Samples/SampleInfo.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    inline constexpr std::string_view RVX_SAMPLE_MANIFEST_SCHEMA =
        "RVX.SampleManifest";
    inline constexpr uint32 RVX_SAMPLE_MANIFEST_SCHEMA_VERSION = 1;

    /**
     * @brief Exact fixed simulation step and deterministic random seed.
     *
     * The step is expressed as numerator / denominator seconds. The parser
     * canonicalizes the fraction to its lowest terms; floating-point values
     * are never the manifest authority for time.
     */
    struct SampleFixedTime
    {
        uint32 numerator = 1;
        uint32 denominator = 60;
        uint32 deterministicSeed = 0;

        friend bool operator==(const SampleFixedTime&,
                               const SampleFixedTime&) = default;
    };

    /** @brief Value-only deterministic camera state used by qualification. */
    struct SampleCameraBookmark
    {
        std::string id;
        std::array<float32, 3> position{};
        std::array<float32, 3> target{};
        float32 verticalFieldOfViewDegrees = 60.0f;
        float32 nearPlane = 0.1f;
        float32 farPlane = 1000.0f;
        float32 exposure = 1.0f;

        friend bool operator==(const SampleCameraBookmark&,
                               const SampleCameraBookmark&) = default;
    };

    /** @brief Normalized image-space region used by visual metrics. */
    struct SampleVisualRegionOfInterest
    {
        std::string id;
        float32 x = 0.0f;
        float32 y = 0.0f;
        float32 width = 1.0f;
        float32 height = 1.0f;

        friend bool operator==(const SampleVisualRegionOfInterest&,
                               const SampleVisualRegionOfInterest&) = default;
    };

    /**
     * @brief One metric bound applied to a named region of interest.
     *
     * At least one bound must be present. Both bounds present describe an
     * inclusive range. Metric codes use the same stable AssessmentCode syntax
     * as the rest of the Sample qualification system.
     */
    struct SampleVisualMetricPolicy
    {
        AssessmentCode code;
        std::string regionOfInterestId;
        std::optional<float64> minimum;
        std::optional<float64> maximum;

        friend bool operator==(const SampleVisualMetricPolicy&,
                               const SampleVisualMetricPolicy&) = default;
    };

    /** @brief Complete ROI and metric policy for one visual contract. */
    struct SampleROIMetricPolicy
    {
        std::vector<SampleVisualRegionOfInterest> regionsOfInterest;
        std::vector<SampleVisualMetricPolicy> metrics;

        friend bool operator==(const SampleROIMetricPolicy&,
                               const SampleROIMetricPolicy&) = default;
    };

    /** @brief Immutable contract for one registered Sample scenario. */
    struct SampleScenarioManifest
    {
        std::string id;
        SampleDomain domain = SampleDomain::Rendering;
        SampleIntent intent = SampleIntent::Demonstration;
        SamplePipelineRecipe defaultRecipe = SamplePipelineRecipe::Minimal;
        SampleQualityTier defaultQualityProfile = SampleQualityTier::Default;
        std::vector<AssessmentCode> requiredCapabilities;
        std::vector<AssessmentCode> optionalCapabilities;
        std::vector<AssessmentCode> allowedFallbacks;
        std::vector<std::string> cookedAssetIds;
        SampleFixedTime fixedTime;
        std::vector<SampleCameraBookmark> cameraBookmarks;
        std::vector<uint32> captureTicks;
        uint32 visualContractVersion = 0;
        SampleROIMetricPolicy roiMetricPolicy;
        bool deterministicQualification = false;

        friend bool operator==(const SampleScenarioManifest&,
                               const SampleScenarioManifest&) = default;
    };

    /** @brief One parsed v1 document and its exact source-content identity. */
    struct SampleManifestCatalog
    {
        std::string schema = std::string(RVX_SAMPLE_MANIFEST_SCHEMA);
        uint32 schemaVersion = RVX_SAMPLE_MANIFEST_SCHEMA_VERSION;
        std::string contentSha256;
        std::map<std::string, SampleScenarioManifest, std::less<>> samples;

        [[nodiscard]] const SampleScenarioManifest* Find(
            std::string_view id) const noexcept;
        [[nodiscard]] bool IsEmpty() const noexcept { return samples.empty(); }

        friend bool operator==(const SampleManifestCatalog&,
                               const SampleManifestCatalog&) = default;
    };

    /** @brief Strict, transactional parser for RVX.SampleManifest v1. */
    class SampleManifestParser final
    {
    public:
        static constexpr size_t MaximumDocumentBytes = 1024u * 1024u;

        [[nodiscard]] static bool Parse(
            std::string_view jsonText,
            SampleManifestCatalog& outCatalog,
            std::string* outError = nullptr);

        [[nodiscard]] static bool LoadFile(
            const std::filesystem::path& path,
            SampleManifestCatalog& outCatalog,
            std::string* outError = nullptr);
    };
} // namespace RVX
