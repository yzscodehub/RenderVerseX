/**
 * @file IConstraint.h
 * @brief Base interface for physics constraints
 */

#pragma once

#include "Physics/PhysicsTypes.h"
#include <memory>

namespace RVX::Physics
{

class RigidBody;

/**
 * @brief Constraint type enumeration
 */
enum class ConstraintType : uint8
{
    Fixed,          ///< Bodies locked together
    Hinge,          ///< Rotation around single axis
    Slider,         ///< Translation along single axis
    Spring,         ///< Spring-damper constraint
    Distance,       ///< Fixed distance between points
    Cone,           ///< Limited rotation cone
    Ball,           ///< Ball-and-socket joint
    Generic         ///< Custom 6-DOF constraint
};

/**
 * @brief Constraint limits for rotational/translational motion
 */
struct ConstraintLimit
{
    float min = -3.14159265f;
    float max = 3.14159265f;
    bool enabled = false;
    float stiffness = 0.0f;     ///< Soft limit stiffness (0 = hard limit)
    float damping = 0.0f;       ///< Soft limit damping

    static ConstraintLimit Free() { return ConstraintLimit{-3.14159265f, 3.14159265f, false}; }
    static ConstraintLimit Locked() { return ConstraintLimit{0.0f, 0.0f, true}; }
    static ConstraintLimit Range(float minVal, float maxVal) { return ConstraintLimit{minVal, maxVal, true}; }
};

/**
 * @brief Motor settings for powered constraints
 */
struct ConstraintMotor
{
    bool enabled = false;
    float targetVelocity = 0.0f;
    float targetPosition = 0.0f;
    float maxForce = FLT_MAX;
    bool usePositionTarget = false;

    static ConstraintMotor Off() { return ConstraintMotor{}; }
    static ConstraintMotor Velocity(float vel, float maxF = FLT_MAX)
    {
        ConstraintMotor m;
        m.enabled = true;
        m.targetVelocity = vel;
        m.maxForce = maxF;
        return m;
    }
    static ConstraintMotor Position(float pos, float maxF = FLT_MAX)
    {
        ConstraintMotor m;
        m.enabled = true;
        m.targetPosition = pos;
        m.maxForce = maxF;
        m.usePositionTarget = true;
        return m;
    }
};

/**
 * @brief Base class for all physics constraints
 * 
 * Constraints connect two rigid bodies and restrict their relative motion.
 * Some constraints (like distance or spring) can also connect a body to a
 * fixed point in world space.
 */
class IConstraint
{
public:
    using Ptr = std::shared_ptr<IConstraint>;

    virtual ~IConstraint() = default;

    // =========================================================================
    // Type Info
    // =========================================================================

    /**
     * @brief Get constraint type
     */
    virtual ConstraintType GetType() const = 0;

    /**
     * @brief Get constraint type name for debugging
     */
    virtual const char* GetTypeName() const = 0;

    // =========================================================================
    // Bodies
    // =========================================================================

    /**
     * @brief Get the first connected body
     */
    RigidBody* GetBodyA() const { return m_bodyA; }

    /**
     * @brief Get the second connected body (may be null for world anchor)
     */
    RigidBody* GetBodyB() const { return m_bodyB; }

    /**
     * @brief Get anchor point on body A (local space)
     */
    const Vec3& GetAnchorA() const { return m_anchorA; }

    /**
     * @brief Get anchor point on body B (local space) or world space if bodyB is null
     */
    const Vec3& GetAnchorB() const { return m_anchorB; }

    /**
     * @brief Set anchor points
     */
    void SetAnchors(const Vec3& anchorA, const Vec3& anchorB)
    {
        m_anchorA = anchorA;
        m_anchorB = anchorB;
    }

    // =========================================================================
    // State
    // =========================================================================

    /**
     * @brief Check if constraint is enabled
     */
    bool IsEnabled() const { return m_enabled; }

    /**
     * @brief Enable or disable the constraint
     */
    void SetEnabled(bool enabled) { m_enabled = enabled; }

    /**
     * @brief Get constraint ID
     */
    uint64 GetId() const { return m_id; }
    void SetId(uint64 id) { m_id = id; }

    // =========================================================================
    // Parameters
    // =========================================================================

    /**
     * @brief Breaking force threshold (0 = unbreakable)
     */
    float GetBreakingForce() const { return m_breakingForce; }
    void SetBreakingForce(float force) { m_breakingForce = force; }

    /**
     * @brief Check if constraint has been broken
     */
    bool IsBroken() const { return m_broken; }

    // =========================================================================
    // Solver Interface
    // =========================================================================

    /**
     * @brief Prepare constraint for solving (called once per step)
     */
    virtual void PreSolve(float deltaTime) = 0;

    /**
     * @brief Solve velocity constraints
     */
    virtual void SolveVelocity(float deltaTime) = 0;

    /**
     * @brief Solve position constraints (for stability)
     */
    virtual void SolvePosition(float deltaTime) = 0;

    /**
     * @brief Get the applied impulse magnitude (for breaking)
     */
    virtual float GetAppliedImpulse() const { return 0.0f; }

protected:
    RigidBody* m_bodyA = nullptr;
    RigidBody* m_bodyB = nullptr;
    Vec3 m_anchorA{0};
    Vec3 m_anchorB{0};
    uint64 m_id = 0;
    bool m_enabled = true;
    bool m_broken = false;
    float m_breakingForce = 0.0f;
};

} // namespace RVX::Physics
