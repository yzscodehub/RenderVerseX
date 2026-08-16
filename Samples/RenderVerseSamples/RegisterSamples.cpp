/** @file RegisterSamples.cpp @brief Explicit built-in sample registration. */

#include "RegisterSamples.h"

#include "Scenes/AnimationCharacterSample.h"
#include "Scenes/AssetStreamingSample.h"
#include "Scenes/AssetGalleryShowcaseSample.h"
#include "Scenes/GPUDrivenShowcaseSample.h"
#include "Scenes/InteriorRenderingSample.h"
#include "Scenes/LightingShadowShowcaseSample.h"
#include "Scenes/ModelRenderingSample.h"
#include "Scenes/ModelViewerSample.h"
#include "Scenes/PBRMaterialsSample.h"
#include "Scenes/PhysicsSandboxSample.h"
#include "Scenes/RenderPipelineShowcaseSample.h"
#include "Scenes/RenderingStressSample.h"
#include "Scenes/SceneLifecycleSample.h"
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
        if (!registry.Register(
            renderPipeline.GetInfo(),
            []()
            {
                return std::make_unique<RenderPipelineShowcaseSample>();
            },
            outError))
        {
            return false;
        }

        SceneLifecycleSample sceneLifecycle;
        if (!registry.Register(
            sceneLifecycle.GetInfo(),
            []()
            {
                return std::make_unique<SceneLifecycleSample>();
            },
            outError))
        {
            return false;
        }

        AssetStreamingSample assetStreaming;
        if (!registry.Register(
            assetStreaming.GetInfo(),
            []()
            {
                return std::make_unique<AssetStreamingSample>();
            },
            outError))
        {
            return false;
        }

        InteriorRenderingSample interiorRendering;
        if (!registry.Register(
            interiorRendering.GetInfo(),
            []()
            {
                return std::make_unique<InteriorRenderingSample>();
            },
            outError))
        {
            return false;
        }

        PhysicsSandboxSample physicsSandbox;
        if (!registry.Register(
            physicsSandbox.GetInfo(),
            []()
            {
                return std::make_unique<PhysicsSandboxSample>();
            },
            outError))
        {
            return false;
        }

        AnimationCharacterSample animationCharacter;
        if (!registry.Register(
            animationCharacter.GetInfo(),
            []()
            {
                return std::make_unique<AnimationCharacterSample>();
            },
            outError))
        {
            return false;
        }

        RenderingStressSample renderingStress;
        return registry.Register(
            renderingStress.GetInfo(),
            []()
            {
                return std::make_unique<RenderingStressSample>();
            },
            outError);
    }
} // namespace RVX
