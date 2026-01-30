/**
 * @file BuiltInBackend.cpp
 * @brief Built-in physics backend implementation
 */

#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/RigidBody.h"
#include <vector>

namespace RVX::Physics
{

/**
 * @brief Built-in physics backend
 * 
 * Simple physics engine for when external backends (Jolt) are not available.
 * Provides basic functionality:
 * - Rigid body dynamics
 * - Simple collision detection
 * - Constraint solving
 */
class BuiltInBackend : public IPhysicsBackend
{
public:
    bool Initialize(const PhysicsBackendConfig& config) override
    {
        m_config = config;
        m_initialized = true;
        return true;
    }

    void Shutdown() override
    {
        m_initialized = false;
    }

    PhysicsBackendType GetType() const override
    {
        return PhysicsBackendType::BuiltIn;
    }

    const char* GetName() const override
    {
        return "Built-in";
    }

    void Step(float deltaTime) override
    {
        // Built-in stepping is handled in PhysicsWorld directly
    }

    void SetGravity(const Vec3& gravity) override
    {
        m_config.gravity = gravity;
    }

    void* CreateBackendBody(RigidBody* body) override
    {
        // Built-in doesn't need separate backend bodies
        return body;
    }

    void DestroyBackendBody(void* backendBody) override
    {
        // Nothing to do
    }

    void SyncBodyFromBackend(void* backendBody, RigidBody* body) override
    {
        // Nothing to sync - built-in operates directly on RigidBody
    }

    void SyncBodyToBackend(RigidBody* body, void* backendBody) override
    {
        // Nothing to sync
    }

    void* CreateBackendShape(CollisionShape* shape) override
    {
        return shape;
    }

    void DestroyBackendShape(void* backendShape) override
    {
        // Nothing to do
    }

    void* CreateBackendConstraint(IConstraint* constraint) override
    {
        return constraint;
    }

    void DestroyBackendConstraint(void* backendConstraint) override
    {
        // Nothing to do
    }

    bool Raycast(const Vec3& origin, const Vec3& direction,
                 float maxDistance, RaycastHit& hit,
                 uint32 layerMask = 0xFFFFFFFF) override
    {
        // Raycast implementation is in PhysicsWorld
        hit = RaycastHit{};
        return false;
    }

    bool SphereCast(const Vec3& origin, float radius,
                    const Vec3& direction, float maxDistance,
                    ShapeCastHit& hit,
                    uint32 layerMask = 0xFFFFFFFF) override
    {
        hit = ShapeCastHit{};
        return false;
    }

    size_t OverlapSphere(const Vec3& center, float radius,
                         std::vector<BodyHandle>& bodies,
                         uint32 layerMask = 0xFFFFFFFF) override
    {
        bodies.clear();
        return 0;
    }

private:
    PhysicsBackendConfig m_config;
    bool m_initialized = false;
};

// =========================================================================
// Factory Implementation
// =========================================================================

IPhysicsBackend::Ptr PhysicsBackendFactory::CreateDefault()
{
#ifdef RVX_PHYSICS_JOLT
    return Create(PhysicsBackendType::Jolt);
#else
    return Create(PhysicsBackendType::BuiltIn);
#endif
}

IPhysicsBackend::Ptr PhysicsBackendFactory::Create(PhysicsBackendType type)
{
    switch (type)
    {
        case PhysicsBackendType::BuiltIn:
            return std::make_unique<BuiltInBackend>();

#ifdef RVX_PHYSICS_JOLT
        case PhysicsBackendType::Jolt:
            // Would create JoltBackend here
            return std::make_unique<BuiltInBackend>();  // Fallback for now
#endif

        default:
            return std::make_unique<BuiltInBackend>();
    }
}

bool PhysicsBackendFactory::IsAvailable(PhysicsBackendType type)
{
    switch (type)
    {
        case PhysicsBackendType::BuiltIn:
            return true;

        case PhysicsBackendType::Jolt:
#ifdef RVX_PHYSICS_JOLT
            return true;
#else
            return false;
#endif

        default:
            return false;
    }
}

} // namespace RVX::Physics
