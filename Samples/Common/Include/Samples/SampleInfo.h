#pragma once

/**
 * @file SampleInfo.h
 * @brief Backend-neutral metadata for one RenderVerseX sample scene.
 */

#include "Core/Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace RVX
{
    /** @brief Stable deterministic scene workload scales exposed by samples. */
    enum class SampleWorkloadScale : uint8
    {
        PullRequest = 0,
        Nightly,
        Qualification
    };

    /**
     * @brief Value-only scene workload identity used for reproducible stress
     *        scenarios and assessment fingerprints.
     */
    struct SampleWorkloadProfile
    {
        SampleWorkloadScale scale = SampleWorkloadScale::PullRequest;
        uint32 objectCount = 1000;
        uint32 dirtyObjectCount = 10;
        uint32 churnObjectCount = 100;
        uint32 gpuCullingCapacity = 2048;
    };

    /** @brief Return the stable command-line spelling for a workload scale. */
    inline constexpr const char* GetSampleWorkloadScaleName(
        SampleWorkloadScale scale) noexcept
    {
        switch (scale)
        {
            case SampleWorkloadScale::PullRequest: return "pr";
            case SampleWorkloadScale::Nightly: return "nightly";
            case SampleWorkloadScale::Qualification: return "qualification";
            default: return "invalid";
        }
    }

    /** @brief Parse one stable command-line workload scale spelling. */
    inline constexpr bool ParseSampleWorkloadScale(
        std::string_view text,
        SampleWorkloadScale& output) noexcept
    {
        if (text == "pr")
        {
            output = SampleWorkloadScale::PullRequest;
            return true;
        }
        if (text == "nightly")
        {
            output = SampleWorkloadScale::Nightly;
            return true;
        }
        if (text == "qualification")
        {
            output = SampleWorkloadScale::Qualification;
            return true;
        }
        return false;
    }

    /** @brief Build the exact deterministic profile for one workload scale. */
    inline constexpr SampleWorkloadProfile BuildSampleWorkloadProfile(
        SampleWorkloadScale scale) noexcept
    {
        switch (scale)
        {
            case SampleWorkloadScale::Nightly:
                return {scale, 10000, 100, 1000, 16384};
            case SampleWorkloadScale::Qualification:
                return {scale, 100000, 1000, 10000, 131072};
            case SampleWorkloadScale::PullRequest:
            default:
                return {SampleWorkloadScale::PullRequest, 1000, 10, 100, 2048};
        }
    }

    /** @brief Controls whether command-line model overrides are accepted. */
    enum class SampleAssetPolicy
    {
        Fixed = 0,
        CompatibleOverride,
        UserModelOrDefault
    };

    /** @brief Declares whether a sample accepts or requires an environment. */
    enum class SampleEnvironmentPolicy
    {
        None = 0,
        Optional,
        Required
    };

    /** @brief Backend-neutral rendering strategy exposed by selectable samples. */
    enum class SampleRenderPath
    {
        Auto = 0,
        Direct,
        GPUDriven
    };

    inline const char* GetSampleRenderPathName(SampleRenderPath path)
    {
        switch (path)
        {
            case SampleRenderPath::Auto: return "auto";
            case SampleRenderPath::Direct: return "direct";
            case SampleRenderPath::GPUDriven: return "gpu-driven";
            default: return "invalid";
        }
    }

    /** @brief Static metadata used by the sample registry and asset resolver. */
    struct SampleInfo
    {
        std::string id;
        std::string displayName;
        std::string description;
        std::string defaultAssetId;
        SampleAssetPolicy assetPolicy = SampleAssetPolicy::Fixed;
        std::string defaultEnvironmentId;
        SampleEnvironmentPolicy environmentPolicy =
            SampleEnvironmentPolicy::None;
        bool supportsRenderPathSelection = false;
        SampleRenderPath defaultRenderPath = SampleRenderPath::Auto;
        std::vector<std::string> additionalDefaultAssetIds;
        bool supportsWorkloadScaleSelection = false;
        SampleWorkloadScale defaultWorkloadScale =
            SampleWorkloadScale::PullRequest;
        /** Catalog animation assets required in addition to the primary model. */
        std::vector<std::string> requiredAnimationAssetIds;
        /** Catalog assets whose CookManifest admission must pass before setup. */
        std::vector<std::string> requiredCookedAssetIds;
    };
} // namespace RVX
