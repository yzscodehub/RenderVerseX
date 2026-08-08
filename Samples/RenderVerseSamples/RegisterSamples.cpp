/** @file RegisterSamples.cpp @brief Explicit built-in sample registration. */

#include "RegisterSamples.h"

#include "Scenes/AssetGalleryShowcaseSample.h"
#include "Scenes/GPUDrivenShowcaseSample.h"
#include "Scenes/LightingShadowShowcaseSample.h"
#include "Scenes/ModelRenderingSample.h"
#include "Scenes/ModelViewerSample.h"
#include "Scenes/PBRMaterialsSample.h"
#include "Scenes/RenderPipelineShowcaseSample.h"
#include "Samples/SampleRegistry.h"

#include <memory>

namespace RVX
{
    bool RegisterRenderVerseSamples(SampleRegistry& registry,
                                    std::string* outError)
    {
        ModelRenderingSample sample;
        if (!registry.Register(
            sample.GetInfo(),
            []()
            {
                return std::make_unique<ModelRenderingSample>();
            },
            outError))
        {
            return false;
        }

        ModelViewerSample modelViewer;
        if (!registry.Register(
            modelViewer.GetInfo(),
            []()
            {
                return std::make_unique<ModelViewerSample>();
            },
            outError))
        {
            return false;
        }

        PBRMaterialsSample pbrMaterials;
        if (!registry.Register(
            pbrMaterials.GetInfo(),
            []()
            {
                return std::make_unique<PBRMaterialsSample>();
            },
            outError))
        {
            return false;
        }

        GPUDrivenShowcaseSample gpuDriven;
        if (!registry.Register(
            gpuDriven.GetInfo(),
            []()
            {
                return std::make_unique<GPUDrivenShowcaseSample>();
            },
            outError))
        {
            return false;
        }

        LightingShadowShowcaseSample lightingShadows;
        if (!registry.Register(
            lightingShadows.GetInfo(),
            []()
            {
                return std::make_unique<LightingShadowShowcaseSample>();
            },
            outError))
        {
            return false;
        }

        AssetGalleryShowcaseSample assetGallery;
        if (!registry.Register(
            assetGallery.GetInfo(),
            []()
            {
                return std::make_unique<AssetGalleryShowcaseSample>();
            },
            outError))
        {
            return false;
        }

        RenderPipelineShowcaseSample renderPipeline;
        return registry.Register(
            renderPipeline.GetInfo(),
            []()
            {
                return std::make_unique<RenderPipelineShowcaseSample>();
            },
            outError);
    }
} // namespace RVX
