/** @file RegisterCookedSamples.cpp @brief Explicit cooked Sample registration. */

#include "RegisterCookedSamples.h"

#include "Scenes/ModelViewerSample.h"
#include "Scenes/PBRMaterialsSample.h"
#include "Scenes/RenderQualityLabSample.h"
#include "Samples/SampleRegistry.h"

#include <memory>

namespace RVX
{
    namespace
    {
        template <typename SampleType>
        void ConfigureCookedSampleInfo(SampleInfo&)
        {
        }

        template <>
        void ConfigureCookedSampleInfo<ModelViewerSample>(SampleInfo& info)
        {
            info.defaultAssetId = "water-bottle-cooked-v1";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.defaultEnvironmentId = "pbr-reference-environment-cooked-v1";
            info.requiredCookedAssetIds = {
                info.defaultAssetId, info.defaultEnvironmentId};
        }

        template <>
        void ConfigureCookedSampleInfo<PBRMaterialsSample>(SampleInfo& info)
        {
            info.defaultAssetId = "pbr-material-grid-cooked-v1";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.defaultEnvironmentId = "pbr-reference-environment-cooked-v1";
            info.requiredCookedAssetIds = {
                info.defaultAssetId,
                info.defaultEnvironmentId};
        }

        template <typename SampleType>
        bool RegisterCookedSample(const SampleManifestCatalog& manifest,
                                  SampleRegistry& registry,
                                  std::string* outError)
        {
            SampleType sample;
            SampleInfo info = sample.GetInfo();
            ConfigureCookedSampleInfo<SampleType>(info);
            const SampleScenarioManifest* scenarioManifest = manifest.Find(info.id);
            if (scenarioManifest == nullptr)
            {
                if (outError != nullptr)
                {
                    *outError = "Cooked Sample manifest is missing registered scenario: " +
                        info.id;
                }
                return false;
            }
            return registry.Register(
                info,
                *scenarioManifest,
                []()
                {
                    return std::make_unique<SampleType>();
                },
                outError);
        }
    } // namespace

    bool RegisterCookedSampleScenarios(const SampleManifestCatalog& manifest,
                                       SampleRegistry& registry,
                                       std::string* outError)
    {
        return RegisterCookedSample<RenderQualityLabSample>(
                   manifest, registry, outError) &&
               RegisterCookedSample<ModelViewerSample>(
                   manifest, registry, outError) &&
               RegisterCookedSample<PBRMaterialsSample>(
                   manifest, registry, outError) &&
               registry.ValidateManifestSelection(manifest, outError);
    }
} // namespace RVX
