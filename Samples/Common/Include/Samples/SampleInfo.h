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
    /** @brief Product-facing domain used to organize sample scenarios. */
    enum class SampleDomain : uint8
    {
        Rendering = 0,
        Animation,
        VFX,
        Environment,
        Physics,
        Systems,
        Qualification
    };

    /** @brief Observable purpose of a sample scenario. */
    enum class SampleIntent : uint8
    {
        Demonstration = 0,
        Regression,
        Stress,
        Integration
    };

    /** @brief Renderer composition recipe selected by a sample contract. */
    enum class SamplePipelineRecipe : uint8
    {
        Minimal = 0,
        ForwardStandard,
        DeferredQuality,
        HybridRayTracing,
        Headless
    };

    /** @brief Stable quality tiers shared by manifests and command-line input. */
    enum class SampleQualityTier : uint8
    {
        Default = 0,
        Low,
        Medium,
        High,
        Ultra
    };

    /** @brief Return the stable manifest spelling for a sample domain. */
    inline constexpr const char* GetSampleDomainName(
        SampleDomain domain) noexcept
    {
        switch (domain)
        {
            case SampleDomain::Rendering: return "rendering";
            case SampleDomain::Animation: return "animation";
            case SampleDomain::VFX: return "vfx";
            case SampleDomain::Environment: return "environment";
            case SampleDomain::Physics: return "physics";
            case SampleDomain::Systems: return "systems";
            case SampleDomain::Qualification: return "qualification";
            default: return "invalid";
        }
    }

    /** @brief Return the stable manifest spelling for a sample intent. */
    inline constexpr const char* GetSampleIntentName(
        SampleIntent intent) noexcept
    {
        switch (intent)
        {
            case SampleIntent::Demonstration: return "demonstration";
            case SampleIntent::Regression: return "regression";
            case SampleIntent::Stress: return "stress";
            case SampleIntent::Integration: return "integration";
            default: return "invalid";
        }
    }

    /** @brief Return the stable command-line spelling for a pipeline recipe. */
    inline constexpr const char* GetSamplePipelineRecipeName(
        SamplePipelineRecipe recipe) noexcept
    {
        switch (recipe)
        {
            case SamplePipelineRecipe::Minimal: return "minimal";
            case SamplePipelineRecipe::ForwardStandard: return "forward-plus";
            case SamplePipelineRecipe::DeferredQuality: return "deferred";
            case SamplePipelineRecipe::HybridRayTracing: return "hybrid-rt";
            case SamplePipelineRecipe::Headless: return "headless";
            default: return "invalid";
        }
    }

    /** @brief Parse one stable command-line pipeline recipe spelling. */
    inline constexpr bool ParseSamplePipelineRecipe(
        std::string_view text,
        SamplePipelineRecipe& output) noexcept
    {
        if (text == "minimal")
        {
            output = SamplePipelineRecipe::Minimal;
            return true;
        }
        if (text == "forward-plus")
        {
            output = SamplePipelineRecipe::ForwardStandard;
            return true;
        }
        if (text == "deferred")
        {
            output = SamplePipelineRecipe::DeferredQuality;
            return true;
        }
        if (text == "hybrid-rt")
        {
            output = SamplePipelineRecipe::HybridRayTracing;
            return true;
        }
        if (text == "headless")
        {
            output = SamplePipelineRecipe::Headless;
            return true;
        }
        return false;
    }

    /** @brief Return the stable command-line spelling for a quality tier. */
    inline constexpr const char* GetSampleQualityTierName(
        SampleQualityTier tier) noexcept
    {
        switch (tier)
        {
            case SampleQualityTier::Default: return "default";
            case SampleQualityTier::Low: return "low";
            case SampleQualityTier::Medium: return "medium";
            case SampleQualityTier::High: return "high";
            case SampleQualityTier::Ultra: return "ultra";
            default: return "invalid";
        }
    }

    /** @brief Parse one stable command-line quality tier spelling. */
    inline constexpr bool ParseSampleQualityTier(
        std::string_view text,
        SampleQualityTier& output) noexcept
    {
        if (text == "default")
        {
            output = SampleQualityTier::Default;
            return true;
        }
        if (text == "low")
        {
            output = SampleQualityTier::Low;
            return true;
        }
        if (text == "medium")
        {
            output = SampleQualityTier::Medium;
            return true;
        }
        if (text == "high")
        {
            output = SampleQualityTier::High;
            return true;
        }
        if (text == "ultra")
        {
            output = SampleQualityTier::Ultra;
            return true;
        }
        return false;
    }

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
        /** Product-facing classification; the machine manifest is authoritative. */
        SampleDomain domain = SampleDomain::Systems;
        /** Observable scenario intent; the machine manifest is authoritative. */
        SampleIntent intent = SampleIntent::Demonstration;
        /** Stable sidecar contract id. Empty is rejected for product samples. */
        std::string visualContractId;
        /** Whether captures must use the deterministic qualification timeline. */
        bool deterministicQualification = false;
    };
} // namespace RVX
