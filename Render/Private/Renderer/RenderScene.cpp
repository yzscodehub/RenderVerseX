/**
 * @file RenderScene.cpp
 * @brief RenderScene implementation
 */

#include "Render/Renderer/RenderScene.h"
#include "RenderSceneCollector.h"
#include "Runtime/Camera/Camera.h"
#include "Core/Math/Frustum.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{

void RenderScene::Clear()
{
    m_objects.clear();
    m_lights.clear();
}

void RenderScene::CollectFromWorld(World* world)
{
    RenderSceneCollector::Collect(*this, world);
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
