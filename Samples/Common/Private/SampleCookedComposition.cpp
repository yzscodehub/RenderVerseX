/** @file SampleCookedComposition.cpp @brief Cooked-only Sample product wiring. */

#include "Samples/SampleHostComposition.h"

#include "Engine/ShippingRuntimeComposition.h"

#include <memory>
#include <string>

namespace RVX
{
namespace
{
    class SampleCookedComposition final : public ISampleHostComposition
    {
    public:
        [[nodiscard]] bool RequiresCookedOnly() const noexcept override
        {
            return true;
        }

        bool CreateEngineRuntimeDependencies(
            const SampleRuntimeCompositionEvidence& evidence,
            EngineRuntimeDependencies& outDependencies,
            std::string& outError) const override
        {
            if (!evidence.HasCookedOnlyShaderProvider())
            {
                outError =
                    "Cooked Sample product requires an admitted cooked runtime composition.";
                return false;
            }
            outDependencies = CreateShippingEngineRuntimeDependencies(
                evidence.cookedShaderRoot);
            return true;
        }

        bool CreateResourceManagerConfig(
            const SampleRuntimeCompositionEvidence& evidence,
            Resource::ResourceManagerConfig& outConfig,
            std::string& outError) const override
        {
            if (!evidence.HasCookedOnlyShaderProvider())
            {
                outError =
                    "Cooked Sample product requires an admitted cooked resource root.";
                return false;
            }
            outConfig = CreateShippingResourceManagerConfig(
                evidence.cookedResourceRoot);
            return true;
        }

        bool PrepareSourceEnvironment(
            const SampleSourceEnvironmentPreparationRequest&,
            uint64& outCanonicalImportOptionsHash,
            Resource::ResourceLoadPreparationStateRef& outState,
            Resource::ResourceLoadError& outError) const override
        {
            outCanonicalImportOptionsHash = 0;
            outState.reset();
            outError = {Resource::ResourceLoadErrorCode::LoaderUnavailable,
                        "Cooked Sample product rejects source environment preparation."};
            return false;
        }
    };
} // namespace

std::unique_ptr<ISampleHostComposition> CreateSampleCookedComposition()
{
    return std::make_unique<SampleCookedComposition>();
}
} // namespace RVX
