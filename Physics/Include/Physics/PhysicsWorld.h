/**
 * @file PhysicsWorld.h
 * @brief Physics simulation world
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>

namespace RVX::Physics
{

class CollisionShape;
class IConstraint;
using Constraint = IConstraint;  // Alias for compatibility

/**
 * @brief Physics world configuration
 */
struct PhysicsWorldConfig
{
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    uint32 maxBodies = 65536;
    uint32 maxBodyPairs = 65536;
    uint32 maxContactConstraints = 65536;
    int velocitySteps = 10;
    int positionSteps = 2;
    float fixedTimeStep = 1.0f / 60.0f;
};

/**
 * @brief Collision callback type
 */
using CollisionCallback = std::function<void(const CollisionEvent&)>;

/**
 * @brief Physics world manages the physics simulation
 * 
 * Provides:
 * - Rigid body simulation
 * - Collision detection
 * - Constraints/joints
 * - Raycasts and shape queries
 * 
 * Usage:
 * @code
 * PhysicsWorld world;
 * world.Initialize(config);
 * 
 * // Create bodies
 * RigidBodyDesc desc;
 * desc.type = BodyType::Dynamic;
 * desc.position = Vec3(0, 10, 0);
 * BodyHandle body = world.CreateBody(desc);
 * 
 * // Simulate
 * world.Step(deltaTime);
 * 
 * // Query
 * RaycastHit hit;
 * if (world.Raycast(origin, direction, 100.0f, hit)) {
 *     // Handle hit
 * }
 * @endcode
 */
class PhysicsWorld
{
public:
    using Ptr = std::shared_ptr<PhysicsWorld>;

    PhysicsWorld() = default;
    ~PhysicsWorld();

    // Non-copyable
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Initialize the physics world
     */
    bool Initialize(const PhysicsWorldConfig& config = {});

    /**
     * @brief Shutdown and cleanup
     */
    void Shutdown();

    /**
     * @brief Check if initialized
     */
    bool IsInitialized() const { return m_initialized; }

    // =========================================================================
    // Simulation
    // =========================================================================

    /**
     * @brief Step the simulation
     * @param deltaTime Time since last step
     */
    void Step(float deltaTime);

    /**
     * @brief Get current time step
     */
    float GetTimeStep() const { return m_config.fixedTimeStep; }

    /**
     * @brief Set gravity
     */
    void SetGravity(const Vec3& gravity);
    Vec3 GetGravity() const { return m_config.gravity; }

    // =========================================================================
    // Body Management
    // =========================================================================

    /**
     * @brief Create a rigid body
     */
    BodyHandle CreateBody(const RigidBodyDesc& desc);

    /**
     * @brief Destroy a rigid body
     */
    void DestroyBody(BodyHandle body);

    /**
     * @brief Get body count
     */
    size_t GetBodyCount() const { return m_bodies.size(); }

    /**
     * @brief Get a body by handle
     */
    RigidBody* GetBody(BodyHandle handle);
    const RigidBody* GetBody(BodyHandle handle) const;

    // =========================================================================
    // Body Properties (convenience methods)
    // =========================================================================

    void SetBodyPosition(BodyHandle body, const Vec3& position);
    Vec3 GetBodyPosition(BodyHandle body) const;

    void SetBodyRotation(BodyHandle body, const Quat& rotation);
    Quat GetBodyRotation(BodyHandle body) const;

    void SetBodyVelocity(BodyHandle body, const Vec3& velocity);
    Vec3 GetBodyVelocity(BodyHandle body) const;

    void SetBodyAngularVelocity(BodyHandle body, const Vec3& angularVelocity);
    Vec3 GetBodyAngularVelocity(BodyHandle body) const;

    void ApplyForce(BodyHandle body, const Vec3& force);
    void ApplyImpulse(BodyHandle body, const Vec3& impulse);
    void ApplyTorque(BodyHandle body, const Vec3& torque);

    // =========================================================================
    // Collision Shapes
    // =========================================================================

    /**
     * @brief Add a shape to a body
     */
    void AddShape(BodyHandle body, std::shared_ptr<CollisionShape> shape,
                  const Vec3& offset = Vec3(0.0f), const Quat& rotation = Quat(1,0,0,0));

    // =========================================================================
    // Constraints
    // =========================================================================

    /**
     * @brief Create a constraint between two bodies
     */
    uint64 CreateConstraint(std::shared_ptr<Constraint> constraint);

    /**
     * @brief Destroy a constraint
     */
    void DestroyConstraint(uint64 constraintId);

    // =========================================================================
    // Queries
    // =========================================================================

    /**
     * @brief Cast a ray into the world
     * @param origin Ray origin
     * @param direction Ray direction (normalized)
     * @param maxDistance Maximum ray distance
     * @param hit Output hit information
     * @param layerMask Collision layer mask
     * @return True if hit something
     */
    bool Raycast(const Vec3& origin, const Vec3& direction, float maxDistance,
                 RaycastHit& hit, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Cast multiple rays
     */
    size_t RaycastAll(const Vec3& origin, const Vec3& direction, float maxDistance,
                      std::vector<RaycastHit>& hits, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Sweep a sphere through the world
     */
    bool SphereCast(const Vec3& origin, float radius, const Vec3& direction,
                    float maxDistance, ShapeCastHit& hit, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Check for overlapping bodies at a point
     */
    size_t OverlapSphere(const Vec3& center, float radius,
                         std::vector<BodyHandle>& bodies, uint32 layerMask = 0xFFFFFFFF) const;

    // =========================================================================
    // Callbacks
    // =========================================================================

    /**
     * @brief Set callback for collision start
     */
    void SetOnCollisionEnter(CollisionCallback callback) { m_onCollisionEnter = std::move(callback); }

    /**
     * @brief Set callback for collision end
     */
    void SetOnCollisionExit(CollisionCallback callback) { m_onCollisionExit = std::move(callback); }

    /**
     * @brief Set callback for trigger enter
     */
    void SetOnTriggerEnter(CollisionCallback callback) { m_onTriggerEnter = std::move(callback); }

    /**
     * @brief Set callback for trigger exit
     */
    void SetOnTriggerExit(CollisionCallback callback) { m_onTriggerExit = std::move(callback); }

    // =========================================================================
    // Debug
    // =========================================================================

    struct DebugDrawOptions
    {
        bool drawBodies = true;
        bool drawShapes = true;
        bool drawContacts = false;
        bool drawConstraints = false;
        bool drawBroadphase = false;
    };

    /**
     * @brief Get debug draw data
     */
    void GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors,
                          const DebugDrawOptions& options = {}) const;

private:
    // =========================================================================
    // Internal Methods
    // =========================================================================

    /**
     * @brief Internal step with fixed timestep
     */
    void StepInternal(float dt);

    /**
     * @brief Update body sleep states
     */
    void UpdateSleepStates();

    // =========================================================================
    // Data Members
    // =========================================================================

    PhysicsWorldConfig m_config;
    bool m_initialized = false;

    std::vector<std::unique_ptr<RigidBody>> m_bodies;
    std::unordered_map<uint64, size_t> m_bodyLookup;
    uint64 m_nextBodyId = 1;

    std::vector<std::shared_ptr<Constraint>> m_constraints;
    uint64 m_nextConstraintId = 1;

    float m_accumulatedTime = 0.0f;

    CollisionCallback m_onCollisionEnter;
    CollisionCallback m_onCollisionExit;
    CollisionCallback m_onTriggerEnter;
    CollisionCallback m_onTriggerExit;

    // Backend-specific implementation pointer
    void* m_backendData = nullptr;
};

} // namespace RVX::Physics
