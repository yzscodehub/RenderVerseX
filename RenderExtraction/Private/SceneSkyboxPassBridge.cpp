/**
 * @file SceneSkyboxPassBridge.cpp
 * @brief Scene-to-skybox-pass selection bridge implementation.
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
                return "NoWorld";
            case SceneSkyboxPassBridgeFallbackReason::NullSceneManager:
                return "NoSceneManager";
            case SceneSkyboxPassBridgeFallbackReason::NoSkyboxComponent:
                return "NoSkyboxComponent";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapMissing:
                return "SkyboxCubemapMissing";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapNotReady:
                return "SkyboxCubemapNotReady";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapTextureUnavailable:
                return "SkyboxCubemapTextureUnavailable";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxTextureResolverMissing:
                return "SkyboxTextureResolverMissing";
            case SceneSkyboxPassBridgeFallbackReason::SkyboxEquirectangularDrawingNotImplemented:
                return "SkyboxEquirectangularDrawingNotImplemented";
            default:
                return "Unknown";
        }
    }

    bool SceneSkyboxPassBridge::Update(World* world,
                                       const SceneSkyboxPassActions& passActions,
                                       const SceneSkyboxTextureAccess& textureAccess,
                                       SceneSkyboxPassBridgeResult* outResult) const
    {
        SceneSkyboxPassBridgeResult result;

        if (!world)
        {
            MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::NullWorld);
            if (outResult)
                *outResult = result;
            return false;
        }

        SceneManager* sceneManager = world->GetSceneManager();
        if (!sceneManager)
        {
            MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::NullSceneManager);
            if (outResult)
                *outResult = result;
            return false;
        }

        SkyboxComponent* skybox = nullptr;
        sceneManager->ForEachActiveEntity(
            [&skybox](SceneEntity* entity)
            {
                if (skybox || !entity)
                    return;

                auto* candidate = entity->GetComponent<SkyboxComponent>();
                if (candidate && candidate->IsEnabled())
                {
                    skybox = candidate;
                }
            });

        if (!skybox)
        {
            MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::NoSkyboxComponent);
            if (outResult)
                *outResult = result;
            return false;
        }

        result.skyboxFound = true;

        switch (skybox->GetSkyboxType())
        {
            case SkyboxType::Procedural:
                if (passActions.setProcedural)
                {
                    passActions.setProcedural(skybox->GetSunDirection(),
                                              skybox->GetZenithColor(),
                                              skybox->GetHorizonColor(),
                                              skybox->GetGroundColor(),
                                              skybox->GetSunColor(),
                                              skybox->GetExposure(),
                                              skybox->GetScatteringIntensity());
                }
                break;
            case SkyboxType::Color:
                if (passActions.setSolidColor)
                {
                    passActions.setSolidColor(skybox->GetSolidColor(), skybox->GetExposure());
                }
                break;
            case SkyboxType::Cubemap:
            {
                IRenderTextureUploadSource* cubemapResource =
                    skybox->GetCubemap().As<IRenderTextureUploadSource>();
                if (!cubemapResource)
                {
                    MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapMissing);
                    if (outResult)
                        *outResult = result;
                    return false;
                }

                if (!textureAccess.isGPUReady || !textureAccess.getTexture)
                {
                    MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::SkyboxTextureResolverMissing);
                    if (outResult)
                        *outResult = result;
                    return false;
                }

                const uint64 cubemapId = cubemapResource->GetRenderResourceId();
                if (!textureAccess.isGPUReady(cubemapId))
                {
                    if (textureAccess.requestUpload)
                    {
                        textureAccess.requestUpload(cubemapResource);
                        result.uploadRequested = true;
                    }
                    MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapNotReady);
                    if (outResult)
                        *outResult = result;
                    return false;
                }

                RHITexture* cubemapTexture = textureAccess.getTexture(cubemapId);
                if (!cubemapTexture)
                {
                    MarkFallback(result, passActions, SceneSkyboxPassBridgeFallbackReason::SkyboxCubemapTextureUnavailable);
                    if (outResult)
                        *outResult = result;
                    return false;
                }

                result.selectedCubemap = cubemapTexture;
                if (passActions.setCubemap)
                {
                    passActions.setCubemap(cubemapTexture,
                                           skybox->GetExposure(),
                                           skybox->GetRotation(),
                                           skybox->GetBlurLevel());
                }
                break;
            }
            case SkyboxType::Equirectangular:
            default:
                MarkFallback(result,
                             passActions,
                             SceneSkyboxPassBridgeFallbackReason::SkyboxEquirectangularDrawingNotImplemented);
                if (outResult)
                    *outResult = result;
                return false;
        }

        result.fallbackReason = SceneSkyboxPassBridgeFallbackReason::None;
        if (outResult)
        {
            *outResult = result;
        }
        return true;
    }

    void SceneSkyboxPassBridge::MarkFallback(SceneSkyboxPassBridgeResult& result,
                                             const SceneSkyboxPassActions& passActions,
                                             SceneSkyboxPassBridgeFallbackReason reason)
    {
        result.fallbackReason = reason;
        result.selectedCubemap = nullptr;
        if (passActions.clear)
        {
            passActions.clear(ToString(reason));
        }
    }

} // namespace RVX
