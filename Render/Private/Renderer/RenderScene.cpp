/**
 * @file RenderScene.cpp
 * @brief RenderScene implementation
 */

#include "Render/Renderer/RenderScene.h"

#include "RenderExtraction/RenderProxySceneBridge.h"
#include "RenderContracts/RenderProxy.h"
#include "Core/Log.h"
#include "Core/Math/Frustum.h"
#include "Runtime/Camera/Camera.h"

#include <algorithm>
#include <utility>

namespace RVX
{

void RenderScene::Clear()
{
    m_objects.clear();
    m_lights.clear();
}

void RenderScene::CollectFromWorld(World* world)
{
    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    if (bridge.BuildSnapshot(world, snapshot, &result))
    {
        ApplyProxySnapshot(snapshot);
        return;
    }

    Clear();
    RVX_CORE_WARN("RenderScene::CollectFromWorld proxy extraction failed, reason={}, ownerId={}",
                  ToString(result.fallbackReason),
                  result.fallbackOwnerId);
}

void RenderScene::CollectFromSceneManager(SceneManager* sceneManager)
{
    RenderProxySceneBridge bridge;
    RenderProxySnapshot snapshot;
    RenderProxySceneBridgeResult result;
    if (bridge.BuildSnapshot(sceneManager, snapshot, &result))
    {
        ApplyProxySnapshot(snapshot);
        return;
    }

    Clear();
    RVX_CORE_WARN("RenderScene::CollectFromSceneManager proxy extraction failed, reason={}, ownerId={}",
                  ToString(result.fallbackReason),
                  result.fallbackOwnerId);
}

void RenderScene::ApplyProxySnapshot(const RenderProxySnapshot& snapshot)
{
    Clear();

    m_objects.reserve(snapshot.primitives.size());
    for (const RenderPrimitiveProxy& proxy : snapshot.primitives)
    {
        RenderObject object;
        object.worldMatrix = proxy.worldMatrix;
        object.normalMatrix = proxy.normalMatrix;
        object.bounds = proxy.bounds;
        object.meshId = proxy.meshId;
        object.meshResource = proxy.meshResource;
        object.materialIds = proxy.materialIds;
        object.materialModes = proxy.materialModes;
        object.materialResources = proxy.materialResources;
        object.skinningMatrices = proxy.skinningMatrices;
        object.entityId = proxy.ownerId;
        object.sortKey = proxy.sortKey;
        object.layerMask = proxy.layerMask;
        object.visible = proxy.visible;
        object.castsShadow = proxy.castsShadow;
        object.receivesShadow = proxy.receivesShadow;
        m_objects.push_back(std::move(object));
    }

    m_lights.reserve(snapshot.lights.size());
    for (const RenderLightProxy& proxy : snapshot.lights)
    {
        RenderLight light;
        switch (proxy.type)
        {
            case RenderLightProxy::Type::Directional:
                light.type = RenderLight::Type::Directional;
                break;
            case RenderLightProxy::Type::Point:
                light.type = RenderLight::Type::Point;
                break;
            case RenderLightProxy::Type::Spot:
                light.type = RenderLight::Type::Spot;
                break;
        }

        light.position = proxy.position;
        light.direction = proxy.direction;
        light.color = proxy.color;
        light.intensity = proxy.intensity;
        light.range = proxy.range;
        light.innerConeAngle = proxy.innerConeAngle;
        light.outerConeAngle = proxy.outerConeAngle;
        light.castsShadow = proxy.castsShadow;
        m_lights.push_back(light);
    }
}

void RenderScene::CullAgainstCamera(const Camera& camera, std::vector<uint32_t>& outVisibleIndices) const
{
    outVisibleIndices.clear();
    outVisibleIndices.reserve(m_objects.size());

    // Extract frustum from camera
    Frustum frustum;
    frustum.ExtractFromMatrix(camera.GetViewProjection());

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
    {
        const RenderObject& obj = m_objects[i];

        if (!obj.visible)
            continue;

        if (frustum.IsVisible(obj.bounds))
        {
            outVisibleIndices.push_back(i);
        }
    }
}

void RenderScene::SortVisibleObjects(std::vector<uint32_t>& visibleIndices, const Vec3& cameraPosition) const
{
    // Sort by material/mesh for batching, with depth as secondary sort
    std::sort(visibleIndices.begin(), visibleIndices.end(),
        [this, &cameraPosition](uint32_t a, uint32_t b)
        {
            const RenderObject& objA = m_objects[a];
            const RenderObject& objB = m_objects[b];

            // Primary sort: by sort key (material + mesh hash)
            if (objA.sortKey != objB.sortKey)
                return objA.sortKey < objB.sortKey;

            // Secondary sort: front-to-back for opaque objects
            Vec3 centerA = objA.bounds.GetCenter();
            Vec3 centerB = objB.bounds.GetCenter();
            Vec3 diffA = centerA - cameraPosition;
            Vec3 diffB = centerB - cameraPosition;
            float distA = dot(diffA, diffA);  // length squared
            float distB = dot(diffB, diffB);
            return distA < distB;
        });
}

} // namespace RVX
