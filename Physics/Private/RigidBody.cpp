/**
 * @file RigidBody.cpp
 * @brief RigidBody implementation
 */

#include "Physics/RigidBody.h"
#include "Physics/Shapes/CollisionShape.h"

#include <limits>

namespace RVX::Physics
{

RigidBody::RigidBody(const RigidBodyDesc& desc)
    : m_type(desc.type)
    , m_position(desc.position)
    , m_rotation(desc.rotation)
    , m_linearVelocity(desc.linearVelocity)
    , m_angularVelocity(desc.angularVelocity)
    , m_mass(desc.mass)
    , m_linearDamping(desc.linearDamping)
    , m_angularDamping(desc.angularDamping)
    , m_gravityScale(desc.gravityScale)
    , m_layer(desc.layer)
    , m_group(desc.group)
    , m_isTrigger(desc.isTrigger)
    , m_sleeping(desc.startAsleep)
    , m_allowSleep(desc.allowSleep)
    , m_userData(desc.userData)
{
    m_inverseMass = (desc.type == BodyType::Static || desc.mass <= 0.0f) ? 0.0f : 1.0f / desc.mass;
}

void RigidBody::SetType(BodyType type)
{
    m_type = type;
    if (type == BodyType::Static)
    {
        m_inverseMass = 0.0f;
        m_linearVelocity = Vec3(0.0f);
        m_angularVelocity = Vec3(0.0f);
    }
    else if (m_mass > 0.0f)
    {
        m_inverseMass = 1.0f / m_mass;
    }
}

void RigidBody::SetPosition(const Vec3& position)
{
    m_position = position;
    WakeUp();
}

void RigidBody::SetRotation(const Quat& rotation)
{
    m_rotation = normalize(rotation);
    WakeUp();
}

Mat4 RigidBody::GetTransform() const
{
    Mat4 result = mat4_cast(m_rotation);
    result[3] = Vec4(m_position, 1.0f);
    return result;
}

void RigidBody::SetTransform(const Vec3& position, const Quat& rotation)
{
    m_position = position;
    m_rotation = normalize(rotation);
    WakeUp();
}

void RigidBody::SetLinearVelocity(const Vec3& velocity)
{
    if (m_type == BodyType::Static) return;
    m_linearVelocity = velocity;
    WakeUp();
}

void RigidBody::SetAngularVelocity(const Vec3& velocity)
{
    if (m_type == BodyType::Static) return;
    m_angularVelocity = velocity;
    WakeUp();
}

Vec3 RigidBody::GetVelocityAtPoint(const Vec3& worldPoint) const
{
    Vec3 r = worldPoint - m_position;
    return m_linearVelocity + cross(m_angularVelocity, r);
}

void RigidBody::ApplyForce(const Vec3& force)
{
    if (m_type != BodyType::Dynamic) return;
    m_force += force;
    WakeUp();
}

void RigidBody::ApplyForceAtPoint(const Vec3& force, const Vec3& worldPoint)
{
    if (m_type != BodyType::Dynamic) return;
    m_force += force;
    m_torque += cross(worldPoint - m_position, force);
    WakeUp();
}

void RigidBody::ApplyImpulse(const Vec3& impulse)
{
    if (m_type != BodyType::Dynamic) return;
    m_linearVelocity += impulse * m_inverseMass;
    WakeUp();
}

void RigidBody::ApplyImpulseAtPoint(const Vec3& impulse, const Vec3& worldPoint)
{
    if (m_type != BodyType::Dynamic) return;
    m_linearVelocity += impulse * m_inverseMass;
    // Simplified angular impulse (would need inverse inertia tensor)
    Vec3 r = worldPoint - m_position;
    m_angularVelocity += cross(r, impulse);
    WakeUp();
}

void RigidBody::ApplyTorque(const Vec3& torque)
{
    if (m_type != BodyType::Dynamic) return;
    m_torque += torque;
    WakeUp();
}

void RigidBody::ApplyAngularImpulse(const Vec3& impulse)
{
    if (m_type != BodyType::Dynamic) return;
    m_angularVelocity += impulse;
    WakeUp();
}

void RigidBody::ClearForces()
{
    m_force = Vec3(0.0f);
    m_torque = Vec3(0.0f);
}

void RigidBody::SetMass(float mass)
{
    m_mass = mass;
    m_inverseMass = (m_type == BodyType::Static || mass <= 0.0f) ? 0.0f : 1.0f / mass;
}

void RigidBody::SetCenterOfMass(const Vec3& com)
{
    m_centerOfMass = com;
}

void RigidBody::SetLinearDamping(float damping)
{
    m_linearDamping = std::max(0.0f, damping);
}

void RigidBody::SetAngularDamping(float damping)
{
    m_angularDamping = std::max(0.0f, damping);
}

void RigidBody::SetGravityScale(float scale)
{
    m_gravityScale = scale;
}

void RigidBody::SetLayer(CollisionLayer layer)
{
    m_layer = layer;
}

void RigidBody::SetGroup(const CollisionGroup& group)
{
    m_group = group;
}

void RigidBody::SetTrigger(bool isTrigger)
{
    m_isTrigger = isTrigger;
}

void RigidBody::AddShape(std::shared_ptr<CollisionShape> shape,
                         const Vec3& offset, const Quat& rotation)
{
    m_shapes.push_back({std::move(shape), offset, rotation});
}

void RigidBody::SetSleeping(bool sleep)
{
    if (m_type == BodyType::Static) return;
    m_sleeping = sleep;
}

void RigidBody::WakeUp()
{
    if (m_type == BodyType::Static) return;
    m_sleeping = false;
}

void RigidBody::SetAllowSleep(bool allow)
{
    m_allowSleep = allow;
    if (!allow) WakeUp();
}

RigidBody::AABB RigidBody::GetAABB() const
{
    AABB result;

    if (m_shapes.empty())
    {
        // No shapes, return point AABB at body position
        result.min = m_position - Vec3(0.01f);
        result.max = m_position + Vec3(0.01f);
        return result;
    }

    // Start with large negative/positive bounds
    result.min = Vec3(std::numeric_limits<float>::max());
    result.max = Vec3(std::numeric_limits<float>::lowest());

    // Get world transform
    Mat4 worldMat = GetTransform();

    // Compute AABB from all shapes
    for (const auto& instance : m_shapes)
    {
        Vec3 localMin, localMax;
        instance.shape->GetLocalBounds(localMin, localMax);

        // Transform all 8 corners to world space
        Vec3 corners[8] = {
            Vec3(localMin.x, localMin.y, localMin.z),
            Vec3(localMax.x, localMin.y, localMin.z),
            Vec3(localMin.x, localMax.y, localMin.z),
            Vec3(localMax.x, localMax.y, localMin.z),
            Vec3(localMin.x, localMin.y, localMax.z),
            Vec3(localMax.x, localMin.y, localMax.z),
            Vec3(localMin.x, localMax.y, localMax.z),
            Vec3(localMax.x, localMax.y, localMax.z)
        };

        for (const auto& corner : corners)
        {
            // Apply rotation offset
            Vec3 rotated = glm::rotate(instance.rotation, corner - instance.offset) + instance.offset;
            // Apply world transform
            Vec3 worldPos = Vec3(worldMat * Vec4(rotated, 1.0f));
            result.min = glm::min(result.min, worldPos);
            result.max = glm::max(result.max, worldPos);
        }
    }

    return result;
}

} // namespace RVX::Physics
