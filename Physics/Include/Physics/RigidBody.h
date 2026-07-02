/**
 * @file RigidBody.h
 * @brief Rigid body definition and management
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include "Core/MathTypes.h"
#include <memory>
#include <vector>

namespace RVX::Physics
{

class CollisionShape;

/**
 * @brief Rigid body description for creation
 */
struct RigidBodyDesc
{
    BodyType type = BodyType::Dynamic;
    MotionQuality motionQuality = MotionQuality::Discrete;

    Vec3 position{0.0f};
    Quat rotation{1, 0, 0, 0};
    Vec3 linearVelocity{0.0f};
    Vec3 angularVelocity{0.0f};

    float mass = 1.0f;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float gravityScale = 1.0f;
    uint8 positionConstraints = 0;
    uint8 rotationConstraints = 0;

    CollisionLayer layer = Layers::Dynamic;
    uint32 collisionMask = 0xFFFFFFFFu;
    CollisionGroup group;

    bool allowSleep = true;
    bool startAsleep = false;
    bool isTrigger = false;

    void* userData = nullptr;
};

/**
 * @brief Rigid body in the physics simulation
 */
class RigidBody
{
public:
    RigidBody() = default;
    explicit RigidBody(const RigidBodyDesc& desc);
    ~RigidBody() = default;

    // Non-copyable
    RigidBody(const RigidBody&) = delete;
    RigidBody& operator=(const RigidBody&) = delete;

    // =========================================================================
    // Identity
    // =========================================================================

    uint64 GetId() const { return m_id; }
    void SetId(uint64 id) { m_id = id; }

    BodyHandle GetHandle() const { return BodyHandle(m_id); }

    // =========================================================================
    // Type
    // =========================================================================

    BodyType GetType() const { return m_type; }
    void SetType(BodyType type);

    bool IsStatic() const { return m_type == BodyType::Static; }
    bool IsKinematic() const { return m_type == BodyType::Kinematic; }
    bool IsDynamic() const { return m_type == BodyType::Dynamic; }

    MotionQuality GetMotionQuality() const { return m_motionQuality; }
    void SetMotionQuality(MotionQuality quality);

    // =========================================================================
    // Transform
    // =========================================================================

    const Vec3& GetPosition() const { return m_position; }
    void SetPosition(const Vec3& position);

    const Quat& GetRotation() const { return m_rotation; }
    void SetRotation(const Quat& rotation);

    Mat4 GetTransform() const;
    void SetTransform(const Vec3& position, const Quat& rotation);

    // =========================================================================
    // Velocity
    // =========================================================================

    const Vec3& GetLinearVelocity() const { return m_linearVelocity; }
    void SetLinearVelocity(const Vec3& velocity);

    const Vec3& GetAngularVelocity() const { return m_angularVelocity; }
    void SetAngularVelocity(const Vec3& velocity);

    Vec3 GetVelocityAtPoint(const Vec3& worldPoint) const;

    // =========================================================================
    // Forces
    // =========================================================================

    void ApplyForce(const Vec3& force);
    void ApplyForceAtPoint(const Vec3& force, const Vec3& worldPoint);
    void ApplyImpulse(const Vec3& impulse);
    void ApplyImpulseAtPoint(const Vec3& impulse, const Vec3& worldPoint);
    void ApplyTorque(const Vec3& torque);
    void ApplyAngularImpulse(const Vec3& impulse);

    const Vec3& GetAccumulatedForce() const { return m_force; }
    const Vec3& GetAccumulatedTorque() const { return m_torque; }
    void ClearForces();

    // =========================================================================
    // Mass
    // =========================================================================

    float GetMass() const { return m_mass; }
    void SetMass(float mass);

    float GetInverseMass() const { return m_inverseMass; }

    Vec3 GetCenterOfMass() const { return m_centerOfMass; }
    void SetCenterOfMass(const Vec3& com);

    float CalculateMassFromShapes() const;
    bool UpdateMassFromShapes();

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

    // =========================================================================
    // Constraints
    // =========================================================================

    uint8 GetPositionConstraints() const { return m_positionConstraints; }
    void SetPositionConstraints(uint8 constraints);

    uint8 GetRotationConstraints() const { return m_rotationConstraints; }
    void SetRotationConstraints(uint8 constraints);

    // =========================================================================
    // Collision
    // =========================================================================
    CollisionLayer GetLayer() const { return m_layer; }
    void SetLayer(CollisionLayer layer);

    uint32 GetCollisionMask() const { return m_collisionMask; }
    void SetCollisionMask(uint32 mask);

    const CollisionGroup& GetGroup() const { return m_group; }
    void SetGroup(const CollisionGroup& group);

    bool IsTrigger() const { return m_isTrigger; }
    void SetTrigger(bool isTrigger);

    // =========================================================================
    // Shapes
    // =========================================================================

    void AddShape(std::shared_ptr<CollisionShape> shape,
                  const Vec3& offset = Vec3(0.0f),
                  const Quat& rotation = Quat(1,0,0,0));

    void ClearShapes();
    size_t GetShapeCount() const { return m_shapes.size(); }

    /**
     * @brief Get world-space axis-aligned bounding box
     */
    struct AABB
    {
        Vec3 min;
        Vec3 max;
    };
    AABB GetAABB() const;

    // =========================================================================
    // Sleep
    // =========================================================================

    bool IsSleeping() const { return m_sleeping; }
    void SetSleeping(bool sleep);
    void WakeUp();

    bool CanSleep() const { return m_allowSleep; }
    void SetAllowSleep(bool allow);
    float GetSleepTimer() const { return m_sleepTimer; }
    void AccumulateSleepTime(float deltaTime);
    void ResetSleepTimer();

    // =========================================================================
    // User Data
    // =========================================================================

    void* GetUserData() const { return m_userData; }
    void SetUserData(void* data) { m_userData = data; }

private:
    uint64 m_id = 0;
    BodyType m_type = BodyType::Dynamic;
    MotionQuality m_motionQuality = MotionQuality::Discrete;

    Vec3 m_position{0.0f};
    Quat m_rotation{1, 0, 0, 0};
    Vec3 m_linearVelocity{0.0f};
    Vec3 m_angularVelocity{0.0f};

    Vec3 m_force{0.0f};
    Vec3 m_torque{0.0f};

    float m_mass = 1.0f;
    float m_inverseMass = 1.0f;
    Vec3 m_centerOfMass{0.0f};

    float m_linearDamping = 0.05f;
    float m_angularDamping = 0.05f;
    float m_gravityScale = 1.0f;
    uint8 m_positionConstraints = 0;
    uint8 m_rotationConstraints = 0;

    CollisionLayer m_layer = Layers::Dynamic;
    uint32 m_collisionMask = 0xFFFFFFFFu;
    CollisionGroup m_group;
    bool m_isTrigger = false;

    bool m_sleeping = false;
    bool m_allowSleep = true;
    float m_sleepTimer = 0.0f;

    struct ShapeInstance
    {
        std::shared_ptr<CollisionShape> shape;
        Vec3 offset;
        Quat rotation;
    };
    std::vector<ShapeInstance> m_shapes;

    void* m_userData = nullptr;
    void* m_backendBody = nullptr;  // Backend-specific body pointer
};

} // namespace RVX::Physics
