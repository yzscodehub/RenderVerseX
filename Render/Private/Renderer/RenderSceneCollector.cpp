/**
 * @file RenderSceneCollector.cpp
 * @brief RenderSceneCollector implementation
 */

#include "RenderSceneCollector.h"

#include "Render/Renderer/RenderScene.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"
#include "World/World.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace RVX
{

void RenderSceneCollector::Collect(RenderScene& outScene, World* world)
{
    outScene.Clear();

    if (!world)
        return;

    SceneManager* sceneManager = world->GetSceneManager();
    if (!sceneManager)
        return;

    const auto& entities = sceneManager->GetEntities();
    for (const auto& [handle, entity] : entities)
    {
        (void)handle;
        if (entity && entity->IsRoot())
        {
            CollectEntity(outScene, entity.get(), Mat4Identity());
        }
    }
}

void RenderSceneCollector::CollectEntity(RenderScene& outScene, SceneEntity* entity, const Mat4& parentMatrix)
{
    if (!entity || !entity->IsActive())
        return;

    Mat4 worldMatrix = parentMatrix * entity->GetLocalMatrix();

    if (auto* renderer = entity->GetComponent<MeshRendererComponent>())
    {
        if (renderer->IsEnabled() && renderer->IsVisible() && renderer->HasValidMesh())
        {
            RenderObject obj;
            obj.worldMatrix = worldMatrix;
            obj.normalMatrix = glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
            obj.bounds = entity->GetWorldBounds();
            obj.meshResource = renderer->GetMesh().Get();
            obj.meshId = renderer->GetMesh().GetId();
            obj.entityId = entity->GetHandle();
            obj.castsShadow = renderer->CastsShadow();
            obj.receivesShadow = renderer->ReceivesShadow();
            obj.visible = true;

            size_t submeshCount = renderer->GetSubmeshCount();
            obj.materialIds.resize(submeshCount);
            obj.materialResources.resize(submeshCount);
            for (size_t i = 0; i < submeshCount; ++i)
            {
                auto material = renderer->GetMaterial(i);
                obj.materialIds[i] = material.IsValid() ? material.GetId() : 0;
                obj.materialResources[i] = material.Get();
            }

            obj.sortKey = obj.materialIds.empty() ? 0 : obj.materialIds[0];
            outScene.AddObject(obj);
        }
    }

    if (auto* lightComp = entity->GetComponent<LightComponent>())
    {
        if (lightComp->IsEnabled())
        {
            RenderLight light;

            switch (lightComp->GetLightType())
            {
                case LightType::Directional:
                    light.type = RenderLight::Type::Directional;
                    break;
                case LightType::Point:
                    light.type = RenderLight::Type::Point;
                    break;
                case LightType::Spot:
                    light.type = RenderLight::Type::Spot;
                    break;
            }

            light.position = entity->GetWorldPosition();

            Mat4 rotMat = glm::mat4_cast(entity->GetWorldRotation());
            light.direction = Vec3(rotMat * Vec4(0, 0, -1, 0));

            light.color = lightComp->GetColor();
            light.intensity = lightComp->GetIntensity();
            light.range = lightComp->GetRange();
            light.innerConeAngle = lightComp->GetInnerConeAngle();
            light.outerConeAngle = lightComp->GetOuterConeAngle();
            light.castsShadow = lightComp->CastsShadow();

            outScene.AddLight(light);
        }
    }

    for (auto* child : entity->GetChildren())
    {
        CollectEntity(outScene, child, worldMatrix);
    }
}

} // namespace RVX
