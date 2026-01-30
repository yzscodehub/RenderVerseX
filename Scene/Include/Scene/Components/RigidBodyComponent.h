#pragma once

/**
 * @file RigidBodyComponent.h
 * @brief Rigid body physics component
 * 
 * RigidBodyComponent provides physics simulation for entities,
 * including forces, velocity, and collision response.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include <memory>

// Forward declare physics types
namespace RVX::Physics
{
    class RigidBody;
    class PhysicsWorld;
    enum class BodyType : uint8_t;
}

namespace RVX
{

/**
 * @brief Body type for physics simulation
 */
enum class RigidBodyType : uint8_t
{
    Static = 0,     // Never moves, infinite mass
    Kinematic,      // Moved by code, affects dynamics but not affected
    Dynamic         // Fully simulated by physics
};

/**
 * @brief Rigid body component for physics simulation
 * 
 * Features:
 * - Static, kinematic, and dynamic body types
 * - Force and impulse application
 * - Velocity control
 * - Mass and inertia configuration
 * - Damping and gravity settings
 * - Collision layer configuration
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Ball");
 * entity->AddComponent<ColliderComponent>()->SetColliderType(ColliderType::Sphere);
 * 
 * auto* rb = entity->AddComponent<RigidBodyComponent>();
 * rb->SetBodyType(RigidBodyType::Dynamic);
 * rb->SetMass(1.0f);
 * rb->ApplyForce(Vec3(0.0f, 100.0f, 0.0f));
 * @endcode
 */
class RigidBodyComponent : public Component
{
public:
    RigidBodyComponent() = default;
    ~RigidBodyComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "RigidBody"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Body Type
    // =========================================================================

    RigidBodyType GetBodyType() const { return m_bodyType; }
    void SetBodyType(RigidBodyType type);

    bool IsStatic() const { return m_bodyType == RigidBodyType::Static; }
    bool IsKinematic() const { return m_bodyType == RigidBodyType::Kinematic; }
    bool IsDynamic() const { return m_bodyType == RigidBodyType::Dynamic; }

    // =========================================================================
    // Mass Properties
    // =========================================================================

    float GetMass() const { return m_mass; }
    void SetMass(float mass);

    /// Use automatic mass calculation from collider shapes
    bool UseAutoMass() const { return m_useAutoMass; }
    void SetUseAutoMass(bool autoMass);

    /// Center of mass offset (local space)
    const Vec3& GetCenterOfMass() const { return m_centerOfMass; }
    void SetCenterOfMass(const Vec3& com);

    // =========================================================================
    // Velocity
    // =========================================================================

    const Vec3& GetLinearVelocity() const { return m_linearVelocity; }
    void SetLinearVelocity(const Vec3& velocity);

    const Vec3& GetAngularVelocity() const { return m_angularVelocity; }
    void SetAngularVelocity(const Vec3& velocity);

    /// Get velocity at a point in world space
    Vec3 GetVelocityAtPoint(const Vec3& worldPoint) const;

    // =========================================================================
    // Forces & Impulses
    // =========================================================================

    /// Apply force at center of mass (over time)
    void ApplyForce(const Vec3& force);

    /// Apply force at world point
    void ApplyForceAtPoint(const Vec3& force, const Vec3& worldPoint);

    /// Apply instant velocity change at center of mass
    void ApplyImpulse(const Vec3& impulse);

    /// Apply instant velocity change at world point
    void ApplyImpulseAtPoint(const Vec3& impulse, const Vec3& worldPoint);

    /// Apply torque (rotational force)
    void ApplyTorque(const Vec3& torque);

    /// Apply angular impulse
    void ApplyAngularImpulse(const Vec3& impulse);

    /// Clear accumulated forces
    void ClearForces();

    // =========================================================================
    // Damping
    // =========================================================================

    float GetLinearDamping() const { return m_linearDamping; }
    void SetLinearDamping(float damping);

    float GetAngularDamping() const { return m_angularDamping; }
    void SetAngularDamping(float damping);

    // =========================================================================
    // Gravity
    // =========================================================================

    float GetGravityScale() const { return m_gravityScale; }
    void SetGravityScale(float scale);

    bool UseGravity() const { return m_useGravity; }
    void SetUseGravity(bool use);

    // =========================================================================
    // Constraints
    // =========================================================================

    /// Freeze position on specific axes
    void SetPositionConstraints(bool x, bool y, bool z);
    void GetPositionConstraints(bool& x, bool& y, bool& z) const;

    /// Freeze rotation on specific axes
    void SetRotationConstraints(bool x, bool y, bool z);
    void GetRotationConstraints(bool& x, bool& y, bool& z) const;

    // =========================================================================
    // Sleep
    // =========================================================================

    bool IsSleeping() const { return m_sleeping; }
    void WakeUp();
    void Sleep();

    bool CanSleep() const { return m_canSleep; }
    void SetCanSleep(bool canSleep);

    // =========================================================================
    // Collision Settings
    // =========================================================================

    /// Collision detection mode
    bool UseContinuousDetection() const { return m_useCCD; }
    void SetContinuousDetection(bool use);

    /// Collision layer
    uint32_t GetCollisionLayer() const { return m_collisionLayer; }
    void SetCollisionLayer(uint32_t layer);

    /// Collision mask (which layers to collide with)
    uint32_t GetCollisionMask() const { return m_collisionMask; }
    void SetCollisionMask(uint32_t mask);

    // =========================================================================
    // Internal Access
    // =========================================================================

    /// Get the underlying physics body (may be null)
    std::shared_ptr<Physics::RigidBody> GetBody() const { return m_body; }

    /// Sync entity transform to physics body
    void SyncToPhysics();

    /// Sync physics body to entity transform
    void SyncFromPhysics();

private:
    void CreateBody();
    void DestroyBody();
    void UpdateBodyProperties();

    RigidBodyType m_bodyType = RigidBodyType::Dynamic;

    // Mass
    float m_mass = 1.0f;
    bool m_useAutoMass = false;
    Vec3 m_centerOfMass{0.0f};

    // Velocity
    Vec3 m_linearVelocity{0.0f};
    Vec3 m_angularVelocity{0.0f};

    // Damping
    float m_linearDamping = 0.05f;
    float m_angularDamping = 0.05f;

    // Gravity
    float m_gravityScale = 1.0f;
    bool m_useGravity = true;

    // Constraints (bit flags: x=1, y=2, z=4)
    uint8_t m_positionConstraints = 0;
    uint8_t m_rotationConstraints = 0;

    // Sleep
    bool m_sleeping = false;
    bool m_canSleep = true;

    // Collision
    bool m_useCCD = false;
    uint32_t m_collisionLayer = 1;
    uint32_t m_collisionMask = ~0u;

    // Physics body reference
    std::shared_ptr<Physics::RigidBody> m_body;
    Physics::PhysicsWorld* m_physicsWorld = nullptr;

    // Pending forces (accumulated until next physics step)
    Vec3 m_pendingForce{0.0f};
    Vec3 m_pendingTorque{0.0f};
};

} // namespace RVX
