#pragma once

/**
 * @file RenderSceneCollector.h
 * @brief Collects renderable data from World/Scene into RenderScene snapshots
 */

#include "Core/MathTypes.h"

namespace RVX
{
    class RenderScene;
    class SceneEntity;
    class World;

    /**
     * @brief Converts scene entities and components into render-thread friendly data.
     */
    class RenderSceneCollector
    {
    public:
        static void Collect(RenderScene& outScene, World* world);

    private:
        static void CollectEntity(RenderScene& outScene, SceneEntity* entity, const Mat4& parentMatrix);
    };

} // namespace RVX
