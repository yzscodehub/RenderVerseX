#pragma once

/**
 * @file RenderSceneCollector.h
 * @brief Collects renderable data from World/Scene into RenderScene snapshots
 */

#include "Core/MathTypes.h"
#include "Scene/SceneEntity.h"

#include <unordered_set>

namespace RVX
{
    class RenderScene;
    class SceneManager;
    class World;

    /**
     * @brief Converts scene entities and components into render-thread friendly data.
     */
    class RenderSceneCollector
    {
    public:
        static void Collect(RenderScene& outScene, World* world);

    private:
        static std::unordered_set<SceneEntity::Handle> CollectRegisteredPrimitives(
            RenderScene& outScene,
            SceneManager* sceneManager);
        static void CollectEntity(RenderScene& outScene,
                                  SceneEntity* entity,
                                  const Mat4& parentMatrix,
                                  const std::unordered_set<SceneEntity::Handle>& primitiveControlledEntities);
    };

} // namespace RVX
