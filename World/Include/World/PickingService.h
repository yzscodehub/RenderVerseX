#pragma once

/**
 * @file PickingService.h
 * @brief Simplified picking service using SpatialSubsystem
 * 
 * This is a lightweight convenience wrapper around SpatialSubsystem
 * for common picking operations.
 */

#include "Core/Math/Geometry.h"
#include <memory>

namespace RVX
{
    class Camera;
    class World;
    class SpatialSubsystem;
    struct RaycastHit;

    /**
     * @brief Picking configuration
     */
    struct PickingConfig
    {
        bool pickClosest = true;         ///< Find closest hit or first hit
        bool cullBackfaces = false;      ///< Cull backfacing triangles
        float maxDistance = 10000.0f;    ///< Maximum picking distance
    };

    /**
     * @brief Lightweight picking service
     * 
     * Provides convenience methods for object picking.
     * Delegates to SpatialSubsystem for actual raycasting.
     */
    class PickingService
    {
    public:
        explicit PickingService(World* world);

        /// Convert screen coordinates to world ray
        Ray ScreenToRay(const Camera& camera,
                       float screenX, float screenY,
                       float screenWidth, float screenHeight) const;

        /// Pick from screen coordinates
        bool PickScreen(const Camera& camera,
                       float screenX, float screenY,
                       float screenWidth, float screenHeight,
                       RaycastHit& outHit,
                       const PickingConfig& config = {}) const;

        /// Pick with world-space ray
        bool Pick(const Ray& ray, 
                 RaycastHit& outHit,
                 const PickingConfig& config = {}) const;

        /// Check if ray is occluded
        bool IsOccluded(const Ray& ray, float maxDistance = 10000.0f) const;

    private:
        World* m_world = nullptr;
    };

} // namespace RVX
