/**
 * @file SpatialSubsystem.cpp
 * @brief SpatialSubsystem implementation
 */

#include "World/SpatialSubsystem.h"
#include "World/World.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneEntity.h"
#include "Runtime/Camera/Camera.h"
#include "Spatial/Index/SpatialFactory.h"
#include "Core/Log.h"

#include <unordered_set>

namespace RVX
{

void SpatialSubsystem::Initialize()
{
    RVX_CORE_INFO("SpatialSubsystem initializing...");

    // Create default BVH index
    m_index = Spatial::SpatialFactory::Create(Spatial::SpatialIndexType::BVH);

    RVX_CORE_INFO("SpatialSubsystem initialized");
}

void SpatialSubsystem::Deinitialize()
{
    RVX_CORE_DEBUG("SpatialSubsystem deinitializing...");
    m_index.reset();
    RVX_CORE_INFO("SpatialSubsystem deinitialized");
}

void SpatialSubsystem::Tick(float deltaTime)
{
    (void)deltaTime;

    // Rebuild index if needed
    if (m_needsRebuild && m_index)
    {
        RebuildIndex();
        m_needsRebuild = false;
    }
}

void SpatialSubsystem::SetIndex(Spatial::SpatialIndexPtr index)
{
    m_index = std::move(index);
    m_needsRebuild = true;
}

void SpatialSubsystem::RebuildIndex()
{
    if (!m_index)
        return;

    World* world = GetWorld();
    if (!world)
        return;

    SceneManager* scene = world->GetSceneManager();
    if (!scene)
        return;

    std::vector<Spatial::ISpatialEntity*> entities;
    scene->CollectSpatialEntities(entities);

    m_index->Build(entities);
}

void SpatialSubsystem::QueryVisible(const Camera& camera, std::vector<SceneEntity*>& outEntities)
{
    // Build frustum from camera matrices
    Frustum frustum;
    frustum.ExtractFromMatrix(camera.GetViewProjection());
    QueryVisible(frustum, outEntities);
}

void SpatialSubsystem::QueryVisible(const Frustum& frustum, std::vector<SceneEntity*>& outEntities)
{
    QueryVisible(frustum, Spatial::QueryFilter{}, outEntities);
}

void SpatialSubsystem::QueryVisible(const Frustum& frustum,
                                    const Spatial::QueryFilter& filter,
                                    std::vector<SceneEntity*>& outEntities)
{
    World* world = GetWorld();
    auto* scene = world ? world->GetSceneManager() : nullptr;
    if (!m_index || !scene)
        return;

    std::vector<Spatial::QueryResult> results;
    m_index->QueryFrustum(frustum, filter, results);

    outEntities.clear();
    outEntities.reserve(results.size());
    std::unordered_set<SceneEntity*> seenEntities;
    for (const auto& result : results)
    {
        auto target = scene->ResolveSpatialQueryTarget(result);
        if (target.entity && seenEntities.insert(target.entity).second)
        {
            outEntities.push_back(target.entity);
        }
    }
}

bool SpatialSubsystem::Raycast(const Ray& ray, RaycastHit& outHit)
{
    return Raycast(ray, Spatial::QueryFilter{}, outHit);
}

bool SpatialSubsystem::Raycast(const Ray& ray,
                               const Spatial::QueryFilter& filter,
                               RaycastHit& outHit)
{
    World* world = GetWorld();
    auto* scene = world ? world->GetSceneManager() : nullptr;
    if (!m_index || !scene)
        return false;

    Spatial::QueryResult result;
    if (m_index->QueryRay(ray, filter, result))
    {
        auto target = scene->ResolveSpatialQueryTarget(result);
        outHit.entity = target.entity;
        outHit.actor = target.actor;
        outHit.component = target.component;
        outHit.distance = result.distance;
        outHit.hitPoint = ray.At(result.distance);
        return outHit.IsValid();
    }
    return false;
}

void SpatialSubsystem::RaycastAll(const Ray& ray, std::vector<RaycastHit>& outHits)
{
    RaycastAll(ray, Spatial::QueryFilter{}, outHits);
}

void SpatialSubsystem::RaycastAll(const Ray& ray,
                                  const Spatial::QueryFilter& filter,
                                  std::vector<RaycastHit>& outHits)
{
    World* world = GetWorld();
    auto* scene = world ? world->GetSceneManager() : nullptr;
    if (!m_index || !scene)
        return;

    std::vector<Spatial::QueryResult> results;
    m_index->QueryRayAll(ray, filter, results);

    outHits.clear();
    outHits.reserve(results.size());
    for (const auto& result : results)
    {
        auto target = scene->ResolveSpatialQueryTarget(result);
        RaycastHit hit;
        hit.entity = target.entity;
        hit.actor = target.actor;
        hit.component = target.component;
        hit.distance = result.distance;
        hit.hitPoint = ray.At(result.distance);
        if (hit.IsValid())
        {
            outHits.push_back(hit);
        }
    }
}

Ray SpatialSubsystem::ScreenToRay(const Camera& camera,
                                  float screenX, float screenY,
                                  float screenWidth, float screenHeight)
{
    // Convert screen coordinates to NDC
    float ndcX = (2.0f * screenX / screenWidth) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / screenHeight);

    // Get inverse view-projection matrix
    Mat4 invViewProj = glm::inverse(camera.GetViewProjection());

    // Transform NDC to world space
    Vec4 nearPoint = invViewProj * Vec4(ndcX, ndcY, 0.0f, 1.0f);
    Vec4 farPoint = invViewProj * Vec4(ndcX, ndcY, 1.0f, 1.0f);

    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    Vec3 origin = Vec3(nearPoint);
    Vec3 direction = normalize(Vec3(farPoint) - origin);

    return Ray{origin, direction};
}

bool SpatialSubsystem::PickScreen(const Camera& camera,
                                  float screenX, float screenY,
                                  float screenWidth, float screenHeight,
                                  RaycastHit& outHit)
{
    Ray ray = ScreenToRay(camera, screenX, screenY, screenWidth, screenHeight);
    return Raycast(ray, outHit);
}

void SpatialSubsystem::QuerySphere(const Vec3& center, float radius,
                                   std::vector<SceneEntity*>& outEntities)
{
    World* world = GetWorld();
    auto* scene = world ? world->GetSceneManager() : nullptr;
    if (!m_index || !scene)
        return;

    std::vector<Spatial::QueryResult> results;
    m_index->QuerySphere(center, radius, Spatial::QueryFilter{}, results);

    outEntities.clear();
    outEntities.reserve(results.size());
    std::unordered_set<SceneEntity*> seenEntities;
    for (const auto& result : results)
    {
        auto target = scene->ResolveSpatialQueryTarget(result);
        if (target.entity && seenEntities.insert(target.entity).second)
        {
            outEntities.push_back(target.entity);
        }
    }
}

void SpatialSubsystem::QueryBox(const AABB& box, std::vector<SceneEntity*>& outEntities)
{
    World* world = GetWorld();
    auto* scene = world ? world->GetSceneManager() : nullptr;
    if (!m_index || !scene)
        return;

    std::vector<Spatial::QueryResult> results;
    m_index->QueryBox(box, Spatial::QueryFilter{}, results);

    outEntities.clear();
    outEntities.reserve(results.size());
    std::unordered_set<SceneEntity*> seenEntities;
    for (const auto& result : results)
    {
        auto target = scene->ResolveSpatialQueryTarget(result);
        if (target.entity && seenEntities.insert(target.entity).second)
        {
            outEntities.push_back(target.entity);
        }
    }
}

} // namespace RVX
