#include "Scene/Components/RigidBodyComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/Components/ColliderComponent.h"
#include "Physics/RigidBody.h"
#include "Physics/PhysicsWorld.h"

namespace RVX
{

void RigidBodyComponent::OnAttach()
{
    CreateBody();
}

void RigidBodyComponent::OnDetach()
{
    DestroyBody();
}

void RigidBodyComponent::Tick(float deltaTime)
{
    (void)deltaTime;

    // Sync transform from physics to entity (if dynamic)
    if (m_body && m_bodyType == RigidBodyType::Dynamic && !m_sleeping)
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
    // Recalculate mass from colliders if enabled
    if (autoMass && m_body)
    {
        // TODO: Calculate mass from attached colliders
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
    m_pendingForce += force;
    if (m_body)
    {
        m_body->ApplyForce(force);
    }
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
        // Apply directly to velocity if no body
        m_linearVelocity += impulse / m_mass;
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
        ApplyImpulse(impulse);
    }
}

void RigidBodyComponent::ApplyTorque(const Vec3& torque)
{
    m_pendingTorque += torque;
    if (m_body)
    {
        m_body->ApplyTorque(torque);
    }
}

void RigidBodyComponent::ApplyAngularImpulse(const Vec3& impulse)
{
    if (m_body)
    {
        m_body->ApplyAngularImpulse(impulse);
    }
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
    // TODO: Apply to physics body
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
    // TODO: Apply to physics body
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
    // TODO: Apply to physics body
}

void RigidBodyComponent::SetCollisionLayer(uint32_t layer)
{
    m_collisionLayer = layer;
    if (m_body)
    {
        m_body->SetLayer(layer);
    }
}

void RigidBodyComponent::SetCollisionMask(uint32_t mask)
{
    m_collisionMask = mask;
    // TODO: Apply to physics body collision group
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
    // TODO: Get physics world from scene/engine
    // For now, body creation will be deferred until physics world is available

    // Get collider from same entity to add shapes
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    ColliderComponent* collider = owner->GetComponent<ColliderComponent>();
    if (collider && collider->GetShape())
    {
        // Create body with collider shape
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
        desc.mass = m_mass;
        desc.linearDamping = m_linearDamping;
        desc.angularDamping = m_angularDamping;
        desc.gravityScale = m_useGravity ? m_gravityScale : 0.0f;
        desc.allowSleep = m_canSleep;
        desc.userData = owner;

        m_body = std::make_shared<Physics::RigidBody>(desc);
        m_body->AddShape(collider->GetShape());
    }
}

void RigidBodyComponent::DestroyBody()
{
    if (m_body)
    {
        // TODO: Remove from physics world
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

} // namespace RVX
