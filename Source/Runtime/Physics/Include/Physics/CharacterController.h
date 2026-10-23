/**
 * @file CharacterController.h
 * @brief Character controller for player/NPC movement
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/Shapes/CollisionShape.h"
#include <memory>

namespace RVX::Physics
{

/**
 * @brief Ground state information
 */
struct GroundState
{
    bool grounded = false;          ///< Is character on ground
    Vec3 groundNormal{0, 1, 0};     ///< Normal of ground surface
    Vec3 groundPoint{0};            ///< Contact point on ground
    float groundDistance = 0.0f;     ///< Distance to ground
    BodyHandle groundBody;           ///< Body we're standing on
    bool onSlope = false;           ///< Is on a slope
    float slopeAngle = 0.0f;        ///< Angle of slope in radians
};

/**
 * @brief Configuration for character controller
 */
struct CharacterControllerDesc
{
    float height = 1.8f;            ///< Total character height
    float radius = 0.3f;            ///< Capsule radius
    float stepHeight = 0.35f;       ///< Maximum step-up height
    float maxSlopeAngle = 0.785f;   ///< Maximum walkable slope (45 degrees)
    float skinWidth = 0.02f;        ///< Collision skin width
    float groundProbeDepth = 0.1f;  ///< How far to probe for ground
    float mass = 80.0f;             ///< Character mass for physics interactions
    Vec3 upDirection{0, 1, 0};      ///< Up direction (for gravity)
    CollisionLayer layer = Layers::Character;
};

/**
 * @brief Move result containing collision info
 */
struct MoveResult
{
    Vec3 finalPosition;             ///< Position after movement
    Vec3 velocity;                  ///< Final velocity
    bool hitCeiling = false;        ///< Hit ceiling during move
    bool hitWall = false;           ///< Hit wall during move
    int collisionCount = 0;         ///< Number of collisions
};

/**
 * @brief Character controller for kinematic character movement
 * 
 * Provides:
 * - Capsule-based collision
 * - Ground detection
 * - Slope handling
 * - Step climbing
 * - Collision slide
 * 
 * Usage:
 * @code
 * CharacterControllerDesc desc;
 * desc.height = 1.8f;
 * desc.radius = 0.3f;
 * 
 * CharacterController controller(world, desc);
 * controller.SetPosition(Vec3(0, 1, 0));
 * 
 * // In update loop
 * Vec3 moveDir = GetInputDirection();
 * controller.Move(moveDir * speed, deltaTime);
 * 
 * if (controller.IsGrounded() && jumpPressed)
 * {
 *     controller.ApplyImpulse(Vec3(0, jumpForce, 0));
 * }
 * @endcode
 */
class CharacterController
{
public:
    using Ptr = std::shared_ptr<CharacterController>;

    CharacterController() = default;
    
    /**
     * @brief Create character controller
     * @param world Physics world to use
     * @param desc Controller configuration
     */
    CharacterController(PhysicsWorld* world, const CharacterControllerDesc& desc);
    
    ~CharacterController();

    // Non-copyable
    CharacterController(const CharacterController&) = delete;
    CharacterController& operator=(const CharacterController&) = delete;

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Initialize the character controller
     */
    bool Initialize(PhysicsWorld* world, const CharacterControllerDesc& desc);

    /**
     * @brief Shutdown and cleanup
     */
    void Shutdown();

    /**
     * @brief Check if initialized
     */
    bool IsInitialized() const { return m_world != nullptr; }

    // =========================================================================
    // Movement
    // =========================================================================

    /**
     * @brief Move the character by the given displacement
     * @param displacement World-space movement vector
     * @param deltaTime Time step
     * @return Movement result with collision info
     */
    MoveResult Move(const Vec3& displacement, float deltaTime);

    /**
     * @brief Set velocity (will be applied during next Move)
     */
    void SetVelocity(const Vec3& velocity) { m_velocity = velocity; }
    Vec3 GetVelocity() const { return m_velocity; }

    /**
     * @brief Apply impulse to character
     */
    void ApplyImpulse(const Vec3& impulse);

    /**
     * @brief Jump with given velocity
     */
    void Jump(float jumpSpeed);

    // =========================================================================
    // Position
    // =========================================================================

    /**
     * @brief Get current position (center of capsule)
     */
    Vec3 GetPosition() const { return m_position; }

    /**
     * @brief Set position directly (teleport)
     */
    void SetPosition(const Vec3& position);

    /**
     * @brief Get foot position
     */
    Vec3 GetFootPosition() const
    {
        return m_position - m_desc.upDirection * (m_desc.height * 0.5f - m_desc.radius);
    }

    /**
     * @brief Get head position
     */
    Vec3 GetHeadPosition() const
    {
        return m_position + m_desc.upDirection * (m_desc.height * 0.5f - m_desc.radius);
    }

    // =========================================================================
    // Ground State
    // =========================================================================

    /**
     * @brief Check if character is grounded
     */
    bool IsGrounded() const { return m_groundState.grounded; }

    /**
     * @brief Get detailed ground information
     */
    const GroundState& GetGroundState() const { return m_groundState; }

    /**
     * @brief Check if on walkable slope
     */
    bool IsOnWalkableGround() const
    {
        return m_groundState.grounded && m_groundState.slopeAngle <= m_desc.maxSlopeAngle;
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Get controller description
     */
    const CharacterControllerDesc& GetDesc() const { return m_desc; }

    /**
     * @brief Set maximum slope angle (radians)
     */
    void SetMaxSlopeAngle(float angle) { m_desc.maxSlopeAngle = angle; }

    /**
     * @brief Set step height
     */
    void SetStepHeight(float height) { m_desc.stepHeight = height; }

    /**
     * @brief Enable/disable gravity
     */
    void SetGravityEnabled(bool enabled) { m_gravityEnabled = enabled; }
    bool IsGravityEnabled() const { return m_gravityEnabled; }

    /**
     * @brief Set gravity scale
     */
    void SetGravityScale(float scale) { m_gravityScale = scale; }
    float GetGravityScale() const { return m_gravityScale; }

    // =========================================================================
    // Collision
    // =========================================================================

    /**
     * @brief Get the collision shape
     */
    CapsuleShape* GetCollisionShape() const { return m_capsuleShape.get(); }

    /**
     * @brief Set collision layer
     */
    void SetCollisionLayer(CollisionLayer layer) { m_desc.layer = layer; }
    CollisionLayer GetCollisionLayer() const { return m_desc.layer; }

    // =========================================================================
    // Factory
    // =========================================================================

    static Ptr Create(PhysicsWorld* world, const CharacterControllerDesc& desc)
    {
        return std::make_shared<CharacterController>(world, desc);
    }

private:
    // =========================================================================
    // Internal Methods
    // =========================================================================

    /**
     * @brief Update ground state
     */
    void UpdateGroundState();

    /**
     * @brief Perform ground probe
     */
    bool ProbeGround(float probeDistance, RaycastHit& hit);

    /**
     * @brief Slide movement against obstacles
     */
    Vec3 SlideMove(const Vec3& displacement, int maxIterations = 4);

    /**
     * @brief Try to step over obstacle
     */
    bool TryStepUp(const Vec3& moveDir, float stepHeight, Vec3& outNewPosition);

    /**
     * @brief Check if position is valid (no penetration)
     */
    bool IsPositionValid(const Vec3& position);

    /**
     * @brief Resolve penetration at position
     */
    Vec3 ResolvePenetration(const Vec3& position);

    /**
     * @brief Apply gravity
     */
    void ApplyGravity(float deltaTime);

    /**
     * @brief Project velocity onto ground plane
     */
    Vec3 ProjectOntoGroundPlane(const Vec3& velocity);

    /**
     * @brief Check if surface is walkable
     */
    bool IsWalkableSurface(const Vec3& normal);

    // =========================================================================
    // Members
    // =========================================================================

    PhysicsWorld* m_world = nullptr;
    CharacterControllerDesc m_desc;

    Vec3 m_position{0};
    Vec3 m_velocity{0};
    GroundState m_groundState;

    std::shared_ptr<CapsuleShape> m_capsuleShape;

    bool m_gravityEnabled = true;
    float m_gravityScale = 1.0f;

    // Collision state
    int m_pendingMoves = 0;
    static constexpr int MAX_MOVE_ITERATIONS = 4;
};

// =========================================================================
// Implementation
// =========================================================================

inline CharacterController::CharacterController(PhysicsWorld* world, const CharacterControllerDesc& desc)
{
    Initialize(world, desc);
}

inline CharacterController::~CharacterController()
{
    Shutdown();
}

inline bool CharacterController::Initialize(PhysicsWorld* world, const CharacterControllerDesc& desc)
{
    m_world = world;
    m_desc = desc;

    // Create capsule shape
    float halfHeight = (desc.height - 2.0f * desc.radius) * 0.5f;
    m_capsuleShape = std::make_shared<CapsuleShape>(desc.radius, halfHeight);

    m_position = Vec3(0);
    m_velocity = Vec3(0);
    m_groundState = GroundState{};

    return true;
}

inline void CharacterController::Shutdown()
{
    m_capsuleShape.reset();
    m_world = nullptr;
}

inline void CharacterController::SetPosition(const Vec3& position)
{
    m_position = position;
    UpdateGroundState();
}

inline void CharacterController::ApplyImpulse(const Vec3& impulse)
{
    m_velocity += impulse / m_desc.mass;
}

inline void CharacterController::Jump(float jumpSpeed)
{
    if (IsGrounded())
    {
        m_velocity += m_desc.upDirection * jumpSpeed;
        m_groundState.grounded = false;
    }
}

inline MoveResult CharacterController::Move(const Vec3& displacement, float deltaTime)
{
    MoveResult result;

    // Apply gravity
    if (m_gravityEnabled && !m_groundState.grounded)
    {
        ApplyGravity(deltaTime);
    }

    // Combine input displacement with velocity
    Vec3 totalMove = displacement + m_velocity * deltaTime;

    // Project onto ground if grounded
    if (m_groundState.grounded && m_groundState.slopeAngle <= m_desc.maxSlopeAngle)
    {
        totalMove = ProjectOntoGroundPlane(totalMove);
    }

    // Perform sliding move
    Vec3 newPos = SlideMove(totalMove, MAX_MOVE_ITERATIONS);
    m_position = newPos;

    // Update ground state
    UpdateGroundState();

    // Clear velocity component into ground if grounded
    if (m_groundState.grounded)
    {
        float downVel = dot(m_velocity, -m_desc.upDirection);
        if (downVel > 0)
        {
            m_velocity += m_desc.upDirection * downVel;
        }
    }

    result.finalPosition = m_position;
    result.velocity = m_velocity;

    return result;
}

inline void CharacterController::UpdateGroundState()
{
    RaycastHit hit;
    if (ProbeGround(m_desc.groundProbeDepth + m_desc.skinWidth, hit))
    {
        m_groundState.grounded = hit.distance <= m_desc.groundProbeDepth + m_desc.skinWidth;
        m_groundState.groundNormal = hit.normal;
        m_groundState.groundPoint = hit.point;
        m_groundState.groundDistance = hit.distance;
        m_groundState.groundBody = BodyHandle(hit.bodyId);

        // Calculate slope angle
        float cosAngle = dot(hit.normal, m_desc.upDirection);
        m_groundState.slopeAngle = std::acos(clamp(cosAngle, -1.0f, 1.0f));
        m_groundState.onSlope = m_groundState.slopeAngle > 0.01f;
    }
    else
    {
        m_groundState.grounded = false;
        m_groundState.groundNormal = m_desc.upDirection;
        m_groundState.slopeAngle = 0.0f;
        m_groundState.onSlope = false;
    }
}

inline bool CharacterController::ProbeGround(float probeDistance, RaycastHit& hit)
{
    if (!m_world) return false;

    Vec3 origin = GetFootPosition() + m_desc.upDirection * m_desc.skinWidth;
    Vec3 direction = -m_desc.upDirection;

    return m_world->Raycast(origin, direction, probeDistance, hit);
}

inline Vec3 CharacterController::SlideMove(const Vec3& displacement, int maxIterations)
{
    Vec3 currentPos = m_position;
    Vec3 remainingMove = displacement;
    
    for (int i = 0; i < maxIterations && length(remainingMove) > 0.001f; ++i)
    {
        Vec3 targetPos = currentPos + remainingMove;

        // Check for collision
        ShapeCastHit hit;
        if (m_world->SphereCast(currentPos, m_desc.radius, normalize(remainingMove),
                                length(remainingMove), hit))
        {
            // Move to just before collision
            currentPos += remainingMove * (hit.fraction - 0.01f);

            // Slide along surface
            Vec3 wallNormal = hit.normal;
            remainingMove = remainingMove * (1.0f - hit.fraction);
            remainingMove = remainingMove - wallNormal * dot(remainingMove, wallNormal);
        }
        else
        {
            currentPos = targetPos;
            break;
        }
    }

    return currentPos;
}

inline bool CharacterController::TryStepUp(const Vec3& moveDir, float stepHeight, Vec3& outNewPosition)
{
    if (!m_world) return false;

    // Step 1: Move up
    Vec3 upPos = m_position + m_desc.upDirection * stepHeight;
    
    // Check if up position is clear
    ShapeCastHit hit;
    if (m_world->SphereCast(m_position, m_desc.radius, m_desc.upDirection, stepHeight, hit))
    {
        return false;  // Can't step up, ceiling in the way
    }

    // Step 2: Move forward at elevated position
    float moveLen = length(moveDir);
    if (moveLen < 0.001f) return false;

    Vec3 forwardPos = upPos + moveDir;
    if (m_world->SphereCast(upPos, m_desc.radius, normalize(moveDir), moveLen, hit))
    {
        return false;  // Still blocked
    }

    // Step 3: Move down to find new ground
    float downCastLength = stepHeight + m_desc.groundProbeDepth;
    if (m_world->SphereCast(forwardPos, m_desc.radius, -m_desc.upDirection, 
                            downCastLength, hit))
    {
        outNewPosition = forwardPos - m_desc.upDirection * (hit.fraction * downCastLength);
        return true;
    }

    return false;  // No ground found
}

inline void CharacterController::ApplyGravity(float deltaTime)
{
    if (!m_world) return;
    
    Vec3 gravity = m_world->GetGravity() * m_gravityScale;
    m_velocity += gravity * deltaTime;
}

inline Vec3 CharacterController::ProjectOntoGroundPlane(const Vec3& velocity)
{
    if (!m_groundState.grounded) return velocity;

    Vec3 normal = m_groundState.groundNormal;
    return velocity - normal * dot(velocity, normal);
}

inline bool CharacterController::IsWalkableSurface(const Vec3& normal)
{
    float cosAngle = dot(normal, m_desc.upDirection);
    float angle = std::acos(clamp(cosAngle, -1.0f, 1.0f));
    return angle <= m_desc.maxSlopeAngle;
}

} // namespace RVX::Physics
