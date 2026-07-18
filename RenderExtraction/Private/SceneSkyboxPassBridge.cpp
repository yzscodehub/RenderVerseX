/**
 * @file SceneSkyboxPassBridge.cpp
 * @brief SceneSkyboxPassBridge value extraction implementation.
 */

#include "RenderExtraction/SceneSkyboxPassBridge.h"

#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

namespace RVX
{
    const char* ToString(SceneSkyboxPassBridgeFallbackReason reason)
    {
        switch (reason)
        {
            case SceneSkyboxPassBridgeFallbackReason::None:
                return "None";
            case SceneSkyboxPassBridgeFallbackReason::NullWorld:
                return "NullWorld";
            case SceneSkyboxPassBridgeFallbackReason::NullSceneManager:
                return "NullSceneManager";
            case SceneSkyboxPassBridgeFallbackReason::NoSkyboxComponent:
                return "NoSkyboxComponent";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxTextureMissing:
                return "SkyboxTextureMissing";
        }
        return "Unknown";
    }

    bool SceneSkyboxPassBridge::Extract(
        World* world,
        SceneSkyboxSnapshot& outSnapshot,
        SceneSkyboxPassBridgeResult* outResult) const
    {
        SceneSkyboxPassBridgeResult result;
        outSnapshot = {};
        auto fail = [&](SceneSkyboxPassBridgeFallbackReason reason)
        {
            result.fallbackReason = reason;
            if (outResult != nullptr)
            {
                *outResult = result;
            }
            return false;
        };

        if (world == nullptr)
        {
            return fail(SceneSkyboxPassBridgeFallbackReason::NullWorld);
        }
        SceneManager* sceneManager = world->GetSceneManager();
        if (sceneManager == nullptr)
        {
            return fail(
                SceneSkyboxPassBridgeFallbackReason::NullSceneManager);
        }

        SkyboxComponent* skybox = nullptr;
        sceneManager->ForEachActiveEntity(
            [&skybox](SceneEntity* entity)
            {
                if (skybox != nullptr || entity == nullptr)
                {
                    return;
                }
                auto* candidate = entity->GetComponent<SkyboxComponent>();
                if (candidate != nullptr && candidate->IsEnabled())
                {
                    skybox = candidate;
                }
            });
        if (skybox == nullptr)
        {
            return fail(
                SceneSkyboxPassBridgeFallbackReason::NoSkyboxComponent);
        }

        result.skyboxFound = true;
        outSnapshot.intensity = skybox->GetExposure();
        outSnapshot.rotationRadians = skybox->GetRotation();
        outSnapshot.blurLevel = skybox->GetBlurLevel();
        outSnapshot.tint = skybox->GetSolidColor();
        outSnapshot.sunDirection = skybox->GetSunDirection();
        outSnapshot.sunColor = skybox->GetSunColor();
        outSnapshot.zenithColor = skybox->GetZenithColor();
        outSnapshot.horizonColor = skybox->GetHorizonColor();
        outSnapshot.groundColor = skybox->GetGroundColor();
        outSnapshot.scatteringIntensity = skybox->GetScatteringIntensity();

        switch (skybox->GetSkyboxType())
        {
            case SkyboxType::Cubemap:
                outSnapshot.mode = SceneSkyboxSnapshotMode::Cubemap;
                outSnapshot.textureAssetId =
                    AssetId{skybox->GetCubemap().GetId()};
                break;
            case SkyboxType::Equirectangular:
                outSnapshot.mode = SceneSkyboxSnapshotMode::Equirectangular;
                outSnapshot.textureAssetId =
                    AssetId{skybox->GetEquirectangular().GetId()};
                break;
            case SkyboxType::Procedural:
                outSnapshot.mode = SceneSkyboxSnapshotMode::Procedural;
                outSnapshot.tint = skybox->GetZenithColor();
                break;
            case SkyboxType::Color:
                outSnapshot.mode = SceneSkyboxSnapshotMode::SolidColor;
                break;
        }

        if ((outSnapshot.mode == SceneSkyboxSnapshotMode::Cubemap ||
             outSnapshot.mode == SceneSkyboxSnapshotMode::Equirectangular) &&
            !outSnapshot.textureAssetId.IsValid())
        {
            outSnapshot = {};
            return fail(
                SceneSkyboxPassBridgeFallbackReason::SkyboxTextureMissing);
        }

        result.fallbackReason = SceneSkyboxPassBridgeFallbackReason::None;
        if (outResult != nullptr)
        {
            *outResult = result;
        }
        return true;
    }
} // namespace RVX
