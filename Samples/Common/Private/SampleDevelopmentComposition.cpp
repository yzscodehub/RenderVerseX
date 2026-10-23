/** @file SampleDevelopmentComposition.cpp @brief Development Sample wiring. */

#include "Samples/SampleHostComposition.h"

#include "AssetImport/EnvironmentLoader.h"
#include "Engine/ShippingRuntimeComposition.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"

#include <memory>
#include <string>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] bool BuildEnvironmentOptions(
        const SampleSourceEnvironmentPreparationRequest& request,
        Resource::EnvironmentLoadOptions& outOptions,
        std::string& outError)
    {
        if (request.quality == "default")
        {
            outOptions.quality = Resource::HDRIBLQualityProfile::Default;
        }
        else if (request.quality == "low")
        {
            outOptions.quality = Resource::HDRIBLQualityProfile::Low;
        }
        else if (request.quality == "high")
        {
            outOptions.quality = Resource::HDRIBLQualityProfile::High;
        }
        else
        {
            outError = "Environment quality must be one of: default, low, high.";
            return false;
        }

        if (request.smoke)
        {
            outOptions.quality = Resource::HDRIBLQualityProfile::Validation;
        }
        outOptions.exposure = request.exposure;
        outOptions.applyGamma = false;
        return true;
    }

    class SampleDevelopmentComposition final : public ISampleHostComposition
    {
    public:
        [[nodiscard]] bool RequiresCookedOnly() const noexcept override
        {
            return false;
        }

        bool CreateEngineRuntimeDependencies(
            const SampleRuntimeCompositionEvidence& evidence,
            EngineRuntimeDependencies& outDependencies,
            std::string& outError) const override
        {
            if (evidence.runtimeOnlyComposition)
            {
                if (!evidence.HasCookedOnlyShaderProvider())
                {
                    outError =
                        "Runtime-only Sample composition requires verified cooked roots.";
                    return false;
                }
                outDependencies = CreateShippingEngineRuntimeDependencies(
                    evidence.cookedShaderRoot);
                return true;
            }

            outDependencies = CreateDevelopmentEngineRuntimeDependencies();
            return true;
        }

        bool CreateResourceManagerConfig(
            const SampleRuntimeCompositionEvidence& evidence,
            Resource::ResourceManagerConfig& outConfig,
            std::string& outError) const override
        {
            if (evidence.runtimeOnlyComposition)
            {
                if (!evidence.HasCookedOnlyShaderProvider())
                {
                    outError =
                        "Runtime-only Sample composition requires verified cooked roots.";
                    return false;
                }
                outConfig = CreateShippingResourceManagerConfig(
                    evidence.cookedResourceRoot);
                return true;
            }

            outConfig = {};
            return true;
        }

        bool PrepareSourceEnvironment(
            const SampleSourceEnvironmentPreparationRequest& request,
            uint64& outCanonicalImportOptionsHash,
            Resource::ResourceLoadPreparationStateRef& outState,
            Resource::ResourceLoadError& outError) const override
        {
            Resource::EnvironmentLoadOptions options;
            std::string optionError;
            if (!BuildEnvironmentOptions(request, options, optionError))
            {
                outCanonicalImportOptionsHash = 0;
                outState.reset();
                outError = {Resource::ResourceLoadErrorCode::InvalidRequest,
                            std::move(optionError)};
                return false;
            }

            outState = Resource::EnvironmentLoader::CreatePreparationState(
                options, outCanonicalImportOptionsHash, outError);
            return outState != nullptr;
        }
    };
} // namespace

std::unique_ptr<ISampleHostComposition> CreateSampleDevelopmentComposition()
{
    return std::make_unique<SampleDevelopmentComposition>();
}
} // namespace RVX
