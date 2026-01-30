/**
 * @file HingeConstraint.h
 * @brief Hinge (revolute) constraint - rotation around single axis
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"
#include <algorithm>

namespace RVX::Physics
{

/**
 * @brief Hinge constraint allows rotation around a single axis
 * 
 * Used for doors, wheels, and other rotating mechanisms.
 * Supports optional limits and motors.
 */
class HingeConstraint : public IConstraint
{
public:
    HingeConstraint() = default;

    /**
     * @brief Create hinge constraint
     * @param bodyA First body
     * @param bodyB Second body (can be null for world anchor)
     * @param worldAnchor World-space anchor point
     * @param worldAxis World-space hinge axis
     */
    HingeConstraint(RigidBody* bodyA, RigidBody* bodyB,
                    const Vec3& worldAnchor, const Vec3& worldAxis)
    {
        m_bodyA = bodyA;
        m_bodyB = bodyB;

        Vec3 axis = normalize(worldAxis);

        // Convert to local space
        if (bodyA)
        {
            Mat4 invA = inverse(bodyA->GetTransform());
            m_anchorA = Vec3(invA * Vec4(worldAnchor, 1.0f));
            m_axisA = normalize(Mat3(invA) * axis);
        }

        if (bodyB)
        {
            Mat4 invB = inverse(bodyB->GetTransform());
            m_anchorB = Vec3(invB * Vec4(worldAnchor, 1.0f));
            m_axisB = normalize(Mat3(invB) * axis);
        }
        else
        {
            m_anchorB = worldAnchor;
            m_axisB = axis;
        }

        // Compute initial angle reference
        ComputeReferenceFrame();
    }

    ConstraintType GetType() const override { return ConstraintType::Hinge; }
    const char* GetTypeName() const override { return "Hinge"; }

    // =========================================================================
    // Axis
    // =========================================================================

    /**
     * @brief Get current hinge angle in radians
     */
    float GetAngle() const { return m_currentAngle; }

    /**
     * @brief Get angular velocity around hinge axis
     */
    float GetAngularVelocity() const
    {
        Vec3 worldAxisA = Mat3(m_bodyA->GetTransform()) * m_axisA;
        Vec3 angVelA = m_bodyA->GetAngularVelocity();
        Vec3 angVelB = m_bodyB ? m_bodyB->GetAngularVelocity() : Vec3(0);
        return dot(angVelB - angVelA, worldAxisA);
    }

    // =========================================================================
    // Limits
    // =========================================================================

    /**
     * @brief Set angular limits
     */
    void SetLimits(float minAngle, float maxAngle)
    {
        m_limit.min = minAngle;
        m_limit.max = maxAngle;
        m_limit.enabled = true;
    }

    void EnableLimits(bool enable) { m_limit.enabled = enable; }
    bool AreLimitsEnabled() const { return m_limit.enabled; }

    float GetMinLimit() const { return m_limit.min; }
    float GetMaxLimit() const { return m_limit.max; }

    // =========================================================================
    // Motor
    // =========================================================================

    /**
     * @brief Set motor to target velocity
     */
    void SetMotorVelocity(float velocity, float maxTorque = FLT_MAX)
    {
        m_motor.enabled = true;
        m_motor.targetVelocity = velocity;
        m_motor.maxForce = maxTorque;
        m_motor.usePositionTarget = false;
    }

    /**
     * @brief Set motor to target position
     */
    void SetMotorPosition(float targetAngle, float maxTorque = FLT_MAX)
    {
        m_motor.enabled = true;
        m_motor.targetPosition = targetAngle;
        m_motor.maxForce = maxTorque;
        m_motor.usePositionTarget = true;
    }

    /**
     * @brief Disable motor
     */
    void DisableMotor() { m_motor.enabled = false; }
    bool IsMotorEnabled() const { return m_motor.enabled; }

    // =========================================================================
    // Solver
    // =========================================================================

    void PreSolve(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Compute world-space positions
        m_worldAnchorA = Vec3(m_bodyA->GetTransform() * Vec4(m_anchorA, 1.0f));
        m_worldAxisA = normalize(Mat3(m_bodyA->GetTransform()) * m_axisA);

        if (m_bodyB)
        {
            m_worldAnchorB = Vec3(m_bodyB->GetTransform() * Vec4(m_anchorB, 1.0f));
            m_worldAxisB = normalize(Mat3(m_bodyB->GetTransform()) * m_axisB);
        }
        else
        {
            m_worldAnchorB = m_anchorB;
            m_worldAxisB = m_axisB;
        }

        // Position error
        m_positionError = m_worldAnchorB - m_worldAnchorA;

        // Compute current angle
        ComputeCurrentAngle();

        // Compute orthogonal axes for angular constraints
        ComputeOrthogonalAxes();

        // Reset impulses for new solve
        m_accumulatedLinearImpulse = Vec3(0);
        m_accumulatedAngularImpulse = 0.0f;
        m_accumulatedLimitImpulse = 0.0f;
    }

    void SolveVelocity(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Solve linear constraint (anchor points)
        SolveLinearVelocity();

        // Solve angular constraints (lock 2 rotational DOFs)
        SolveAngularVelocity();

        // Solve motor
        if (m_motor.enabled)
        {
            SolveMotor(deltaTime);
        }

        // Solve limits
        if (m_limit.enabled)
        {
            SolveLimits();
        }
    }

    void SolvePosition(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        const float slop = 0.005f;
        const float baumgarte = 0.2f;

        // Position correction
        float errorLen = length(m_positionError);
        if (errorLen > slop)
        {
            float invMassA = m_bodyA->GetInverseMass();
            float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
            float totalInvMass = invMassA + invMassB;

            if (totalInvMass > 0.0f)
            {
                Vec3 correction = m_positionError * (baumgarte / totalInvMass);

                if (m_bodyA->IsDynamic())
                {
                    m_bodyA->SetPosition(m_bodyA->GetPosition() + correction * invMassA);
                }
                if (m_bodyB && m_bodyB->IsDynamic())
                {
                    m_bodyB->SetPosition(m_bodyB->GetPosition() - correction * invMassB);
                }
            }
        }
    }

    float GetAppliedImpulse() const override
    {
        return length(m_accumulatedLinearImpulse);
    }

    static Ptr Create(RigidBody* bodyA, RigidBody* bodyB,
                      const Vec3& worldAnchor, const Vec3& worldAxis)
    {
        return std::make_shared<HingeConstraint>(bodyA, bodyB, worldAnchor, worldAxis);
    }

private:
    void ComputeReferenceFrame()
    {
        // Compute initial rotation reference for angle calculation
        m_referenceAngle = 0.0f;
        ComputeCurrentAngle();
        m_referenceAngle = m_currentAngle;
    }

    void ComputeCurrentAngle()
    {
        // Use the cross product of perpendicular vectors to compute angle
        if (!m_bodyA) return;

        Vec3 perpA = GetPerpendicularVector(m_worldAxisA);
        Vec3 perpB = m_bodyB
            ? Mat3(m_bodyB->GetTransform()) * (Mat3(inverse(m_bodyA->GetTransform())) * perpA)
            : perpA;

        // Project onto plane perpendicular to axis
        perpB = perpB - m_worldAxisA * dot(perpB, m_worldAxisA);
        if (length(perpB) > 0.001f)
        {
            perpB = normalize(perpB);
            float cosAngle = clamp(dot(perpA, perpB), -1.0f, 1.0f);
            float sinAngle = dot(cross(perpA, perpB), m_worldAxisA);
            m_currentAngle = std::atan2(sinAngle, cosAngle) - m_referenceAngle;
        }
    }

    void ComputeOrthogonalAxes()
    {
        m_orthoAxis1 = GetPerpendicularVector(m_worldAxisA);
        m_orthoAxis2 = cross(m_worldAxisA, m_orthoAxis1);
    }

    Vec3 GetPerpendicularVector(const Vec3& v) const
    {
        Vec3 perp = (std::abs(v.x) < 0.9f) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        return normalize(cross(v, perp));
    }

    void SolveLinearVelocity()
    {
        Vec3 velA = m_bodyA->GetVelocityAtPoint(m_worldAnchorA);
        Vec3 velB = m_bodyB ? m_bodyB->GetVelocityAtPoint(m_worldAnchorB) : Vec3(0);
        Vec3 relVel = velB - velA;

        float invMassA = m_bodyA->GetInverseMass();
        float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
        float totalInvMass = invMassA + invMassB;

        if (totalInvMass > 0.0f)
        {
            Vec3 impulse = -relVel / totalInvMass;

            if (m_bodyA->IsDynamic())
            {
                m_bodyA->ApplyImpulse(-impulse);
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                m_bodyB->ApplyImpulse(impulse);
            }

            m_accumulatedLinearImpulse += impulse;
        }
    }

    void SolveAngularVelocity()
    {
        Vec3 angVelA = m_bodyA->GetAngularVelocity();
        Vec3 angVelB = m_bodyB ? m_bodyB->GetAngularVelocity() : Vec3(0);
        Vec3 relAngVel = angVelB - angVelA;

        // Remove velocity component around hinge axis (allowed)
        relAngVel -= m_worldAxisA * dot(relAngVel, m_worldAxisA);

        // Apply angular impulse to correct unwanted rotation
        float magnitude = length(relAngVel);
        if (magnitude > 0.001f)
        {
            Vec3 impulse = -relAngVel * 0.5f;  // Damped

            if (m_bodyA->IsDynamic())
            {
                m_bodyA->ApplyAngularImpulse(-impulse);
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                m_bodyB->ApplyAngularImpulse(impulse);
            }
        }
    }

    void SolveMotor(float deltaTime)
    {
        float angVel = GetAngularVelocity();
        float targetVel = m_motor.targetVelocity;

        if (m_motor.usePositionTarget)
        {
            // Position servo
            float error = m_motor.targetPosition - m_currentAngle;
            targetVel = error * 10.0f;  // P-gain
        }

        float velError = targetVel - angVel;
        float impulse = std::clamp(velError, -m_motor.maxForce * deltaTime, m_motor.maxForce * deltaTime);

        Vec3 angImpulse = m_worldAxisA * impulse;
        if (m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyAngularImpulse(-angImpulse);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyAngularImpulse(angImpulse);
        }
    }

    void SolveLimits()
    {
        float angVel = GetAngularVelocity();
        float impulse = 0.0f;

        if (m_currentAngle < m_limit.min)
        {
            // Hit lower limit
            impulse = std::max(0.0f, -angVel);
        }
        else if (m_currentAngle > m_limit.max)
        {
            // Hit upper limit
            impulse = std::min(0.0f, -angVel);
        }

        if (std::abs(impulse) > 0.0001f)
        {
            Vec3 angImpulse = m_worldAxisA * impulse;
            if (m_bodyA->IsDynamic())
            {
                m_bodyA->ApplyAngularImpulse(-angImpulse);
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                m_bodyB->ApplyAngularImpulse(angImpulse);
            }

            m_accumulatedLimitImpulse += impulse;
        }
    }

    Vec3 m_axisA{0, 1, 0};
    Vec3 m_axisB{0, 1, 0};
    Vec3 m_worldAnchorA{0};
    Vec3 m_worldAnchorB{0};
    Vec3 m_worldAxisA{0, 1, 0};
    Vec3 m_worldAxisB{0, 1, 0};
    Vec3 m_orthoAxis1{1, 0, 0};
    Vec3 m_orthoAxis2{0, 0, 1};
    Vec3 m_positionError{0};

    float m_referenceAngle = 0.0f;
    float m_currentAngle = 0.0f;

    ConstraintLimit m_limit;
    ConstraintMotor m_motor;

    Vec3 m_accumulatedLinearImpulse{0};
    float m_accumulatedAngularImpulse = 0.0f;
    float m_accumulatedLimitImpulse = 0.0f;
};

} // namespace RVX::Physics
