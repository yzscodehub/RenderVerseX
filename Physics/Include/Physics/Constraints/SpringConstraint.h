/**
 * @file SpringConstraint.h
 * @brief Spring-damper constraint
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"
#include <algorithm>
#include <cmath>

namespace RVX::Physics
{

/**
 * @brief Spring constraint with configurable stiffness and damping
 * 
 * Applies forces to maintain a rest length between two points.
 * Can be used for suspension systems, soft connections, and dynamic effects.
 */
class SpringConstraint : public IConstraint
{
public:
    SpringConstraint() = default;

    /**
     * @brief Create spring constraint
     * @param bodyA First body
     * @param bodyB Second body (can be null for world anchor)
     * @param anchorA Local anchor on body A
     * @param anchorB Local anchor on body B (or world position if bodyB is null)
     * @param restLength Natural length of spring
     * @param stiffness Spring stiffness (force per unit displacement)
     * @param damping Damping coefficient
     */
    SpringConstraint(RigidBody* bodyA, RigidBody* bodyB,
                     const Vec3& anchorA, const Vec3& anchorB,
                     float restLength, float stiffness, float damping)
        : m_restLength(restLength)
        , m_stiffness(stiffness)
        , m_damping(damping)
    {
        m_bodyA = bodyA;
        m_bodyB = bodyB;
        m_anchorA = anchorA;
        m_anchorB = anchorB;
    }

    /**
     * @brief Create spring with automatic rest length
     */
    static Ptr CreateAutoLength(RigidBody* bodyA, RigidBody* bodyB,
                                const Vec3& worldAnchorA, const Vec3& worldAnchorB,
                                float stiffness, float damping)
    {
        auto spring = std::make_shared<SpringConstraint>();
        spring->m_bodyA = bodyA;
        spring->m_bodyB = bodyB;
        spring->m_stiffness = stiffness;
        spring->m_damping = damping;

        // Convert to local space
        if (bodyA)
        {
            Mat4 invA = inverse(bodyA->GetTransform());
            spring->m_anchorA = Vec3(invA * Vec4(worldAnchorA, 1.0f));
        }
        else
        {
            spring->m_anchorA = worldAnchorA;
        }

        if (bodyB)
        {
            Mat4 invB = inverse(bodyB->GetTransform());
            spring->m_anchorB = Vec3(invB * Vec4(worldAnchorB, 1.0f));
        }
        else
        {
            spring->m_anchorB = worldAnchorB;
        }

        // Calculate rest length
        spring->m_restLength = length(worldAnchorB - worldAnchorA);

        return spring;
    }

    ConstraintType GetType() const override { return ConstraintType::Spring; }
    const char* GetTypeName() const override { return "Spring"; }

    // =========================================================================
    // Spring Properties
    // =========================================================================

    float GetRestLength() const { return m_restLength; }
    void SetRestLength(float length) { m_restLength = std::max(0.0f, length); }

    float GetStiffness() const { return m_stiffness; }
    void SetStiffness(float k) { m_stiffness = std::max(0.0f, k); }

    float GetDamping() const { return m_damping; }
    void SetDamping(float c) { m_damping = std::max(0.0f, c); }

    /**
     * @brief Get current spring length
     */
    float GetCurrentLength() const { return m_currentLength; }

    /**
     * @brief Get spring extension (positive = stretched)
     */
    float GetExtension() const { return m_currentLength - m_restLength; }

    /**
     * @brief Set spring to behave like a bungee (no compression force)
     */
    void SetBungeeMode(bool bungee) { m_bungeeMode = bungee; }
    bool IsBungeeMode() const { return m_bungeeMode; }

    /**
     * @brief Set length limits
     */
    void SetLengthLimits(float minLength, float maxLength)
    {
        m_minLength = std::max(0.0f, minLength);
        m_maxLength = maxLength;
        m_useLimits = true;
    }

    void DisableLengthLimits() { m_useLimits = false; }

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

        // Compute spring state
        Vec3 delta = m_worldAnchorB - m_worldAnchorA;
        m_currentLength = length(delta);

        if (m_currentLength > 0.0001f)
        {
            m_direction = delta / m_currentLength;
        }
        else
        {
            m_direction = Vec3(0, 1, 0);  // Default direction
        }
    }

    void SolveVelocity(float deltaTime) override
    {
        if (!m_enabled || m_broken) return;

        // Spring force (Hooke's law)
        float extension = m_currentLength - m_restLength;

        // Bungee mode: only pull, don't push
        if (m_bungeeMode && extension < 0)
        {
            extension = 0;
        }

        float springForce = m_stiffness * extension;

        // Damping force
        Vec3 velA = m_bodyA ? m_bodyA->GetVelocityAtPoint(m_worldAnchorA) : Vec3(0);
        Vec3 velB = m_bodyB ? m_bodyB->GetVelocityAtPoint(m_worldAnchorB) : Vec3(0);
        Vec3 relVel = velB - velA;
        float dampingForce = m_damping * dot(relVel, m_direction);

        // Total force
        float totalForce = springForce + dampingForce;
        Vec3 force = m_direction * totalForce;

        // Convert to impulse
        Vec3 impulse = force * deltaTime;

        // Apply impulses
        if (m_bodyA && m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyImpulseAtPoint(impulse, m_worldAnchorA);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyImpulseAtPoint(-impulse, m_worldAnchorB);
        }

        m_appliedForce = std::abs(totalForce);

        // Check for breaking
        if (m_breakingForce > 0.0f && m_appliedForce > m_breakingForce)
        {
            m_broken = true;
        }
    }

    void SolvePosition(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_useLimits) return;

        // Hard position limits
        bool needsCorrection = false;
        float targetLength = m_currentLength;

        if (m_currentLength < m_minLength)
        {
            targetLength = m_minLength;
            needsCorrection = true;
        }
        else if (m_currentLength > m_maxLength)
        {
            targetLength = m_maxLength;
            needsCorrection = true;
        }

        if (needsCorrection && m_currentLength > 0.0001f)
        {
            float correction = targetLength - m_currentLength;
            Vec3 correctionVec = m_direction * correction;

            float invMassA = m_bodyA ? m_bodyA->GetInverseMass() : 0.0f;
            float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
            float totalInvMass = invMassA + invMassB;

            if (totalInvMass > 0.0f)
            {
                if (m_bodyA && m_bodyA->IsDynamic())
                {
                    m_bodyA->SetPosition(m_bodyA->GetPosition() - correctionVec * (invMassA / totalInvMass));
                }
                if (m_bodyB && m_bodyB->IsDynamic())
                {
                    m_bodyB->SetPosition(m_bodyB->GetPosition() + correctionVec * (invMassB / totalInvMass));
                }
            }
        }
    }

    float GetAppliedImpulse() const override
    {
        return m_appliedForce;
    }

    static Ptr Create(RigidBody* bodyA, RigidBody* bodyB,
                      const Vec3& anchorA, const Vec3& anchorB,
                      float restLength, float stiffness, float damping)
    {
        return std::make_shared<SpringConstraint>(bodyA, bodyB, anchorA, anchorB,
                                                  restLength, stiffness, damping);
    }

private:
    float m_restLength = 1.0f;
    float m_stiffness = 100.0f;
    float m_damping = 10.0f;

    float m_minLength = 0.0f;
    float m_maxLength = FLT_MAX;
    bool m_useLimits = false;
    bool m_bungeeMode = false;

    Vec3 m_worldAnchorA{0};
    Vec3 m_worldAnchorB{0};
    Vec3 m_direction{0, 1, 0};
    float m_currentLength = 0.0f;
    float m_appliedForce = 0.0f;
};

} // namespace RVX::Physics
