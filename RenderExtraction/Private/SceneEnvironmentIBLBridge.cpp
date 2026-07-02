/**
 * @file SceneEnvironmentIBLBridge.cpp
 * @brief SceneEnvironmentIBLBridge implementation.
 */

#include "RenderExtraction/SceneEnvironmentIBLBridge.h"

#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
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

        SceneManager* sceneManager = world->GetSceneManager();
        if (!sceneManager)
        {
            return fail(SceneEnvironmentIBLFallbackReason::NullSceneManager);
        }

        SkyboxComponent* skybox = nullptr;
        sceneManager->ForEachActiveEntity(
            [&skybox](SceneEntity* entity)
            {
                if (skybox || !entity)
                    return;

                auto* candidate = entity->GetComponent<SkyboxComponent>();
                if (candidate && candidate->IsEnabled() && candidate->ContributesToLighting())
                {
                    skybox = candidate;
                }
            });

        if (!skybox)
        {
            return fail(SceneEnvironmentIBLFallbackReason::NoLightingSkybox);
        }

        result.skyboxFound = true;
        outSnapshot.intensity = skybox->GetExposure();
        outSnapshot.irradiance = skybox->GetIrradianceMap().As<IRenderTextureUploadSource>();
        outSnapshot.prefiltered = skybox->GetPrefilteredMap().As<IRenderTextureUploadSource>();
        outSnapshot.brdfLUT = skybox->GetBRDFLUT().As<IRenderTextureUploadSource>();

        if (!outSnapshot.irradiance || !outSnapshot.prefiltered || !outSnapshot.brdfLUT)
        {
            return fail(SceneEnvironmentIBLFallbackReason::SkyboxIBLResourcesMissing);
        }

        outSnapshot.prefilteredMipLevels = std::max(1u, outSnapshot.prefiltered->GetRenderTextureMipLevels());
        result.textureIBLEnabled = true;
        result.fallbackReason = SceneEnvironmentIBLFallbackReason::None;
        if (outResult)
        {
            *outResult = result;
        }
        return true;
    }

} // namespace RVX
