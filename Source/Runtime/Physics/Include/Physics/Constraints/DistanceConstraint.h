/**
 * @file DistanceConstraint.h
 * @brief Distance (rope/rod) constraint
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"
#include <algorithm>
#include <cmath>

namespace RVX::Physics
{

/**
 * @brief Distance constraint maintains fixed distance between two points
 * 
 * Can be configured as:
 * - Rod: Maintains exact distance (both min and max)
 * - Rope: Prevents stretching beyond max distance (allows slack)
 */
class DistanceConstraint : public IConstraint
{
public:
    DistanceConstraint() = default;

    /**
     * @brief Create distance constraint
     * @param bodyA First body
     * @param bodyB Second body (can be null for world anchor)
     * @param anchorA Local anchor on body A
     * @param anchorB Local anchor on body B (or world position if bodyB is null)
     * @param distance Target distance
     */
    DistanceConstraint(RigidBody* bodyA, RigidBody* bodyB,
                       const Vec3& anchorA, const Vec3& anchorB,
                       float distance)
        : m_targetDistance(distance)
        , m_minDistance(distance)
        , m_maxDistance(distance)
    {
        m_bodyA = bodyA;
        m_bodyB = bodyB;
        m_anchorA = anchorA;
        m_anchorB = anchorB;
    }

    /**
     * @brief Create constraint with auto-calculated distance
     */
    static Ptr CreateFromWorldAnchors(RigidBody* bodyA, RigidBody* bodyB,
                                       const Vec3& worldAnchorA, const Vec3& worldAnchorB)
    {
        auto constraint = std::make_shared<DistanceConstraint>();
        constraint->m_bodyA = bodyA;
        constraint->m_bodyB = bodyB;

        // Convert to local space
        if (bodyA)
        {
            Mat4 invA = inverse(bodyA->GetTransform());
            constraint->m_anchorA = Vec3(invA * Vec4(worldAnchorA, 1.0f));
        }
        else
        {
            constraint->m_anchorA = worldAnchorA;
        }

        if (bodyB)
        {
            Mat4 invB = inverse(bodyB->GetTransform());
            constraint->m_anchorB = Vec3(invB * Vec4(worldAnchorB, 1.0f));
        }
        else
        {
            constraint->m_anchorB = worldAnchorB;
        }

        // Calculate distance
        float dist = length(worldAnchorB - worldAnchorA);
        constraint->m_targetDistance = dist;
        constraint->m_minDistance = dist;
        constraint->m_maxDistance = dist;

        return constraint;
    }

    ConstraintType GetType() const override { return ConstraintType::Distance; }
    const char* GetTypeName() const override { return "Distance"; }

    // =========================================================================
    // Distance Properties
    // =========================================================================

    /**
     * @brief Get target distance
     */
    float GetDistance() const { return m_targetDistance; }

    /**
     * @brief Set exact distance (rod mode)
     */
    void SetDistance(float distance)
    {
        m_targetDistance = std::max(0.0f, distance);
        m_minDistance = m_targetDistance;
        m_maxDistance = m_targetDistance;
    }

    /**
     * @brief Set distance range (rope mode if min < max)
     */
    void SetDistanceRange(float minDist, float maxDist)
    {
        m_minDistance = std::max(0.0f, minDist);
        m_maxDistance = std::max(m_minDistance, maxDist);
        m_targetDistance = (m_minDistance + m_maxDistance) * 0.5f;
    }

    float GetMinDistance() const { return m_minDistance; }
    float GetMaxDistance() const { return m_maxDistance; }

    /**
     * @brief Get current distance
     */
    float GetCurrentDistance() const { return m_currentDistance; }

    /**
     * @brief Set constraint stiffness (0 = rigid, higher = softer)
     */
    void SetStiffness(float stiffness) { m_stiffness = std::max(0.0f, stiffness); }
    float GetStiffness() const { return m_stiffness; }

    // =========================================================================
    // Solver
    // =========================================================================

    void PreSolve(float deltaTime) override
    {
        if (!m_enabled || m_broken) return;

        // Compute world-space anchor positions
        if (m_bodyA)
        {
            m_worldAnchorA = Vec3(m_bodyA->GetTransform() * Vec4(m_anchorA, 1.0f));
        }
        else
        {
            m_worldAnchorA = m_anchorA;
        }

        if (m_bodyB)
        {
            m_worldAnchorB = Vec3(m_bodyB->GetTransform() * Vec4(m_anchorB, 1.0f));
        }
        else
        {
            m_worldAnchorB = m_anchorB;
        }

        // Compute current state
        Vec3 delta = m_worldAnchorB - m_worldAnchorA;
        m_currentDistance = length(delta);

        if (m_currentDistance > 0.0001f)
        {
            m_direction = delta / m_currentDistance;
        }
        else
        {
            m_direction = Vec3(0, 1, 0);
        }

        // Compute effective mass
        float invMassA = m_bodyA ? m_bodyA->GetInverseMass() : 0.0f;
        float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
        m_effectiveMass = invMassA + invMassB;

        if (m_effectiveMass > 0.0f)
        {
            m_effectiveMass = 1.0f / m_effectiveMass;
        }

        // Warm start
        if (m_warmStartEnabled)
        {
            ApplyImpulse(m_accumulatedImpulse);
        }
        else
        {
            m_accumulatedImpulse = 0.0f;
        }
    }

    void SolveVelocity(float deltaTime) override
    {
        if (!m_enabled || m_broken) return;
        if (m_effectiveMass <= 0.0f) return;

        // Check if constraint is active
        bool tooShort = m_currentDistance < m_minDistance;
        bool tooLong = m_currentDistance > m_maxDistance;

        if (!tooShort && !tooLong)
        {
            return;  // Within range, no constraint needed
        }

        // Get relative velocity along constraint axis
        Vec3 velA = m_bodyA ? m_bodyA->GetVelocityAtPoint(m_worldAnchorA) : Vec3(0);
        Vec3 velB = m_bodyB ? m_bodyB->GetVelocityAtPoint(m_worldAnchorB) : Vec3(0);
        float relVel = dot(velB - velA, m_direction);

        // Compute impulse
        float impulse = 0.0f;

        if (tooLong)
        {
            // Pull together (rope mode - only if stretching)
            impulse = -relVel * m_effectiveMass;
            
            // Only apply if pulling apart
            float newImpulse = m_accumulatedImpulse + impulse;
            newImpulse = std::max(newImpulse, 0.0f);  // Can only pull, not push
            impulse = newImpulse - m_accumulatedImpulse;
        }
        else if (tooShort)
        {
            // Push apart
            impulse = -relVel * m_effectiveMass;
            
            // Only apply if compressing
            float newImpulse = m_accumulatedImpulse + impulse;
            newImpulse = std::min(newImpulse, 0.0f);  // Can only push, not pull
            impulse = newImpulse - m_accumulatedImpulse;
        }

        m_accumulatedImpulse += impulse;
        ApplyImpulse(impulse);

        // Check for breaking
        if (m_breakingForce > 0.0f)
        {
            float appliedForce = std::abs(m_accumulatedImpulse) / deltaTime;
            if (appliedForce > m_breakingForce)
            {
                m_broken = true;
            }
        }
    }

    void SolvePosition(float deltaTime) override
    {
        if (!m_enabled || m_broken) return;

        const float slop = 0.005f;
        const float baumgarte = 0.2f;

        // Recompute current distance
        Vec3 delta = m_worldAnchorB - m_worldAnchorA;
        float dist = length(delta);
        
        if (dist < 0.0001f) return;

        Vec3 dir = delta / dist;

        // Compute error
        float error = 0.0f;
        if (dist < m_minDistance)
        {
            error = m_minDistance - dist;
        }
        else if (dist > m_maxDistance)
        {
            error = m_maxDistance - dist;
        }

        if (std::abs(error) > slop)
        {
            float invMassA = m_bodyA ? m_bodyA->GetInverseMass() : 0.0f;
            float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
            float totalInvMass = invMassA + invMassB;

            if (totalInvMass > 0.0f)
            {
                // Apply soft constraint
                float softness = 1.0f / (1.0f + m_stiffness);
                Vec3 correction = dir * (error * baumgarte * softness);

                if (m_bodyA && m_bodyA->IsDynamic())
                {
                    m_bodyA->SetPosition(m_bodyA->GetPosition() - correction * (invMassA / totalInvMass));
                }
                if (m_bodyB && m_bodyB->IsDynamic())
                {
                    m_bodyB->SetPosition(m_bodyB->GetPosition() + correction * (invMassB / totalInvMass));
                }
            }
        }
    }

    float GetAppliedImpulse() const override
    {
        return std::abs(m_accumulatedImpulse);
    }

    void SetWarmStartEnabled(bool enabled) { m_warmStartEnabled = enabled; }

    static Ptr Create(RigidBody* bodyA, RigidBody* bodyB,
                      const Vec3& anchorA, const Vec3& anchorB,
                      float distance)
    {
        return std::make_shared<DistanceConstraint>(bodyA, bodyB, anchorA, anchorB, distance);
    }

private:
    void ApplyImpulse(float impulse)
    {
        Vec3 linearImpulse = m_direction * impulse;

        if (m_bodyA && m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyImpulseAtPoint(-linearImpulse, m_worldAnchorA);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyImpulseAtPoint(linearImpulse, m_worldAnchorB);
        }
    }

    float m_targetDistance = 1.0f;
    float m_minDistance = 1.0f;
    float m_maxDistance = 1.0f;
    float m_stiffness = 0.0f;  // 0 = rigid

    Vec3 m_worldAnchorA{0};
    Vec3 m_worldAnchorB{0};
    Vec3 m_direction{0, 1, 0};
    float m_currentDistance = 0.0f;
    float m_effectiveMass = 0.0f;
    float m_accumulatedImpulse = 0.0f;
    bool m_warmStartEnabled = true;
};

} // namespace RVX::Physics
