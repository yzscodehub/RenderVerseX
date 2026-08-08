/**
 * @file SceneEnvironmentIBLBridge.cpp
 * @brief SceneEnvironmentIBLBridge implementation.
 */

#include "RenderExtraction/SceneEnvironmentIBLBridge.h"

#include "Resource/Types/TextureResource.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "World/World.h"

#include <algorithm>

namespace RVX
{
    const char* ToString(SceneEnvironmentIBLFallbackReason reason)
    {
        switch (reason)
        {
            case SceneEnvironmentIBLFallbackReason::None:
                return "None";
            case SceneEnvironmentIBLFallbackReason::NullWorld:
                return "NoWorld";
            case SceneEnvironmentIBLFallbackReason::NullSceneManager:
                return "NoSceneManager";
            case SceneEnvironmentIBLFallbackReason::NoLightingSkybox:
                return "NoLightingSkybox";
            case SceneEnvironmentIBLFallbackReason::SkyboxIBLResourcesMissing:
                return "SkyboxIBLResourcesMissing";
            default:
                return "Unknown";
        }
    }

    bool SceneEnvironmentIBLBridge::Extract(World* world,
                                            SceneEnvironmentIBLSnapshot& outSnapshot,
                                            SceneEnvironmentIBLBridgeResult* outResult) const
    {
        SceneEnvironmentIBLBridgeResult result;
        outSnapshot = SceneEnvironmentIBLSnapshot();

        auto fail = [&](SceneEnvironmentIBLFallbackReason reason) {
            result.fallbackReason = reason;
            if (outResult)
            {
                *outResult = result;
            }
            return false;
        };

        if (!world)
        {
            return fail(SceneEnvironmentIBLFallbackReason::NullWorld);
        }

        Scene* scene = world->GetScene();
        if (!scene)
        {
            return fail(SceneEnvironmentIBLFallbackReason::NullSceneManager);
        }

        SkyboxComponent* skybox = nullptr;
        for (SkyboxComponent* candidate :
             scene->GetComponentsImplementing<SkyboxComponent>())
        {
            if (candidate && candidate->IsEnabled() &&
                candidate->ContributesToLighting() &&
                candidate->GetOwner() && candidate->GetOwner()->IsActive())
            {
                skybox = candidate;
                break;
            }
        }

        if (!skybox)
        {
            return fail(SceneEnvironmentIBLFallbackReason::NoLightingSkybox);
        }

        result.skyboxFound = true;
        outSnapshot.intensity = skybox->GetExposure();
        const SceneTextureHandle irradiance = skybox->GetIrradianceMap();
        const SceneTextureHandle prefiltered = skybox->GetPrefilteredMap();
        const SceneTextureHandle brdfLut = skybox->GetBRDFLUT();
        outSnapshot.irradianceAssetId = AssetId{irradiance.GetId()};
        outSnapshot.prefilteredAssetId = AssetId{prefiltered.GetId()};
        outSnapshot.brdfLutAssetId = AssetId{brdfLut.GetId()};

        if (!outSnapshot.irradianceAssetId.IsValid() ||
            !outSnapshot.prefilteredAssetId.IsValid() ||
            !outSnapshot.brdfLutAssetId.IsValid())
        {
            return fail(SceneEnvironmentIBLFallbackReason::SkyboxIBLResourcesMissing);
        }

        const auto* prefilteredResource =
            prefiltered.As<Resource::TextureResource>();
        outSnapshot.prefilteredMipLevels =
            std::max(1u,
                     prefilteredResource != nullptr
                         ? prefilteredResource->GetMipLevels()
                         : 1u);
        result.textureIBLEnabled = true;
        result.fallbackReason = SceneEnvironmentIBLFallbackReason::None;
        if (outResult)
        {
            *outResult = result;
        }
        return true;
    }

} // namespace RVX
