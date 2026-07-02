#pragma once

/**
 * @file SceneSkyboxPassBridge.h
 * @brief Scene-to-skybox-pass selection bridge.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/TextureResource.h"

#include <functional>

namespace RVX
{
    class RHITexture;
    class World;

    enum class SceneSkyboxPassBridgeFallbackReason : uint8
    {
        None = 0,
        NullWorld,
        NullSceneManager,
        NoSkyboxComponent,
        SkyboxCubemapMissing,
        SkyboxCubemapNotReady,
        SkyboxCubemapTextureUnavailable,
        SkyboxTextureResolverMissing,
        SkyboxEquirectangularDrawingNotImplemented
    };

    const char* ToString(SceneSkyboxPassBridgeFallbackReason reason);

    struct SceneSkyboxPassBridgeResult
    {
        bool skyboxFound = false;
        bool uploadRequested = false;
        RHITexture* selectedCubemap = nullptr;
        SceneSkyboxPassBridgeFallbackReason fallbackReason = SceneSkyboxPassBridgeFallbackReason::None;
    };

    struct SceneSkyboxTextureAccess
    {
        std::function<void(Resource::TextureResource*)> requestUpload;
        std::function<bool(Resource::ResourceId)> isGPUReady;
        std::function<RHITexture*(Resource::ResourceId)> getTexture;
    };

    struct SceneSkyboxPassActions
    {
        std::function<void(const Vec3& sunDirection,
                           const Vec3& skyColor,
                           const Vec3& horizonColor,
                           const Vec3& groundColor,
                           const Vec3& sunColor,
                           float exposure,
                           float scatteringIntensity)> setProcedural;
        std::function<void(const Vec3& color, float exposure)> setSolidColor;
        std::function<void(RHITexture* cubemap, float exposure, float rotation, float blurLevel)> setCubemap;
        std::function<void(const char* reason)> clear;
    };

    class SceneSkyboxPassBridge
    {
    public:
        bool Update(World* world,
                    const SceneSkyboxPassActions& passActions,
                    const SceneSkyboxTextureAccess& textureAccess,
                    SceneSkyboxPassBridgeResult* outResult = nullptr) const;

    private:
        static void MarkFallback(SceneSkyboxPassBridgeResult& result,
                                 const SceneSkyboxPassActions& passActions,
                                 SceneSkyboxPassBridgeFallbackReason reason);
    };

} // namespace RVX
