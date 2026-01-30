/**
 * @file Constraints.h
 * @brief Unified header for all constraint types
 */

#pragma once

#include "Physics/Constraints/IConstraint.h"
#include "Physics/Constraints/FixedConstraint.h"
#include "Physics/Constraints/HingeConstraint.h"
#include "Physics/Constraints/SliderConstraint.h"
#include "Physics/Constraints/SpringConstraint.h"
#include "Physics/Constraints/DistanceConstraint.h"

namespace RVX::Physics
{

/**
 * @brief Factory for creating common constraint configurations
 */
class ConstraintFactory
{
public:
    /**
     * @brief Create a fixed (weld) joint between two bodies at their current positions
     */
    static FixedConstraint::Ptr CreateWeldJoint(RigidBody* bodyA, RigidBody* bodyB)
    {
        Vec3 midpoint = (bodyA->GetPosition() + (bodyB ? bodyB->GetPosition() : bodyA->GetPosition())) * 0.5f;
        return FixedConstraint::Create(bodyA, bodyB, midpoint);
    }

    /**
     * @brief Create a hinge joint with limits (e.g., for a door)
     */
    static HingeConstraint::Ptr CreateDoorHinge(RigidBody* door, RigidBody* frame,
                                                const Vec3& hingePoint, const Vec3& hingeAxis,
                                                float minAngle = 0.0f, float maxAngle = 1.57f)
    {
        auto hinge = HingeConstraint::Create(door, frame, hingePoint, hingeAxis);
        auto hingePtr = std::dynamic_pointer_cast<HingeConstraint>(hinge);
        if (hingePtr)
        {
            hingePtr->SetLimits(minAngle, maxAngle);
        }
        return hingePtr;
    }

    /**
     * @brief Create a wheel joint (motor-driven hinge)
     */
    static HingeConstraint::Ptr CreateWheelJoint(RigidBody* wheel, RigidBody* chassis,
                                                  const Vec3& axlePoint, const Vec3& axleAxis,
                                                  float motorSpeed = 0.0f, float maxTorque = 100.0f)
    {
        auto hinge = HingeConstraint::Create(wheel, chassis, axlePoint, axleAxis);
        auto hingePtr = std::dynamic_pointer_cast<HingeConstraint>(hinge);
        if (hingePtr && motorSpeed != 0.0f)
        {
            hingePtr->SetMotorVelocity(motorSpeed, maxTorque);
        }
        return hingePtr;
    }

    /**
     * @brief Create a piston joint (motor-driven slider)
     */
    static SliderConstraint::Ptr CreatePistonJoint(RigidBody* piston, RigidBody* cylinder,
                                                    const Vec3& anchor, const Vec3& axis,
                                                    float minPos, float maxPos)
    {
        auto slider = SliderConstraint::Create(piston, cylinder, anchor, axis);
        auto sliderPtr = std::dynamic_pointer_cast<SliderConstraint>(slider);
        if (sliderPtr)
        {
            sliderPtr->SetLimits(minPos, maxPos);
        }
        return sliderPtr;
    }

    /**
     * @brief Create a suspension spring
     */
    static SpringConstraint::Ptr CreateSuspension(RigidBody* wheel, RigidBody* chassis,
                                                   const Vec3& wheelAnchor, const Vec3& chassisAnchor,
                                                   float restLength, float stiffness, float damping)
    {
        return SpringConstraint::Create(wheel, chassis, wheelAnchor, chassisAnchor,
                                        restLength, stiffness, damping);
    }

    /**
     * @brief Create a rope constraint (allows slack)
     */
    static DistanceConstraint::Ptr CreateRope(RigidBody* bodyA, RigidBody* bodyB,
                                               const Vec3& anchorA, const Vec3& anchorB,
                                               float maxLength)
    {
        auto rope = std::make_shared<DistanceConstraint>(bodyA, bodyB, anchorA, anchorB, maxLength);
        rope->SetDistanceRange(0.0f, maxLength);
        return rope;
    }

    /**
     * @brief Create a chain link constraint
     */
    static DistanceConstraint::Ptr CreateChainLink(RigidBody* linkA, RigidBody* linkB,
                                                    const Vec3& endA, const Vec3& endB,
                                                    float linkLength)
    {
        return DistanceConstraint::Create(linkA, linkB, endA, endB, linkLength);
    }
};

} // namespace RVX::Physics
