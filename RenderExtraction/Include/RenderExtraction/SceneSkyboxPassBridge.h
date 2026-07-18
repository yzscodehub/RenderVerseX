#pragma once

/**
 * @file SceneSkyboxPassBridge.h
 * @brief Extract update-owned skybox state into one owned value snapshot.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"

namespace RVX
{
    class World;

    enum class SceneSkyboxSnapshotMode : uint8
    {
        Disabled = 0,
        Cubemap,
        Equirectangular,
        Procedural,
        SolidColor
    };

    enum class SceneSkyboxPassBridgeFallbackReason : uint8
    {
        None = 0,
        NullWorld,
        NullSceneManager,
        NoSkyboxComponent,
        SkyboxTextureMissing
    };

    const char* ToString(SceneSkyboxPassBridgeFallbackReason reason);

    struct SceneSkyboxSnapshot
    {
        SceneSkyboxSnapshotMode mode = SceneSkyboxSnapshotMode::Disabled;
        AssetId textureAssetId;
        Vec3 tint{1.0f};
        Vec3 sunDirection{0.0f, 1.0f, 0.0f};
        Vec3 sunColor{1.0f};
        Vec3 zenithColor{0.2f, 0.4f, 0.8f};
        Vec3 horizonColor{0.7f, 0.8f, 0.9f};
        Vec3 groundColor{0.3f, 0.25f, 0.2f};
        float32 intensity = 1.0f;
        float32 rotationRadians = 0.0f;
        float32 blurLevel = 0.0f;
        float32 scatteringIntensity = 1.0f;
    };

    struct SceneSkyboxPassBridgeResult
    {
        bool skyboxFound = false;
        SceneSkyboxPassBridgeFallbackReason fallbackReason =
            SceneSkyboxPassBridgeFallbackReason::None;
    };

    class SceneSkyboxPassBridge
    {
    public:
        bool Extract(World* world,
                     SceneSkyboxSnapshot& outSnapshot,
                     SceneSkyboxPassBridgeResult* outResult = nullptr) const;
    };
} // namespace RVX
