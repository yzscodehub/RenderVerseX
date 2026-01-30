#pragma once

/**
 * @file CollisionModule.h
 * @brief Collision module - particle collision with planes/world
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
#include <vector>
#include <cstring>

namespace RVX::Particle
{
    /**
     * @brief Collision type
     */
    enum class CollisionType : uint8
    {
        Planes,     ///< Collide with defined planes
        World,      ///< Collide with world geometry (depth buffer)
        Both        ///< Both planes and world
    };

    /**
     * @brief GPU data for collision module
     */
    struct CollisionModuleGPUData
    {
        Vec4 planes[RVX_MAX_COLLISION_PLANES];  ///< xyz=normal, w=distance
        Vec4 params;    ///< x=bounce, y=lifetimeLoss, z=radiusScale, w=planeCount
        Vec4 params2;   ///< x=collisionType, y=minKillSpeed, zw=unused
    };

    /**
     * @brief Particle collision with planes and world geometry
     */
    class CollisionModule : public IParticleModule
    {
    public:
        /// Collision type
        CollisionType type = CollisionType::Planes;

        /// Bounce coefficient (0 = no bounce, 1 = perfect bounce)
        float bounce = 0.5f;

        /// Lifetime lost on collision (0-1)
        float lifetimeLoss = 0.0f;

        /// Radius scale for collision detection
        float radiusScale = 1.0f;

        /// Minimum speed to kill particle on collision
        float minKillSpeed = 0.0f;

        /// Send collision events
        bool sendCollisionMessages = false;

        /// Collision planes (xyz=normal, w=distance from origin)
        std::vector<Vec4> planes;

        /// Add a ground plane at Y=0
        void AddGroundPlane(float height = 0.0f)
        {
            planes.push_back(Vec4(0.0f, 1.0f, 0.0f, -height));
        }

        /// Add a plane
        void AddPlane(const Vec3& normal, float distance)
        {
            planes.push_back(Vec4(normalize(normal), distance));
        }

        const char* GetTypeName() const override { return "CollisionModule"; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(CollisionModuleGPUData);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(CollisionModuleGPUData)) return;
            
            CollisionModuleGPUData data{};
            
            // Copy planes
            uint32 planeCount = static_cast<uint32>(
                std::min(planes.size(), static_cast<size_t>(RVX_MAX_COLLISION_PLANES)));
            for (uint32 i = 0; i < planeCount; ++i)
            {
                data.planes[i] = planes[i];
            }
            
            data.params = Vec4(bounce, lifetimeLoss, radiusScale, 
                               static_cast<float>(planeCount));
            data.params2 = Vec4(static_cast<float>(type), minKillSpeed, 0.0f, 0.0f);
            
            std::memcpy(outData, &data, sizeof(data));
        }
    };

} // namespace RVX::Particle
