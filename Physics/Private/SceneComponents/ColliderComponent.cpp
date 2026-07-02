#include "Scene/Components/ColliderComponent.h"
#include "Scene/Components/RigidBodyComponent.h"
#include "Scene/SceneEntity.h"

// Include physics headers for shape creation
// Note: Using forward declarations in header to avoid circular dependency
#include "Physics/Shapes/CollisionShape.h"

#include <algorithm>
#include <cmath>

namespace RVX
{

void ColliderComponent::OnAttach()
{
    // Create initial shape based on type
    RebuildShape();
    NotifyBoundsChanged();
    NotifyRigidBodyShapeChanged();
}

void ColliderComponent::OnDetach()
{
    // Clear shape reference
    m_shape.reset();
    NotifyRigidBodyShapeChanged();
}

AABB ColliderComponent::GetLocalBounds() const
{
    Vec3 halfExtents;

    switch (m_colliderType)
    {
        case ColliderType::Box:
            halfExtents = m_size;
            break;

        case ColliderType::Sphere:
            halfExtents = Vec3(m_size.x);  // Radius
            break;

        case ColliderType::Capsule:
        {
            float radius = m_size.x;
            float totalHeight = m_size.y + radius;  // Half-height + radius
            halfExtents = Vec3(radius, totalHeight, radius);
            break;
        }

        case ColliderType::Mesh:
        case ColliderType::Convex:
            // For mesh/convex, use stored size or compute from shape
            halfExtents = m_size;
            break;
    }

    return AABB(m_center - halfExtents, m_center + halfExtents);
}

void ColliderComponent::SetColliderType(ColliderType type)
{
    if (m_colliderType != type)
    {
        m_colliderType = type;
        RebuildShape();
        NotifyBoundsChanged();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetCenter(const Vec3& center)
{
    if (m_center != center)
    {
        m_center = center;
        NotifyBoundsChanged();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetSize(const Vec3& size)
{
    if (m_size != size)
    {
        m_size = size;
        RebuildShape();
        NotifyBoundsChanged();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetRadius(float radius)
{
    if (m_size.x != radius)
    {
        m_size.x = radius;
        RebuildShape();
        NotifyBoundsChanged();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetHalfHeight(float halfHeight)
{
    if (m_size.y != halfHeight)
    {
        m_size.y = halfHeight;
        RebuildShape();
        NotifyBoundsChanged();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetHalfExtents(const Vec3& halfExtents)
{
    SetSize(halfExtents);
}

void ColliderComponent::SetTrigger(bool trigger)
{
    if (m_isTrigger != trigger)
    {
        m_isTrigger = trigger;
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetFriction(float friction)
{
    if (m_friction != friction)
    {
        m_friction = friction;
        RebuildShape();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetRestitution(float restitution)
{
    if (m_restitution != restitution)
    {
        m_restitution = restitution;
        RebuildShape();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::SetDensity(float density)
{
    const float sanitizedDensity = std::isfinite(density) ? std::max(0.0f, density) : 0.0f;
    if (m_density != sanitizedDensity)
    {
        m_density = sanitizedDensity;
        RebuildShape();
        NotifyRigidBodyShapeChanged();
    }
}

void ColliderComponent::RebuildShape()
{
    switch (m_colliderType)
    {
        case ColliderType::Box:
            CreateBoxShape();
            break;

        case ColliderType::Sphere:
            CreateSphereShape();
            break;

        case ColliderType::Capsule:
            CreateCapsuleShape();
            break;

        case ColliderType::Mesh:
        case ColliderType::Convex:
            // Mesh and convex shapes require external data
            // These would typically be set via SetMesh() or similar
            m_shape.reset();
            break;
    }

    // Apply material properties if shape was created
    if (m_shape)
    {
        Physics::PhysicsMaterial material;
        material.friction = m_friction;
        material.restitution = m_restitution;
        material.density = m_density;
        m_shape->SetMaterial(material);
    }
}

void ColliderComponent::CreateBoxShape()
{
    m_shape = Physics::BoxShape::Create(m_size);
}

void ColliderComponent::CreateSphereShape()
{
    m_shape = Physics::SphereShape::Create(m_size.x);
}

void ColliderComponent::CreateCapsuleShape()
{
    m_shape = Physics::CapsuleShape::Create(m_size.x, m_size.y);
}

void ColliderComponent::NotifyRigidBodyShapeChanged()
{
    SceneEntity* owner = GetOwner();
    if (!owner)
    {
        return;
    }

    if (auto* rigidBody = owner->GetComponent<RigidBodyComponent>())
    {
        rigidBody->RefreshColliderShape(this);
    }
}

} // namespace RVX
