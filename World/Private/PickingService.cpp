/**
 * @file PickingService.cpp
 * @brief PickingService implementation
 */

#include "World/PickingService.h"
#include "World/World.h"
#include "World/SpatialSubsystem.h"
#include "Scene/SceneManager.h"

namespace RVX
{

PickingService::PickingService(World* world)
    : m_world(world)
{
}

Ray PickingService::ScreenToRay(const Camera& camera,
                                float screenX, float screenY,
                                float screenWidth, float screenHeight) const
{
    return SpatialSubsystem::ScreenToRay(camera, screenX, screenY, screenWidth, screenHeight);
}

bool PickingService::PickScreen(const Camera& camera,
                                float screenX, float screenY,
                                float screenWidth, float screenHeight,
                                RaycastHit& outHit,
                                const PickingConfig& config) const
{
    Ray ray = ScreenToRay(camera, screenX, screenY, screenWidth, screenHeight);
    
    // Apply max distance
    // (could be used for optimization in the spatial query)
    (void)config;
    
    return Pick(ray, outHit, config);
}

bool PickingService::Pick(const Ray& ray,
                          RaycastHit& outHit,
                          const PickingConfig& config) const
{
    if (!m_world)
        return false;

    auto* spatial = m_world->GetSpatial();
    if (!spatial)
        return false;

    (void)config; // TODO: Apply config settings

    return spatial->Raycast(ray, outHit);
}

bool PickingService::IsOccluded(const Ray& ray, float maxDistance) const
{
    RaycastHit hit;
    if (Pick(ray, hit, {}))
    {
        return hit.distance < maxDistance;
    }
    return false;
}

} // namespace RVX
