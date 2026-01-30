#pragma once

/**
 * @file ColliderComponent.h
 * @brief Collision shape component for physics integration
 * 
 * ColliderComponent wraps physics collision shapes and provides
 * integration with the physics system.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include <memory>

// Forward declare physics types to avoid circular dependency
namespace RVX::Physics
{
    class CollisionShape;
    enum class ShapeType : uint8_t;
    struct PhysicsMaterial;
}

namespace RVX
{

/**
 * @brief Collider type for simplified shape creation
 */
enum class ColliderType : uint8_t
{
    Box = 0,
    Sphere,
    Capsule,
    Mesh,       // Triangle mesh (static only)
    Convex      // Convex hull
};

/**
 * @brief Collider component for physics collision
 * 
 * Features:
 * - Box, Sphere, Capsule primitive shapes
 * - Mesh and convex hull colliders
 * - Trigger mode for non-physical collisions
 * - Physics material properties
 * - Center offset and size configuration
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Wall");
 * auto* collider = entity->AddComponent<ColliderComponent>();
 * collider->SetColliderType(ColliderType::Box);
 * collider->SetSize(Vec3(2.0f, 3.0f, 0.2f));
 * @endcode
 */
class ColliderComponent : public Component
{
public:
    ColliderComponent() = default;
    ~ColliderComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Collider"; }

    void OnAttach() override;
    void OnDetach() override;

    // =========================================================================
    // Spatial Bounds
    // =========================================================================

    bool ProvidesBounds() const override { return true; }
    AABB GetLocalBounds() const override;

    // =========================================================================
    // Collider Type
    // =========================================================================

    ColliderType GetColliderType() const { return m_colliderType; }
    void SetColliderType(ColliderType type);

    // =========================================================================
    // Shape Parameters
    // =========================================================================

    /// Center offset from entity origin
    const Vec3& GetCenter() const { return m_center; }
    void SetCenter(const Vec3& center);

    /// Size/extents (interpretation depends on type)
    /// Box: half extents
    /// Sphere: radius in x component
    /// Capsule: radius in x, half-height in y
    const Vec3& GetSize() const { return m_size; }
    void SetSize(const Vec3& size);

    // Convenience setters for specific types
    void SetRadius(float radius);  // For Sphere and Capsule
    void SetHalfHeight(float halfHeight);  // For Capsule
    void SetHalfExtents(const Vec3& halfExtents);  // For Box

    // =========================================================================
    // Trigger Mode
    // =========================================================================

    /// Is this a trigger (non-physical, events only)?
    bool IsTrigger() const { return m_isTrigger; }
    void SetTrigger(bool trigger) { m_isTrigger = trigger; }

    // =========================================================================
    // Physics Material
    // =========================================================================

    float GetFriction() const { return m_friction; }
    void SetFriction(float friction) { m_friction = friction; }

    float GetRestitution() const { return m_restitution; }
    void SetRestitution(float restitution) { m_restitution = restitution; }

    // =========================================================================
    // Internal Shape Access
    // =========================================================================

    /// Get the underlying physics shape (may be null if not created)
    std::shared_ptr<Physics::CollisionShape> GetShape() const { return m_shape; }

    /// Create/update the internal collision shape
    void RebuildShape();

private:
    void CreateBoxShape();
    void CreateSphereShape();
    void CreateCapsuleShape();

    ColliderType m_colliderType = ColliderType::Box;
    Vec3 m_center{0.0f};
    Vec3 m_size{0.5f};  // Default: half extents of 0.5 (1x1x1 box)

    bool m_isTrigger = false;
    float m_friction = 0.5f;
    float m_restitution = 0.3f;

    std::shared_ptr<Physics::CollisionShape> m_shape;
};

} // namespace RVX
