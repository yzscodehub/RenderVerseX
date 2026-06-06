#pragma once

/**
 * @file RenderProxy.h
 * @brief Render-only scene proxy data consumed by RenderScene.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"

#include <vector>

namespace RVX
{
    namespace Resource
    {
        class MaterialResource;
        class MeshResource;
    }

    struct RenderProxyId
    {
        uint64 value = 0;

        bool IsValid() const { return value != 0; }
    };

    struct RenderPrimitiveProxy
    {
        RenderProxyId id;
        uint64 ownerId = 0;

        Mat4 worldMatrix = Mat4Identity();
        Mat4 normalMatrix = Mat4Identity();
        AABB bounds;

        uint64 meshId = 0;
        Resource::MeshResource* meshResource = nullptr;

        std::vector<uint64> materialIds;
        std::vector<Resource::MaterialResource*> materialResources;

        uint64 sortKey = 0;
        uint32 layerMask = ~0u;
        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;
    };

    struct RenderLightProxy
    {
        enum class Type : uint8
        {
            Directional,
            Point,
            Spot
        };

        RenderProxyId id;
        uint64 ownerId = 0;
        Type type = Type::Directional;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 direction{0.0f, 0.0f, -1.0f};
        Vec3 color{1.0f, 1.0f, 1.0f};
        float intensity = 1.0f;
        float range = 10.0f;
        float innerConeAngle = 0.0f;
        float outerConeAngle = 0.7854f;
        bool castsShadow = false;
    };

    struct RenderProxyCommand
    {
        enum class Type : uint8
        {
            AddOrUpdatePrimitive,
            RemovePrimitive,
            AddOrUpdateLight,
            RemoveLight
        };

        Type type = Type::AddOrUpdatePrimitive;
        RenderProxyId id;
        RenderPrimitiveProxy primitive;
        RenderLightProxy light;
    };

    struct RenderProxySnapshot
    {
        std::vector<RenderPrimitiveProxy> primitives;
        std::vector<RenderLightProxy> lights;

        void Clear()
        {
            primitives.clear();
            lights.clear();
        }
    };

} // namespace RVX
