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
        (void)deltaTime;
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
        (void)backendBody;
        // Nothing to do
    }

    void SyncBodyFromBackend(void* backendBody, RigidBody* body) override
    {
        (void)backendBody;
        (void)body;
        // Nothing to sync - built-in operates directly on RigidBody
    }

    void SyncBodyToBackend(RigidBody* body, void* backendBody) override
    {
        (void)body;
        (void)backendBody;
        // Nothing to sync
    }

    void* CreateBackendShape(CollisionShape* shape) override
    {
        return shape;
    }

    void DestroyBackendShape(void* backendShape) override
    {
        (void)backendShape;
        // Nothing to do
    }

    void* CreateBackendConstraint(IConstraint* constraint) override
    {
        return constraint;
    }

    void DestroyBackendConstraint(void* backendConstraint) override
    {
        (void)backendConstraint;
        // Nothing to do
    }

    bool Raycast(const Vec3& origin, const Vec3& direction,
                 float maxDistance, RaycastHit& hit,
                 uint32 layerMask = 0xFFFFFFFF) override
    {
        (void)origin;
        (void)direction;
        (void)maxDistance;
        (void)layerMask;
        // Raycast implementation is in PhysicsWorld
        hit = RaycastHit{};
        return false;
    }

    bool SphereCast(const Vec3& origin, float radius,
                    const Vec3& direction, float maxDistance,
                    ShapeCastHit& hit,
                    uint32 layerMask = 0xFFFFFFFF) override
    {
        (void)origin;
        (void)radius;
        (void)direction;
        (void)maxDistance;
        (void)layerMask;
        hit = ShapeCastHit{};
        return false;
    }

    size_t OverlapSphere(const Vec3& center, float radius,
                         std::vector<BodyHandle>& bodies,
                         uint32 layerMask = 0xFFFFFFFF) override
    {
        (void)center;
        (void)radius;
        (void)layerMask;
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
    return Create(PhysicsBackendType::BuiltIn);
}

IPhysicsBackend::Ptr PhysicsBackendFactory::Create(PhysicsBackendType type)
{
    switch (type)
    {
        case PhysicsBackendType::Auto:
            return CreateDefault();

        case PhysicsBackendType::BuiltIn:
            return std::make_unique<BuiltInBackend>();

        case PhysicsBackendType::Jolt:
            return nullptr;

        default:
            return nullptr;
    }
}

bool PhysicsBackendFactory::IsAvailable(PhysicsBackendType type)
{
    switch (type)
    {
        case PhysicsBackendType::Auto:
            return true;

        case PhysicsBackendType::BuiltIn:
            return true;

        case PhysicsBackendType::Jolt:
            return false;

        default:
            return false;
    }
}

} // namespace RVX::Physics
