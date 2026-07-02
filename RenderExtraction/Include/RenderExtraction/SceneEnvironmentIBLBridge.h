#pragma once

/**
 * @file SceneEnvironmentIBLBridge.h
 * @brief Extract scene skybox IBL resources into a render-side environment snapshot.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderResource.h"

namespace RVX
{
    class World;

    enum class SceneEnvironmentIBLFallbackReason : uint8
    {
        None = 0,
        NullWorld,
        NullSceneManager,
        NoLightingSkybox,
        SkyboxIBLResourcesMissing
    };

    const char* ToString(SceneEnvironmentIBLFallbackReason reason);

    struct SceneEnvironmentIBLSnapshot
    {
        IRenderTextureUploadSource* irradiance = nullptr;
        IRenderTextureUploadSource* prefiltered = nullptr;
        IRenderTextureUploadSource* brdfLUT = nullptr;
        uint32 prefilteredMipLevels = 1;
        float intensity = 1.0f;
    };

    struct SceneEnvironmentIBLBridgeResult
    {
        bool skyboxFound = false;
        bool textureIBLEnabled = false;
        SceneEnvironmentIBLFallbackReason fallbackReason = SceneEnvironmentIBLFallbackReason::None;
    };

    class SceneEnvironmentIBLBridge
    {
    public:
        bool Extract(World* world,
                     SceneEnvironmentIBLSnapshot& outSnapshot,
                     SceneEnvironmentIBLBridgeResult* outResult = nullptr) const;
    };

} // namespace RVX
