/** @file RegisterCookedSamples.cpp @brief Cooked RenderQualityLab registration. */

#include "RegisterCookedSamples.h"

#include "Scenes/RenderQualityLabSample.h"
#include "Samples/SampleRegistry.h"

#include <memory>

namespace RVX
{
    bool RegisterRenderQualityLabCookedSample(
        const SampleManifestCatalog& manifest,
        SampleRegistry& registry,
        std::string* outError)
    {
        RenderQualityLabSample sample;
        const SampleInfo& info = sample.GetInfo();
        const SampleScenarioManifest* scenarioManifest = manifest.Find(info.id);
        if (scenarioManifest == nullptr)
        {
            if (outError != nullptr)
            {
                *outError =
                    "Cooked Sample manifest is missing render-quality-lab.";
            }
            return false;
        }
        return registry.Register(
                   info,
                   *scenarioManifest,
                   []()
                   {
                       return std::make_unique<RenderQualityLabSample>();
                   },
                   outError) &&
               registry.ValidateManifestSelection(manifest, outError);
    }
} // namespace RVX
