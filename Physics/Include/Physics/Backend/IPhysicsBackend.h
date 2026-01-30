/**
 * @file IPhysicsBackend.h
 * @brief Physics backend abstraction interface
 * 
 * Allows switching between different physics engine implementations:
 * - Built-in simple physics engine
 * - Jolt Physics (high performance)
 * - Future: PhysX, Bullet, etc.
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include "Physics/Shapes/CollisionShape.h"
#include <memory>
#include <vector>

namespace RVX::Physics
{

class IConstraint;

/**
 * @brief Physics backend type
 */
enum class PhysicsBackendType : uint8
{
    BuiltIn,    ///< Simple built-in physics engine
    Jolt,       ///< Jolt Physics backend
    PhysX,      ///< NVIDIA PhysX (future)
    Bullet      ///< Bullet Physics (future)
};

/**
 * @brief Backend configuration
 */
struct PhysicsBackendConfig
{
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    uint32 maxBodies = 65536;
    uint32 maxBodyPairs = 65536;
    uint32 maxContactConstraints = 65536;
    int velocitySteps = 10;
    int positionSteps = 2;
    bool enableSleeping = true;
    bool enableCCD = true;
};

/**
 * @brief Abstract interface for physics engine backends
 */
class IPhysicsBackend
{
public:
    using Ptr = std::unique_ptr<IPhysicsBackend>;

    virtual ~IPhysicsBackend() = default;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Initialize the backend
     */
    virtual bool Initialize(const PhysicsBackendConfig& config) = 0;

    /**
     * @brief Shutdown the backend
     */
    virtual void Shutdown() = 0;

    /**
     * @brief Get backend type
     */
    virtual PhysicsBackendType GetType() const = 0;

    /**
     * @brief Get backend name
     */
    virtual const char* GetName() const = 0;

    // =========================================================================
    // Simulation
    // =========================================================================

    /**
     * @brief Step the simulation
     */
    virtual void Step(float deltaTime) = 0;

    /**
     * @brief Set gravity
     */
    virtual void SetGravity(const Vec3& gravity) = 0;

    // =========================================================================
    // Body Management
    // =========================================================================

    /**
     * @brief Create a body in the backend
     */
    virtual void* CreateBackendBody(RigidBody* body) = 0;

    /**
     * @brief Destroy a backend body
     */
    virtual void DestroyBackendBody(void* backendBody) = 0;

    /**
     * @brief Sync body state from backend to RigidBody
     */
    virtual void SyncBodyFromBackend(void* backendBody, RigidBody* body) = 0;

    /**
     * @brief Sync body state from RigidBody to backend
     */
    virtual void SyncBodyToBackend(RigidBody* body, void* backendBody) = 0;

    // =========================================================================
    // Shapes
    // =========================================================================

    /**
     * @brief Create a backend shape
     */
    virtual void* CreateBackendShape(CollisionShape* shape) = 0;

    /**
     * @brief Destroy a backend shape
     */
    virtual void DestroyBackendShape(void* backendShape) = 0;

    // =========================================================================
    // Constraints
    // =========================================================================

    /**
     * @brief Create a backend constraint
     */
    virtual void* CreateBackendConstraint(IConstraint* constraint) = 0;

    /**
     * @brief Destroy a backend constraint
     */
    virtual void DestroyBackendConstraint(void* backendConstraint) = 0;

    // =========================================================================
    // Queries
    // =========================================================================

    /**
     * @brief Raycast into the world
     */
    virtual bool Raycast(const Vec3& origin, const Vec3& direction,
                         float maxDistance, RaycastHit& hit,
                         uint32 layerMask = 0xFFFFFFFF) = 0;

    /**
     * @brief Sphere cast
     */
    virtual bool SphereCast(const Vec3& origin, float radius,
                            const Vec3& direction, float maxDistance,
                            ShapeCastHit& hit,
                            uint32 layerMask = 0xFFFFFFFF) = 0;

    /**
     * @brief Overlap sphere query
     */
    virtual size_t OverlapSphere(const Vec3& center, float radius,
                                 std::vector<BodyHandle>& bodies,
                                 uint32 layerMask = 0xFFFFFFFF) = 0;
};

/**
 * @brief Factory for creating physics backends
 */
class PhysicsBackendFactory
{
public:
    /**
     * @brief Create the default backend (Jolt if available, built-in otherwise)
     */
    static IPhysicsBackend::Ptr CreateDefault();

    /**
     * @brief Create a specific backend
     */
    static IPhysicsBackend::Ptr Create(PhysicsBackendType type);

    /**
     * @brief Check if a backend type is available
     */
    static bool IsAvailable(PhysicsBackendType type);
};

} // namespace RVX::Physics
