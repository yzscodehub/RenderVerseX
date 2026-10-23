/**
 * @file FixedConstraint.h
 * @brief Fixed (weld) constraint - locks bodies together
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"

namespace RVX::Physics
{

/**
 * @brief Fixed constraint locks two bodies together
 * 
 * No relative motion is allowed between the bodies.
 * Useful for creating compound objects from multiple bodies.
 */
class FixedConstraint : public IConstraint
{
public:
    FixedConstraint() = default;

    /**
     * @brief Create fixed constraint between two bodies
     * @param bodyA First body
     * @param bodyB Second body (can be null for world anchor)
     * @param worldAnchor World-space anchor point
     */
    FixedConstraint(RigidBody* bodyA, RigidBody* bodyB, const Vec3& worldAnchor)
    {
        m_bodyA = bodyA;
        m_bodyB = bodyB;

        // Convert world anchor to local space
        if (bodyA)
        {
            Mat4 invA = inverse(bodyA->GetTransform());
            m_anchorA = Vec3(invA * Vec4(worldAnchor, 1.0f));
        }

        if (bodyB)
        {
            Mat4 invB = inverse(bodyB->GetTransform());
            m_anchorB = Vec3(invB * Vec4(worldAnchor, 1.0f));
            m_initialRelativeRotation = normalize(
                glm::conjugate(bodyA->GetRotation()) * bodyB->GetRotation());
        }
        else
        {
            m_anchorB = worldAnchor;
            m_initialRelativeRotation = bodyA ? glm::conjugate(bodyA->GetRotation()) : Quat(1, 0, 0, 0);
        }
    }

    ConstraintType GetType() const override { return ConstraintType::Fixed; }
    const char* GetTypeName() const override { return "Fixed"; }

    void PreSolve(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Compute world-space anchor positions
        m_worldAnchorA = Vec3(m_bodyA->GetTransform() * Vec4(m_anchorA, 1.0f));
        
        if (m_bodyB)
        {
            m_worldAnchorB = Vec3(m_bodyB->GetTransform() * Vec4(m_anchorB, 1.0f));
        }
        else
        {
            m_worldAnchorB = m_anchorB;
        }

        // Compute position error
        m_positionError = m_worldAnchorB - m_worldAnchorA;

        // Compute rotation error
        Quat currentRelRot;
        if (m_bodyB)
        {
            currentRelRot = glm::conjugate(m_bodyA->GetRotation()) * m_bodyB->GetRotation();
        }
        else
        {
            currentRelRot = glm::conjugate(m_bodyA->GetRotation());
        }

        Quat rotError = currentRelRot * glm::conjugate(m_initialRelativeRotation);
        if (rotError.w < 0) rotError = -rotError;  // Take shorter path
        m_rotationError = Vec3(rotError.x, rotError.y, rotError.z) * 2.0f;

        // Compute effective mass
        ComputeEffectiveMass();

        // Warm starting
        if (m_warmStartEnabled)
        {
            ApplyImpulse(m_accumulatedLinearImpulse, m_accumulatedAngularImpulse);
        }
        else
        {
            m_accumulatedLinearImpulse = Vec3(0);
            m_accumulatedAngularImpulse = Vec3(0);
        }
    }

    void SolveVelocity(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Get relative velocity at anchor points
        Vec3 velA = m_bodyA->GetVelocityAtPoint(m_worldAnchorA);
        Vec3 velB = m_bodyB ? m_bodyB->GetVelocityAtPoint(m_worldAnchorB) : Vec3(0);
        Vec3 relVel = velB - velA;

        // Linear constraint
        Vec3 linearImpulse = m_linearMass * (-relVel);

        // Angular constraint
        Vec3 angVelA = m_bodyA->GetAngularVelocity();
        Vec3 angVelB = m_bodyB ? m_bodyB->GetAngularVelocity() : Vec3(0);
        Vec3 relAngVel = angVelB - angVelA;

        Vec3 angularImpulse = m_angularMass * (-relAngVel);

        // Apply and accumulate
        ApplyImpulse(linearImpulse, angularImpulse);
        m_accumulatedLinearImpulse += linearImpulse;
        m_accumulatedAngularImpulse += angularImpulse;

        // Check for breaking
        if (m_breakingForce > 0.0f)
        {
            float impulse = length(m_accumulatedLinearImpulse) / deltaTime;
            if (impulse > m_breakingForce)
            {
                m_broken = true;
            }
        }
    }

    void SolvePosition(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        const float slop = 0.005f;  // Allowable error
        const float baumgarte = 0.2f;

        // Position correction
        float errorLen = length(m_positionError);
        if (errorLen > slop)
        {
            Vec3 correction = m_linearMass * (m_positionError * baumgarte);
            
            if (m_bodyA->IsDynamic())
            {
                m_bodyA->SetPosition(m_bodyA->GetPosition() + correction * m_bodyA->GetInverseMass());
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                m_bodyB->SetPosition(m_bodyB->GetPosition() - correction * m_bodyB->GetInverseMass());
            }
        }

        // Rotation correction
        float rotErrorLen = length(m_rotationError);
        if (rotErrorLen > slop)
        {
            Vec3 angCorrection = m_angularMass * (m_rotationError * baumgarte);
            
            if (m_bodyA->IsDynamic())
            {
                Quat dq = Quat(0, angCorrection.x * 0.5f, angCorrection.y * 0.5f, angCorrection.z * 0.5f);
                m_bodyA->SetRotation(normalize(m_bodyA->GetRotation() + dq * m_bodyA->GetRotation()));
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                Quat dq = Quat(0, -angCorrection.x * 0.5f, -angCorrection.y * 0.5f, -angCorrection.z * 0.5f);
                m_bodyB->SetRotation(normalize(m_bodyB->GetRotation() + dq * m_bodyB->GetRotation()));
            }
        }
    }

    float GetAppliedImpulse() const override
    {
        return length(m_accumulatedLinearImpulse);
    }

    /**
     * @brief Enable/disable warm starting
     */
    void SetWarmStartEnabled(bool enabled) { m_warmStartEnabled = enabled; }

    static Ptr Create(RigidBody* bodyA, RigidBody* bodyB, const Vec3& worldAnchor)
    {
        return std::make_shared<FixedConstraint>(bodyA, bodyB, worldAnchor);
    }

private:
    void ComputeEffectiveMass()
    {
        float invMassA = m_bodyA ? m_bodyA->GetInverseMass() : 0.0f;
        float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
        float totalInvMass = invMassA + invMassB;

        if (totalInvMass > 0.0f)
        {
            m_linearMass = Mat3(1.0f / totalInvMass);
        }
        else
        {
            m_linearMass = Mat3(0.0f);
        }

        // Simplified angular mass (identity scaled by inertia)
        m_angularMass = Mat3(1.0f);  // Would need full inertia tensors for accurate calculation
    }

    void ApplyImpulse(const Vec3& linearImpulse, const Vec3& angularImpulse)
    {
        if (m_bodyA && m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyImpulse(-linearImpulse);
            m_bodyA->ApplyAngularImpulse(-angularImpulse);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyImpulse(linearImpulse);
            m_bodyB->ApplyAngularImpulse(angularImpulse);
        }
    }

    Quat m_initialRelativeRotation{1, 0, 0, 0};
    Vec3 m_worldAnchorA{0};
    Vec3 m_worldAnchorB{0};
    Vec3 m_positionError{0};
    Vec3 m_rotationError{0};
    Mat3 m_linearMass{1.0f};
    Mat3 m_angularMass{1.0f};
    Vec3 m_accumulatedLinearImpulse{0};
    Vec3 m_accumulatedAngularImpulse{0};
    bool m_warmStartEnabled = true;
};

} // namespace RVX::Physics
