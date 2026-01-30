/**
 * @file SliderConstraint.h
 * @brief Slider (prismatic) constraint - translation along single axis
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"
#include <algorithm>

namespace RVX::Physics
{

/**
 * @brief Slider constraint allows translation along a single axis
 * 
 * Used for pistons, sliding doors, and linear actuators.
 * Supports optional limits and motors.
 */
class SliderConstraint : public IConstraint
{
public:
    SliderConstraint() = default;

    /**
     * @brief Create slider constraint
     * @param bodyA First body
     * @param bodyB Second body (can be null for world anchor)
     * @param worldAnchor World-space anchor point
     * @param worldAxis World-space slide axis
     */
    SliderConstraint(RigidBody* bodyA, RigidBody* bodyB,
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
            m_initialRotationA = bodyA->GetRotation();
        }

        if (bodyB)
        {
            Mat4 invB = inverse(bodyB->GetTransform());
            m_anchorB = Vec3(invB * Vec4(worldAnchor, 1.0f));
            m_axisB = normalize(Mat3(invB) * axis);
            m_initialRotationB = bodyB->GetRotation();
        }
        else
        {
            m_anchorB = worldAnchor;
            m_axisB = axis;
            m_initialRotationB = Quat(1, 0, 0, 0);
        }

        m_initialPosition = 0.0f;
    }

    ConstraintType GetType() const override { return ConstraintType::Slider; }
    const char* GetTypeName() const override { return "Slider"; }

    // =========================================================================
    // Position
    // =========================================================================

    /**
     * @brief Get current position along slide axis
     */
    float GetPosition() const { return m_currentPosition; }

    /**
     * @brief Get velocity along slide axis
     */
    float GetVelocity() const
    {
        Vec3 velA = m_bodyA ? m_bodyA->GetLinearVelocity() : Vec3(0);
        Vec3 velB = m_bodyB ? m_bodyB->GetLinearVelocity() : Vec3(0);
        return dot(velB - velA, m_worldAxis);
    }

    // =========================================================================
    // Limits
    // =========================================================================

    /**
     * @brief Set position limits
     */
    void SetLimits(float minPos, float maxPos)
    {
        m_limit.min = minPos;
        m_limit.max = maxPos;
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
    void SetMotorVelocity(float velocity, float maxForce = FLT_MAX)
    {
        m_motor.enabled = true;
        m_motor.targetVelocity = velocity;
        m_motor.maxForce = maxForce;
        m_motor.usePositionTarget = false;
    }

    /**
     * @brief Set motor to target position
     */
    void SetMotorPosition(float targetPos, float maxForce = FLT_MAX)
    {
        m_motor.enabled = true;
        m_motor.targetPosition = targetPos;
        m_motor.maxForce = maxForce;
        m_motor.usePositionTarget = true;
    }

    void DisableMotor() { m_motor.enabled = false; }
    bool IsMotorEnabled() const { return m_motor.enabled; }

    // =========================================================================
    // Solver
    // =========================================================================

    void PreSolve(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Compute world-space values
        m_worldAnchorA = Vec3(m_bodyA->GetTransform() * Vec4(m_anchorA, 1.0f));
        m_worldAxis = normalize(Mat3(m_bodyA->GetTransform()) * m_axisA);

        if (m_bodyB)
        {
            m_worldAnchorB = Vec3(m_bodyB->GetTransform() * Vec4(m_anchorB, 1.0f));
        }
        else
        {
            m_worldAnchorB = m_anchorB;
        }

        // Compute current position
        Vec3 diff = m_worldAnchorB - m_worldAnchorA;
        m_currentPosition = dot(diff, m_worldAxis) - m_initialPosition;

        // Compute lateral error (perpendicular to axis)
        m_lateralError = diff - m_worldAxis * dot(diff, m_worldAxis);

        // Compute orthogonal axes
        m_orthoAxis1 = GetPerpendicularVector(m_worldAxis);
        m_orthoAxis2 = cross(m_worldAxis, m_orthoAxis1);

        // Reset accumulators
        m_accumulatedImpulse = Vec3(0);
    }

    void SolveVelocity(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        // Get relative velocity
        Vec3 velA = m_bodyA->GetVelocityAtPoint(m_worldAnchorA);
        Vec3 velB = m_bodyB ? m_bodyB->GetVelocityAtPoint(m_worldAnchorB) : Vec3(0);
        Vec3 relVel = velB - velA;

        float invMassA = m_bodyA->GetInverseMass();
        float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
        float totalInvMass = invMassA + invMassB;

        if (totalInvMass <= 0.0f) return;

        // Constrain lateral motion (perpendicular to axis)
        float lateralVel1 = dot(relVel, m_orthoAxis1);
        float lateralVel2 = dot(relVel, m_orthoAxis2);

        Vec3 lateralImpulse = -(m_orthoAxis1 * lateralVel1 + m_orthoAxis2 * lateralVel2) / totalInvMass;

        if (m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyImpulse(-lateralImpulse);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyImpulse(lateralImpulse);
        }

        // Lock rotation
        SolveAngularVelocity();

        // Motor
        if (m_motor.enabled)
        {
            SolveMotor(deltaTime, totalInvMass);
        }

        // Limits
        if (m_limit.enabled)
        {
            SolveLimits(totalInvMass);
        }

        m_accumulatedImpulse += lateralImpulse;
    }

    void SolvePosition(float deltaTime) override
    {
        if (!m_enabled || m_broken || !m_bodyA) return;

        const float slop = 0.005f;
        const float baumgarte = 0.2f;

        // Correct lateral error
        float errorLen = length(m_lateralError);
        if (errorLen > slop)
        {
            float invMassA = m_bodyA->GetInverseMass();
            float invMassB = m_bodyB ? m_bodyB->GetInverseMass() : 0.0f;
            float totalInvMass = invMassA + invMassB;

            if (totalInvMass > 0.0f)
            {
                Vec3 correction = m_lateralError * (baumgarte / totalInvMass);

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
        return length(m_accumulatedImpulse);
    }

    static Ptr Create(RigidBody* bodyA, RigidBody* bodyB,
                      const Vec3& worldAnchor, const Vec3& worldAxis)
    {
        return std::make_shared<SliderConstraint>(bodyA, bodyB, worldAnchor, worldAxis);
    }

private:
    Vec3 GetPerpendicularVector(const Vec3& v) const
    {
        Vec3 perp = (std::abs(v.x) < 0.9f) ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        return normalize(cross(v, perp));
    }

    void SolveAngularVelocity()
    {
        Vec3 angVelA = m_bodyA->GetAngularVelocity();
        Vec3 angVelB = m_bodyB ? m_bodyB->GetAngularVelocity() : Vec3(0);
        Vec3 relAngVel = angVelB - angVelA;

        // Lock all rotation
        if (length(relAngVel) > 0.001f)
        {
            Vec3 impulse = -relAngVel * 0.5f;

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

    void SolveMotor(float deltaTime, float totalInvMass)
    {
        float vel = GetVelocity();
        float targetVel = m_motor.targetVelocity;

        if (m_motor.usePositionTarget)
        {
            float error = m_motor.targetPosition - m_currentPosition;
            targetVel = error * 10.0f;  // P-gain
        }

        float velError = targetVel - vel;
        float maxImpulse = m_motor.maxForce * deltaTime;
        float impulse = std::clamp(velError / totalInvMass, -maxImpulse, maxImpulse);

        Vec3 linearImpulse = m_worldAxis * impulse;
        if (m_bodyA->IsDynamic())
        {
            m_bodyA->ApplyImpulse(-linearImpulse);
        }
        if (m_bodyB && m_bodyB->IsDynamic())
        {
            m_bodyB->ApplyImpulse(linearImpulse);
        }
    }

    void SolveLimits(float totalInvMass)
    {
        float vel = GetVelocity();
        float impulse = 0.0f;

        if (m_currentPosition < m_limit.min)
        {
            impulse = std::max(0.0f, -vel / totalInvMass);
        }
        else if (m_currentPosition > m_limit.max)
        {
            impulse = std::min(0.0f, -vel / totalInvMass);
        }

        if (std::abs(impulse) > 0.0001f)
        {
            Vec3 linearImpulse = m_worldAxis * impulse;
            if (m_bodyA->IsDynamic())
            {
                m_bodyA->ApplyImpulse(-linearImpulse);
            }
            if (m_bodyB && m_bodyB->IsDynamic())
            {
                m_bodyB->ApplyImpulse(linearImpulse);
            }
        }
    }

    Vec3 m_axisA{1, 0, 0};
    Vec3 m_axisB{1, 0, 0};
    Vec3 m_worldAnchorA{0};
    Vec3 m_worldAnchorB{0};
    Vec3 m_worldAxis{1, 0, 0};
    Vec3 m_orthoAxis1{0, 1, 0};
    Vec3 m_orthoAxis2{0, 0, 1};
    Vec3 m_lateralError{0};

    Quat m_initialRotationA{1, 0, 0, 0};
    Quat m_initialRotationB{1, 0, 0, 0};
    float m_initialPosition = 0.0f;
    float m_currentPosition = 0.0f;

    ConstraintLimit m_limit;
    ConstraintMotor m_motor;

    Vec3 m_accumulatedImpulse{0};
};

} // namespace RVX::Physics
