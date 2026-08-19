#pragma once

/**
 * @file RenderFragments.h
 * @brief Data-only Scene ECS source fragments consumed by frozen extraction.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "ECS/Fragment.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"

#include <array>

namespace RVX::SceneECS
{
    /** @brief Projection model stored as source data, independent of a render backend. */
    enum class CameraProjection : uint8
    {
        Perspective = 0,
        Orthographic,
    };

    /** @brief Source clear policy for one render view. */
    enum class CameraClearPolicy : uint8
    {
        Skybox = 0,
        SolidColor,
        DepthOnly,
        Nothing,
    };

    /** @brief Value-only view configuration. World-space pose comes from RenderWorldTransform. */
    struct Camera
    {
        CameraProjection projection = CameraProjection::Perspective;
        CameraClearPolicy clearPolicy = CameraClearPolicy::Skybox;
        float32 verticalFieldOfViewRadians = 1.04719755f;
        float32 orthographicHalfHeight = 5.0f;
        float32 nearPlane = 0.1f;
        float32 farPlane = 1000.0f;
        float32 aspectRatio = 1.77777779f;
        float32 exposure = 1.0f;
        Vec4 normalizedViewport{0.0f, 0.0f, 1.0f, 1.0f};
        Vec4 clearColor{0.0f, 0.0f, 0.0f, 1.0f};
        int32 priority = 0;
        uint32 cullingMask = ~0u;
        /** @brief Explicit discontinuity generation advanced by camera cuts. */
        uint64 cutRevision = 1;
        bool enabled = true;
    };

    /** @brief Mesh source identity and extraction flags. */
    struct Mesh
    {
        AssetId meshAssetId{};
        uint32 submeshCount = 0;
        uint32 flags = 0;
    };

    /** @brief One explicit material binding for a mesh submesh. */
    struct MaterialSlot
    {
        AssetId materialAssetId{};
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
    };

    /**
     * @brief Inline material-slot source data for the first ECS render slice.
     *
     * The fixed value array keeps this P2 fragment trivially copyable. A later
     * DynamicBuffer-backed material representation can preserve the same
     * snapshot contract without placing resource objects in the ECS registry.
     */
    struct MaterialSlots
    {
        static constexpr uint32 MaxSlotCount = 16;

        std::array<MaterialSlot, MaxSlotCount> values{};
        uint32 count = 0;
    };

    /** @brief Render-facing visibility, layer, and shadow source data. */
    struct Visibility
    {
        uint32 layerMask = ~0u;
        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;
    };

    /** @brief Backend-neutral light type stored as source data. */
    enum class LightType : uint8
    {
        Directional = 0,
        Point,
        Spot,
    };

    /** @brief Value-only illumination source. World-space pose comes from RenderWorldTransform. */
    struct Light
    {
        LightType type = LightType::Directional;
        Vec3 color{1.0f};
        float32 intensity = 1.0f;
        float32 range = 10.0f;
        float32 innerConeRadians = 0.0f;
        float32 outerConeRadians = 0.78539816f;
        float32 shadowBias = 0.001f;
        bool castsShadows = false;
    };

    /** @brief Backend-neutral environment source mode. */
    enum class SkyboxMode : uint8
    {
        Cubemap = 0,
        Equirectangular,
        Procedural,
        SolidColor,
    };

    /**
     * @brief Value-only sky and IBL source references.
     *
     * Every asset field is a stable AssetId, never a Resource/RHI handle or
     * object pointer. Resource residency and backend binding remain downstream.
     */
    struct Skybox
    {
        SkyboxMode mode = SkyboxMode::SolidColor;
        AssetId environmentAssetId{};
        AssetId prefilteredEnvironmentAssetId{};
        AssetId irradianceAssetId{};
        AssetId brdfLutAssetId{};
        Vec3 solidColor{0.1f, 0.1f, 0.15f};
        Vec3 sunDirection{0.0f, 1.0f, 0.0f};
        Vec3 sunColor{1.0f, 0.95f, 0.9f};
        Vec3 zenithColor{0.2f, 0.4f, 0.8f};
        Vec3 horizonColor{0.7f, 0.8f, 0.9f};
        Vec3 groundColor{0.3f, 0.25f, 0.2f};
        float32 exposure = 1.0f;
        float32 rotationRadians = 0.0f;
        float32 blur = 0.0f;
        float32 scatteringIntensity = 1.0f;
        bool contributesToLighting = true;
    };

    static_assert(ECS::Fragment<Camera>);
    static_assert(ECS::Fragment<Mesh>);
    static_assert(ECS::Fragment<MaterialSlot>);
    static_assert(ECS::Fragment<MaterialSlots>);
    static_assert(ECS::Fragment<Visibility>);
    static_assert(ECS::Fragment<Light>);
    static_assert(ECS::Fragment<Skybox>);
} // namespace RVX::SceneECS
