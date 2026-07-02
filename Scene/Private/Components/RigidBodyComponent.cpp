#include "Scene/Components/RigidBodyComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/Components/ColliderComponent.h"
#include "Physics/RigidBody.h"
#include "Physics/PhysicsWorld.h"

#include <limits>

namespace RVX
{
namespace
{
    Physics::CollisionLayer ToPhysicsCollisionLayer(uint32_t layer)
    {
        constexpr uint32_t maxLayer = std::numeric_limits<Physics::CollisionLayer>::max();
        return static_cast<Physics::CollisionLayer>(layer > maxLayer ? maxLayer : layer);
    }

    float GetSafeInverseMass(float mass)
    {
        return mass > 0.0f ? 1.0f / mass : 0.0f;
    }

    Vec3 GetWorldOffsetFromOwner(const SceneEntity* owner, const Vec3& worldPoint)
    {
        return owner ? worldPoint - owner->GetWorldPosition() : worldPoint;
    }
} // namespace

void RigidBodyComponent::OnAttach()
{
    CreateBody();
}

void RigidBodyComponent::OnDetach()
{
    DestroyBody();
    m_physicsWorld = nullptr;
}

void RigidBodyComponent::Tick(float deltaTime)
{
    (void)deltaTime;

    // World-owned physics sync is ordered by PhysicsSubsystem. This compatibility
    // path is only for manually managed bodies outside a World.
    if (!m_physicsWorld && m_body && m_bodyType == RigidBodyType::Dynamic && !m_sleeping)
    {
        SyncFromPhysics();
    }
}

void RigidBodyComponent::SetBodyType(RigidBodyType type)
{
    if (m_bodyType != type)
    {
        m_bodyType = type;
        UpdateBodyProperties();
    }
}

void RigidBodyComponent::SetMass(float mass)
{
    m_mass = mass;
    if (m_body)
    {
        m_body->SetMass(mass);
    }
}

void RigidBodyComponent::SetUseAutoMass(bool autoMass)
{
    m_useAutoMass = autoMass;
    if (autoMass)
    {
        UpdateAutoMass();
    }
}

void RigidBodyComponent::SetCenterOfMass(const Vec3& com)
{
    m_centerOfMass = com;
    if (m_body)
    {
        m_body->SetCenterOfMass(com);
    }
}

void RigidBodyComponent::SetLinearVelocity(const Vec3& velocity)
{
    m_linearVelocity = velocity;
    if (m_body)
    {
        m_body->SetLinearVelocity(velocity);
    }
}

void RigidBodyComponent::SetAngularVelocity(const Vec3& velocity)
{
    m_angularVelocity = velocity;
    if (m_body)
    {
        m_body->SetAngularVelocity(velocity);
    }
}

Vec3 RigidBodyComponent::GetVelocityAtPoint(const Vec3& worldPoint) const
{
    if (m_body)
    {
        return m_body->GetVelocityAtPoint(worldPoint);
    }
    return m_linearVelocity;
}

void RigidBodyComponent::ApplyForce(const Vec3& force)
{
    if (m_body)
    {
        m_body->ApplyForce(force);
        return;
    }

    m_pendingForce += force;
}

void RigidBodyComponent::ApplyForceAtPoint(const Vec3& force, const Vec3& worldPoint)
{
    if (m_body)
    {
        m_body->ApplyForceAtPoint(force, worldPoint);
    }
    else
    {
        m_pendingForce += force;
        m_pendingTorque += cross(GetWorldOffsetFromOwner(GetOwner(), worldPoint), force);
    }
}

void RigidBodyComponent::ApplyImpulse(const Vec3& impulse)
{
    if (m_body)
    {
        m_body->ApplyImpulse(impulse);
    }
    else
    {
        m_linearVelocity += impulse * GetSafeInverseMass(m_mass);
    }
}

void RigidBodyComponent::ApplyImpulseAtPoint(const Vec3& impulse, const Vec3& worldPoint)
{
    if (m_body)
    {
        m_body->ApplyImpulseAtPoint(impulse, worldPoint);
    }
    else
    {
        m_linearVelocity += impulse * GetSafeInverseMass(m_mass);
        m_angularVelocity += cross(GetWorldOffsetFromOwner(GetOwner(), worldPoint), impulse);
    }
}

void RigidBodyComponent::ApplyTorque(const Vec3& torque)
{
    if (m_body)
    {
        m_body->ApplyTorque(torque);
        return;
    }

    m_pendingTorque += torque;
}

void RigidBodyComponent::ApplyAngularImpulse(const Vec3& impulse)
{
    if (m_body)
    {
        m_body->ApplyAngularImpulse(impulse);
        return;
    }

    m_angularVelocity += impulse;
}

void RigidBodyComponent::ClearForces()
{
    m_pendingForce = Vec3(0.0f);
    m_pendingTorque = Vec3(0.0f);
    if (m_body)
    {
        m_body->ClearForces();
    }
}

void RigidBodyComponent::SetLinearDamping(float damping)
{
    m_linearDamping = damping;
    if (m_body)
    {
        m_body->SetLinearDamping(damping);
    }
}

void RigidBodyComponent::SetAngularDamping(float damping)
{
    m_angularDamping = damping;
    if (m_body)
    {
        m_body->SetAngularDamping(damping);
    }
}

void RigidBodyComponent::SetGravityScale(float scale)
{
    m_gravityScale = scale;
    if (m_body)
    {
        m_body->SetGravityScale(scale);
    }
}

void RigidBodyComponent::SetUseGravity(bool use)
{
    m_useGravity = use;
    if (m_body)
    {
        m_body->SetGravityScale(use ? m_gravityScale : 0.0f);
    }
}

void RigidBodyComponent::SetPositionConstraints(bool x, bool y, bool z)
{
    m_positionConstraints = (x ? 1 : 0) | (y ? 2 : 0) | (z ? 4 : 0);
    if (m_body)
    {
        m_body->SetPositionConstraints(m_positionConstraints);
    }
}

void RigidBodyComponent::GetPositionConstraints(bool& x, bool& y, bool& z) const
{
    x = (m_positionConstraints & 1) != 0;
    y = (m_positionConstraints & 2) != 0;
    z = (m_positionConstraints & 4) != 0;
}

void RigidBodyComponent::SetRotationConstraints(bool x, bool y, bool z)
{
    m_rotationConstraints = (x ? 1 : 0) | (y ? 2 : 0) | (z ? 4 : 0);
    if (m_body)
    {
        m_body->SetRotationConstraints(m_rotationConstraints);
    }
}

void RigidBodyComponent::GetRotationConstraints(bool& x, bool& y, bool& z) const
{
    x = (m_rotationConstraints & 1) != 0;
    y = (m_rotationConstraints & 2) != 0;
    z = (m_rotationConstraints & 4) != 0;
}

void RigidBodyComponent::WakeUp()
{
    m_sleeping = false;
    if (m_body)
    {
        m_body->WakeUp();
    }
}

void RigidBodyComponent::Sleep()
{
    m_sleeping = true;
    if (m_body)
    {
        m_body->SetSleeping(true);
    }
}

void RigidBodyComponent::SetCanSleep(bool canSleep)
{
    m_canSleep = canSleep;
    if (m_body)
    {
        m_body->SetAllowSleep(canSleep);
    }
}

void RigidBodyComponent::SetContinuousDetection(bool use)
{
    m_useCCD = use;
    if (m_body)
    {
        m_body->SetMotionQuality(use ? Physics::MotionQuality::LinearCast
                                     : Physics::MotionQuality::Discrete);
    }
}

void RigidBodyComponent::SetCollisionLayer(uint32_t layer)
{
    m_collisionLayer = layer;
    if (m_body)
    {
        m_body->SetLayer(ToPhysicsCollisionLayer(layer));
    }
}

void RigidBodyComponent::SetCollisionMask(uint32_t mask)
{
    m_collisionMask = mask;
    if (m_body)
    {
        m_body->SetCollisionMask(mask);
    }
}

void RigidBodyComponent::SetCollisionGroup(uint32_t groupId, uint32_t subGroupId)
{
    m_collisionGroupId = groupId;
    m_collisionSubGroupId = subGroupId;
    if (m_body)
    {
        Physics::CollisionGroup group;
        group.groupId = groupId;
        group.subGroupId = subGroupId;
        m_body->SetGroup(group);
    }
}

void RigidBodyComponent::SetPhysicsWorld(Physics::PhysicsWorld* physicsWorld)
{
    if (m_physicsWorld == physicsWorld)
    {
        if (!m_body)
        {
            CreateBody();
        }
        return;
    }

    DestroyBody();
    m_physicsWorld = physicsWorld;
    CreateBody();
}

void RigidBodyComponent::RefreshColliderShape()
{
    const ColliderComponent* collider = nullptr;
    if (SceneEntity* owner = GetOwner())
    {
        collider = owner->GetComponent<ColliderComponent>();
    }

    RefreshColliderShape(collider);
}

void RigidBodyComponent::RefreshColliderShape(const ColliderComponent* collider)
{
    if (!m_body)
    {
        CreateBody();
        if (!m_body)
        {
            return;
        }
    }

    m_body->ClearShapes();
    if (collider && collider->GetShape())
    {
        m_body->AddShape(collider->GetShape(), collider->GetCenter());
        m_body->SetTrigger(collider->IsTrigger());
    }
    else
    {
        m_body->SetTrigger(false);
    }

    UpdateAutoMass();
}

void RigidBodyComponent::SyncToPhysics()
{
    if (!m_body || !GetOwner())
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    m_body->SetPosition(owner->GetWorldPosition());
    m_body->SetRotation(owner->GetWorldRotation());
}

void RigidBodyComponent::SyncFromPhysics()
{
    if (!m_body || !GetOwner())
    {
        return;
    }

    SceneEntity* owner = GetOwner();
    
    // Update cached velocity
    m_linearVelocity = m_body->GetLinearVelocity();
    m_angularVelocity = m_body->GetAngularVelocity();
    m_sleeping = m_body->IsSleeping();

    // Update entity transform from physics
    // Note: For parented entities, we need to convert world to local
    if (owner->GetParent() == nullptr)
    {
        owner->SetPosition(m_body->GetPosition());
        owner->SetRotation(m_body->GetRotation());
    }
    else
    {
        // Convert world position/rotation to local space
        Mat4 parentWorldInverse = inverse(owner->GetParent()->GetWorldMatrix());
        Vec4 localPos = parentWorldInverse * Vec4(m_body->GetPosition(), 1.0f);
        owner->SetPosition(Vec3(localPos));

        // For rotation, multiply by inverse of parent rotation
        Quat parentWorldRot = owner->GetParent()->GetWorldRotation();
        owner->SetRotation(inverse(parentWorldRot) * m_body->GetRotation());
    }
}

void RigidBodyComponent::CreateBody()
{
    if (m_body || !m_physicsWorld)
    {
        return;
    }

    // Get collider from same entity to add shapes
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    Physics::RigidBodyDesc desc;

    switch (m_bodyType)
    {
        case RigidBodyType::Static:
            desc.type = Physics::BodyType::Static;
            break;
        case RigidBodyType::Kinematic:
            desc.type = Physics::BodyType::Kinematic;
            break;
        case RigidBodyType::Dynamic:
            desc.type = Physics::BodyType::Dynamic;
            break;
    }

    desc.position = owner->GetWorldPosition();
    desc.rotation = owner->GetWorldRotation();
    desc.linearVelocity = m_linearVelocity;
    desc.angularVelocity = m_angularVelocity;
    desc.mass = m_mass;
    desc.linearDamping = m_linearDamping;
    desc.angularDamping = m_angularDamping;
    desc.gravityScale = m_useGravity ? m_gravityScale : 0.0f;
    desc.motionQuality = m_useCCD ? Physics::MotionQuality::LinearCast
                                  : Physics::MotionQuality::Discrete;
    desc.positionConstraints = m_positionConstraints;
    desc.rotationConstraints = m_rotationConstraints;
    desc.layer = ToPhysicsCollisionLayer(m_collisionLayer);
    desc.collisionMask = m_collisionMask;
    desc.group.groupId = m_collisionGroupId;
    desc.group.subGroupId = m_collisionSubGroupId;
    desc.allowSleep = m_canSleep;
    desc.startAsleep = m_sleeping;
    desc.userData = owner;

    Physics::BodyHandle handle = m_physicsWorld->CreateBody(desc);
    m_body = m_physicsWorld->GetBodyRef(handle);
    if (!m_body)
    {
        return;
    }

    RefreshColliderShape();

    if (m_pendingForce != Vec3(0.0f))
    {
        m_body->ApplyForce(m_pendingForce);
    }
    if (m_pendingTorque != Vec3(0.0f))
    {
        m_body->ApplyTorque(m_pendingTorque);
    }
    m_pendingForce = Vec3(0.0f);
    m_pendingTorque = Vec3(0.0f);
}

void RigidBodyComponent::DestroyBody()
{
    if (m_body)
    {
        if (m_physicsWorld)
        {
            m_physicsWorld->DestroyBody(m_body->GetHandle());
        }
        m_body.reset();
    }
}

void RigidBodyComponent::UpdateBodyProperties()
{
    if (!m_body)
    {
        return;
    }

    switch (m_bodyType)
    {
        case RigidBodyType::Static:
            m_body->SetType(Physics::BodyType::Static);
            break;
        case RigidBodyType::Kinematic:
            m_body->SetType(Physics::BodyType::Kinematic);
            break;
        case RigidBodyType::Dynamic:
            m_body->SetType(Physics::BodyType::Dynamic);
            break;
    }
}

void RigidBodyComponent::UpdateAutoMass()
{
    if (!m_useAutoMass || !m_body || m_bodyType == RigidBodyType::Static)
    {
        return;
    }

    if (m_body->UpdateMassFromShapes())
    {
        m_mass = m_body->GetMass();
    }
}

} // namespace RVX
