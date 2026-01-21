/**
 * @file RenderScene.cpp
 * @brief RenderScene implementation
 */

#include "Render/Renderer/RenderScene.h"
#include "Runtime/Camera/Camera.h"
#include "Core/Math/Frustum.h"
#include "Core/Log.h"
#include "World/World.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Scene/Components/MeshRendererComponent.h"
#include "Scene/Components/LightComponent.h"
#include <algorithm>
#include <glm/gtc/matrix_inverse.hpp>

namespace RVX
{

void RenderScene::Clear()
{
    m_objects.clear();
    m_lights.clear();
}

void RenderScene::CollectFromWorld(World* world)
{
    // Clear previous frame's data
    Clear();

    if (!world)
        return;

    SceneManager* sceneManager = world->GetSceneManager();
    if (!sceneManager)
        return;

    // Get root entities and collect recursively
    const auto& entities = sceneManager->GetEntities();
    for (const auto& [handle, entity] : entities)
    {
        // Only process root entities (no parent) to avoid duplicates
        if (entity && entity->IsRoot())
        {
            CollectFromEntity(entity.get(), Mat4Identity());
        }
    }
}

void RenderScene::CollectFromEntity(SceneEntity* entity, const Mat4& parentMatrix)
{
    if (!entity || !entity->IsActive())
        return;

    // Compute world matrix
    Mat4 worldMatrix = parentMatrix * entity->GetLocalMatrix();

    // Check for MeshRendererComponent
    if (auto* renderer = entity->GetComponent<MeshRendererComponent>())
    {
        if (renderer->IsEnabled() && renderer->IsVisible() && renderer->HasValidMesh())
        {
            RenderObject obj;
            obj.worldMatrix = worldMatrix;
            obj.normalMatrix = glm::inverseTranspose(Mat4(Mat3(worldMatrix)));
            obj.bounds = entity->GetWorldBounds();
            obj.meshId = renderer->GetMesh().GetId();
            obj.entityId = entity->GetHandle();
            obj.castsShadow = renderer->CastsShadow();
            obj.receivesShadow = renderer->ReceivesShadow();
            obj.visible = true;

            // Collect materials for each submesh
            size_t submeshCount = renderer->GetSubmeshCount();
            obj.materialIds.resize(submeshCount);
            for (size_t i = 0; i < submeshCount; ++i)
            {
                auto mat = renderer->GetMaterial(i);
                obj.materialIds[i] = mat.IsValid() ? mat.GetId() : 0;
            }

            // Sort key: use first material ID for batching
            obj.sortKey = obj.materialIds.empty() ? 0 : obj.materialIds[0];

            m_objects.push_back(std::move(obj));
        }
    }

    // Check for LightComponent
    if (auto* lightComp = entity->GetComponent<LightComponent>())
    {
        if (lightComp->IsEnabled())
        {
            RenderLight light;
            
            // Set light type
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
            
            // Get world position and direction
            light.position = entity->GetWorldPosition();
            
            // Direction is the forward vector (negative Z in local space)
            Mat4 rotMat = glm::mat4_cast(entity->GetWorldRotation());
            light.direction = Vec3(rotMat * Vec4(0, 0, -1, 0));
            
            light.color = lightComp->GetColor();
            light.intensity = lightComp->GetIntensity();
            light.range = lightComp->GetRange();
            light.innerConeAngle = lightComp->GetInnerConeAngle();
            light.outerConeAngle = lightComp->GetOuterConeAngle();
            light.castsShadow = lightComp->CastsShadow();
            
            m_lights.push_back(light);
        }
    }

    // Recursively process children
    for (auto* child : entity->GetChildren())
    {
        CollectFromEntity(child, worldMatrix);
    }
}

void RenderScene::CullAgainstCamera(const Camera& camera, std::vector<uint32_t>& outVisibleIndices) const
{
    outVisibleIndices.clear();
    outVisibleIndices.reserve(m_objects.size());

    // Extract frustum from camera
    Frustum frustum;
    frustum.ExtractFromMatrix(camera.GetViewProjection());

    static bool firstCull = true;
    if (firstCull && !m_objects.empty())
    {
        RVX_CORE_INFO("RenderScene::CullAgainstCamera - First cull diagnostics:");
        RVX_CORE_INFO("  Camera position: ({}, {}, {})", 
                      camera.GetPosition().x, camera.GetPosition().y, camera.GetPosition().z);
        RVX_CORE_INFO("  Object count: {}", m_objects.size());
        
        for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
        {
            const RenderObject& obj = m_objects[i];
            Vec3 bmin = obj.bounds.GetMin();
            Vec3 bmax = obj.bounds.GetMax();
            Vec3 center = obj.bounds.GetCenter();
            RVX_CORE_INFO("  Object {}: bounds min=({:.2f}, {:.2f}, {:.2f}) max=({:.2f}, {:.2f}, {:.2f})",
                          i, bmin.x, bmin.y, bmin.z, bmax.x, bmax.y, bmax.z);
            RVX_CORE_INFO("           center=({:.2f}, {:.2f}, {:.2f}), visible={}", 
                          center.x, center.y, center.z, obj.visible);
            
            // Check if bounds are valid (non-zero size)
            Vec3 size = bmax - bmin;
            bool validBounds = (size.x > 0.0001f || size.y > 0.0001f || size.z > 0.0001f);
            RVX_CORE_INFO("           bounds valid: {}", validBounds);
        }
        firstCull = false;
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(m_objects.size()); ++i)
    {
        const RenderObject& obj = m_objects[i];
        
        if (!obj.visible)
            continue;

        // TEMPORARY: Disable frustum culling to test rendering pipeline
        // TODO: Fix Frustum::IsVisible() implementation
        outVisibleIndices.push_back(i);
        
        // Test against frustum (disabled for now)
        // if (frustum.IsVisible(obj.bounds))
        // {
        //     outVisibleIndices.push_back(i);
        // }
    }
    
    // Per-frame log disabled to reduce spam
    // RVX_CORE_DEBUG("RenderScene::CullAgainstCamera - {} visible out of {} objects (culling disabled)", 
    //                outVisibleIndices.size(), m_objects.size());
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
