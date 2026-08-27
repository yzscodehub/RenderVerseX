/** @file RegisterSamples.cpp @brief Explicit built-in sample registration. */

#include "RegisterSamples.h"

#include "Scenes/AnimationCharacterSample.h"
#include "Scenes/AutoExposureSample.h"
#include "Scenes/AssetStreamingSample.h"
#include "Scenes/AssetGalleryShowcaseSample.h"
#include "Scenes/GPUDrivenShowcaseSample.h"
#include "Scenes/InteriorRenderingSample.h"
#include "Scenes/LightingShadowShowcaseSample.h"
#include "Scenes/ModelRenderingSample.h"
#include "Scenes/ModelViewerSample.h"
#include "Scenes/PBRMaterialsSample.h"
#include "Scenes/PhysicsSandboxSample.h"
#include "Scenes/PhysicalExposureSample.h"
#include "Scenes/ParticleVfxSamples.h"
#include "Scenes/PostProcessingShowcaseSample.h"
#include "Scenes/RenderPipelineShowcaseSample.h"
#include "Scenes/RenderingStressSample.h"
#include "Scenes/SceneLifecycleSample.h"
#include "Scenes/StylizedMaterialsSample.h"
#include "Scenes/TemporalAASample.h"
#include "Scenes/TransparentMaterialsSample.h"
#include "Samples/SampleRegistry.h"

#include <memory>
#include <string>

namespace RVX
{
    namespace
    {
        template <typename SampleType>
        bool RegisterBuiltInSample(const SampleManifestCatalog& catalog,
                                   SampleRegistry& registry,
                                   std::string* outError)
        {
            SampleType sample;
            const SampleInfo& info = sample.GetInfo();
            const SampleScenarioManifest* manifest = catalog.Find(info.id);
            if (manifest == nullptr)
            {
                if (outError)
                {
                    *outError = "Sample manifest is missing registered scenario: " +
                                info.id;
                }
                return false;
            }
            return registry.Register(
                info,
                *manifest,
                []()
                {
                    return std::make_unique<SampleType>();
                },
                outError);
        }
    } // namespace

    bool RegisterRenderVerseSamples(const SampleManifestCatalog& manifest,
                                    SampleRegistry& registry,
                                    std::string* outError)
    {
        return RegisterBuiltInSample<ModelRenderingSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<PhysicalExposureSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<AutoExposureSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<ModelViewerSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<PBRMaterialsSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<GPUDrivenShowcaseSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<LightingShadowShowcaseSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<AssetGalleryShowcaseSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<PostProcessingShowcaseSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<RenderPipelineShowcaseSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<SceneLifecycleSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<AssetStreamingSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<InteriorRenderingSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<PhysicsSandboxSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<AnimationCharacterSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<TemporalAASample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<StylizedMaterialsSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<TransparentMaterialsSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<ParticleFireSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<ParticleSmokeSample>(
                   manifest, registry, outError) &&
               RegisterBuiltInSample<RenderingStressSample>(
                   manifest, registry, outError) &&
               registry.ValidateManifestCoverage(manifest, outError);
    }
} // namespace RVX
