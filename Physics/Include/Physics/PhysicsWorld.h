/**
 * @file PhysicsWorld.h
 * @brief Physics simulation world
 */

#pragma once

#include "Physics/Backend/IPhysicsBackend.h"
#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    PhysicsBackendType backend = PhysicsBackendType::Auto;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    uint32 maxBodies = 65536;
    uint32 maxBodyPairs = 65536;
    uint32 maxContactConstraints = 65536;
    int velocitySteps = 10;
    int positionSteps = 2;
    float fixedTimeStep = 1.0f / 60.0f;
    uint32 maxSubSteps = 4;
    float sleepVelocityThreshold = 0.1f;
    float sleepTimeThreshold = 0.5f;
};

/**
 * @brief Collision callback type
 */
using CollisionCallback = std::function<void(const CollisionEvent&)>;

/**
 * @brief Deterministic runtime state for physics qualification samples.
 *
 * Lifetime counters reset on Initialize(). Live census fields are derived from
 * the current world and therefore report zero after Shutdown().
 */
struct PhysicsRuntimeDiagnosticsSnapshot
{
    uint64 fixedStepSequence = 0;
    uint32 lastSubstepCount = 0;
    float droppedSimulationTimeSeconds = 0.0f;
    uint64 substepClampCount = 0;

    size_t staticBodyCount = 0;
    size_t dynamicBodyCount = 0;
    size_t kinematicBodyCount = 0;
    size_t colliderCount = 0;

    uint64 createCount = 0;
    uint64 destroyCount = 0;
    uint64 recreateCount = 0;
    uint64 colliderRebuildCount = 0;
    uint64 staleHandleRejectCount = 0;

    /** Owned by the World synchronization layer; zero for a standalone world. */
    uint64 sceneToPhysicsPushCount = 0;
    /** Owned by the World synchronization layer; zero for a standalone world. */
    uint64 physicsToScenePullCount = 0;

    uint64 bodyPoseHash = 0;
};

/** @brief Handle-only creation outcome for clients that must not retain RigidBody pointers. */
enum class BodyCreateStatus : uint8
{
    Created = 0,
    WorldUnavailable,
    CapacityExceeded,
    AllocationFailed,
};

struct BodyCreateResult
{
    BodyCreateStatus status = BodyCreateStatus::WorldUnavailable;
    BodyHandle handle = BodyHandle::Invalid();

    [[nodiscard]] bool Succeeded() const { return status == BodyCreateStatus::Created; }
};

/** @brief Explicit handle-only destruction outcome. AlreadyAbsent is safe to acknowledge. */
enum class BodyDestroyStatus : uint8
{
    Destroyed = 0,
    AlreadyAbsent,
    ReleaseFailed,
};

/** @brief Value pose consumed and produced by handle-only body clients. */
struct BodyPose
{
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/** @brief Mutable rigid-body configuration excluding pose and velocities. */
struct BodyConfiguration
{
    BodyType type = BodyType::Dynamic;
    MotionQuality motionQuality = MotionQuality::Discrete;
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
    bool isTrigger = false;
};

/** @brief Snapshot of all bridge-relevant body values. */
struct BodyState
{
    BodyPose pose;
    Vec3 linearVelocity{0.0f};
    Vec3 angularVelocity{0.0f};
    BodyConfiguration configuration;
    bool sleeping = false;
};

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
     * @brief Advance the simulation exactly once using a caller-owned fixed delta.
     *
     * This path intentionally does not touch the compatibility accumulator used
     * by Step(). World-level schedulers should use it when they own fixed-step
     * accumulation and phase ordering.
     */
    void StepFixed(float fixedDelta);

    /**
     * @brief Get current time step
     */
    float GetTimeStep() const { return m_config.fixedTimeStep; }
    uint32 GetMaxSubSteps() const { return m_config.maxSubSteps; }
    uint32 GetLastStepCount() const { return m_lastStepCount; }
    float GetAccumulatedTime() const { return m_accumulatedTime; }
    PhysicsRuntimeDiagnosticsSnapshot GetRuntimeDiagnosticsSnapshot() const;

    /**
     * @brief Set gravity
     */
    void SetGravity(const Vec3& gravity);
    Vec3 GetGravity() const { return m_config.gravity; }
    PhysicsBackendType GetRequestedBackendType() const { return m_requestedBackend; }
    PhysicsBackendType GetActiveBackendType() const { return m_activeBackend; }
    const char* GetActiveBackendName() const;
    bool IsBackendFallbackActive() const { return m_backendFallbackActive; }

    // =========================================================================
    // Body Management
    // =========================================================================

    /**
     * @brief Create a rigid body
     */
    BodyHandle CreateBody(const RigidBodyDesc& desc);

    /**
     * @brief Create through the generation-safe, value-only body contract.
     *
     * This is the ECS-facing lifecycle surface. It deliberately returns no
     * RigidBody ownership or pointer and keeps legacy pointer APIs separate.
     */
    [[nodiscard]] BodyCreateResult CreateBodyValue(const RigidBodyDesc& desc);

    /**
     * @brief Destroy a rigid body
     */
    void DestroyBody(BodyHandle body);

    /** @brief Destroy through the value-only body contract. */
    [[nodiscard]] BodyDestroyStatus DestroyBodyValue(BodyHandle body);

    /** @brief Read bridge-relevant state without exposing RigidBody storage. */
    [[nodiscard]] std::optional<BodyState> ReadBodyState(BodyHandle body) const;
    /** @brief Teleport a body through its generation-safe handle. */
    [[nodiscard]] bool WriteBodyPose(BodyHandle body, const BodyPose& pose);
    /** @brief Apply bridge-relevant configuration through its generation-safe handle. */
    [[nodiscard]] bool WriteBodyConfiguration(BodyHandle body,
                                              const BodyConfiguration& configuration);

    /**
     * @brief Get body count
     */
    size_t GetBodyCount() const { return m_bodies.size(); }

    /** @brief Get the total number of collision shapes attached to bodies. */
    size_t GetColliderCount() const;

    /**
     * @brief Get a body by handle
     */
    RigidBody* GetBody(BodyHandle handle);
    const RigidBody* GetBody(BodyHandle handle) const;
    std::shared_ptr<RigidBody> GetBodyRef(BodyHandle handle) const;

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

    /**
     * @brief Replace the component-owned collider without exposing an empty
     * intermediate body state.
     * @return False when the handle is stale or replacement preparation fails.
     */
    bool ReplaceBodyCollider(BodyHandle body,
                             std::shared_ptr<CollisionShape> shape,
                             const Vec3& offset = Vec3(0.0f),
                             const Quat& rotation = Quat(1,0,0,0),
                             bool isTrigger = false,
                             bool recordRebuild = true);

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

    /**
     * @brief Get active constraint count
     */
    size_t GetConstraintCount() const { return m_constraints.size(); }
    // =========================================================================
    // Queries
    // =========================================================================

    struct QueryStats
    {
        size_t broadphaseNodeVisits = 0;
        size_t broadphaseCandidateCount = 0;
        size_t narrowphaseTestCount = 0;
        size_t bodyWithoutShapeSkipCount = 0;
    };

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

    /**
     * @brief Check for bodies overlapping an axis-aligned box
     */
    size_t OverlapBox(const Vec3& center, const Vec3& halfExtents,
                      std::vector<BodyHandle>& bodies, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Check for bodies overlapping an oriented box
     */
    size_t OverlapBox(const Vec3& center, const Vec3& halfExtents, const Quat& rotation,
                      std::vector<BodyHandle>& bodies, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Check for bodies overlapping a capsule segment
     */
    size_t OverlapCapsule(const Vec3& pointA, const Vec3& pointB, float radius,
                          std::vector<BodyHandle>& bodies, uint32 layerMask = 0xFFFFFFFF) const;

    /**
     * @brief Get diagnostic counters for the last physics query
     */
    const QueryStats& GetLastQueryStats() const { return m_lastQueryStats; }

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
    void GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors) const;
    void GetDebugDrawData(std::vector<Vec3>& lines, std::vector<Vec4>& colors,
                          const DebugDrawOptions& options) const;

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
    void UpdateSleepStates(float deltaTime);

    /**
     * @brief Resolve built-in AABB contacts for dynamic bodies
     */
    void ResolveBodyCollisions();

    /**
     * @brief Sweep a linear-cast body through the current body AABBs
     */
    bool TryLinearCastBody(const RigidBody& movingBody,
                           const Vec3& displacement,
                           float& hitFraction,
                           Vec3& hitNormal) const;

    /**
     * @brief Mark constraints broken when applied force exceeds their threshold
     */
    void UpdateConstraintBreakage(float deltaTime);

    /**
     * @brief Update collision and trigger enter/exit callbacks
     */
    void UpdateCollisionEvents();

    /**
     * @brief Remove cached collision pairs containing a body id
     */
    void RemoveActivePairsForBody(uint64 bodyId);

    bool IsLiveBodyHandle(BodyHandle handle) const;
    void RecordStaleHandleReject(BodyHandle handle) const;
    bool TryReleaseBodySlot(BodyHandle handle);

    struct BodyPairKey
    {
        uint64 bodyA = 0;
        uint64 bodyB = 0;

        bool operator==(const BodyPairKey& other) const
        {
            return bodyA == other.bodyA && bodyB == other.bodyB;
        }
    };

    struct BodyPairKeyHash
    {
        size_t operator()(const BodyPairKey& key) const
        {
            const size_t hashA = std::hash<uint64>{}(key.bodyA);
            const size_t hashB = std::hash<uint64>{}(key.bodyB);
            return hashA ^ (hashB + 0x9e3779b9u + (hashA << 6u) + (hashA >> 2u));
        }
    };

    // =========================================================================
    // Data Members
    // =========================================================================

    PhysicsWorldConfig m_config;
    bool m_initialized = false;

    std::vector<std::shared_ptr<RigidBody>> m_bodies;
    std::unordered_map<uint64, size_t> m_bodyLookup;
    struct BodySlot
    {
        uint32 generation = 0;
        bool allocated = false;
        bool everAllocated = false;
        /** A generation cannot wrap because that would revive a stale handle. */
        bool retired = false;
    };
    /**
     * Slot generations intentionally survive Shutdown() so a stale handle
     * cannot alias a body created by a later Initialize() call.
     */
    std::vector<BodySlot> m_bodySlots;
    std::vector<uint32> m_freeBodySlots;

    friend class PhysicsWorldTestAccess;

    std::vector<std::shared_ptr<Constraint>> m_constraints;
    uint64 m_nextConstraintId = 1;

    float m_accumulatedTime = 0.0f;
    uint32 m_lastStepCount = 0;
    uint64 m_fixedStepSequence = 0;
    float m_droppedSimulationTimeSeconds = 0.0f;
    uint64 m_substepClampCount = 0;
    uint64 m_createCount = 0;
    uint64 m_destroyCount = 0;
    uint64 m_recreateCount = 0;
    uint64 m_colliderRebuildCount = 0;
    mutable uint64 m_staleHandleRejectCount = 0;

    CollisionCallback m_onCollisionEnter;
    CollisionCallback m_onCollisionExit;
    CollisionCallback m_onTriggerEnter;
    CollisionCallback m_onTriggerExit;
    std::unordered_set<BodyPairKey, BodyPairKeyHash> m_activeCollisionPairs;
    mutable QueryStats m_lastQueryStats;

    IPhysicsBackend::Ptr m_backend;
    PhysicsBackendType m_requestedBackend = PhysicsBackendType::Auto;
    PhysicsBackendType m_activeBackend = PhysicsBackendType::BuiltIn;
    bool m_backendFallbackActive = false;
};

} // namespace RVX::Physics
