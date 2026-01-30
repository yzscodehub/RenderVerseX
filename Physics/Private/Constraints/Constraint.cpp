/**
 * @file Constraint.cpp
 * @brief Constraint base implementations
 */

#include "Physics/Constraints/IConstraint.h"
#include "Physics/Constraints/FixedConstraint.h"
#include "Physics/Constraints/HingeConstraint.h"
#include "Physics/Constraints/SliderConstraint.h"
#include "Physics/Constraints/SpringConstraint.h"
#include "Physics/Constraints/DistanceConstraint.h"
#include "Physics/RigidBody.h"

namespace RVX::Physics
{

// Note: Most constraint implementations are in headers as they are template-like
// and benefit from inlining. This file contains any non-inline implementations
// and utility functions.

/**
 * @brief Get world-space anchor position for a constraint
 */
Vec3 GetWorldAnchor(const RigidBody* body, const Vec3& localAnchor)
{
    if (!body)
        return localAnchor;  // Already world space
    
    return Vec3(body->GetTransform() * Vec4(localAnchor, 1.0f));
}

/**
 * @brief Transform direction from local to world space
 */
Vec3 TransformDirection(const RigidBody* body, const Vec3& localDir)
{
    if (!body)
        return localDir;
    
    return Mat3(body->GetTransform()) * localDir;
}

/**
 * @brief Compute relative transform between two bodies
 */
Mat4 GetRelativeTransform(const RigidBody* bodyA, const RigidBody* bodyB)
{
    Mat4 transformA = bodyA ? bodyA->GetTransform() : Mat4(1.0f);
    Mat4 transformB = bodyB ? bodyB->GetTransform() : Mat4(1.0f);
    
    return inverse(transformA) * transformB;
}

/**
 * @brief Compute angular error as axis-angle
 */
Vec3 ComputeAngularError(const Quat& currentRot, const Quat& targetRot)
{
    Quat error = currentRot * conjugate(targetRot);
    
    // Take shorter path
    if (error.w < 0)
        error = -error;
    
    // Convert to axis-angle
    return Vec3(error.x, error.y, error.z) * 2.0f;
}

/**
 * @brief Apply constraint warm starting
 */
void WarmStartConstraint(RigidBody* bodyA, RigidBody* bodyB,
                         const Vec3& linearImpulse, const Vec3& angularImpulse,
                         const Vec3& pointA, const Vec3& pointB)
{
    if (bodyA && bodyA->IsDynamic())
    {
        bodyA->ApplyImpulseAtPoint(-linearImpulse, pointA);
        bodyA->ApplyAngularImpulse(-angularImpulse);
    }
    
    if (bodyB && bodyB->IsDynamic())
    {
        bodyB->ApplyImpulseAtPoint(linearImpulse, pointB);
        bodyB->ApplyAngularImpulse(angularImpulse);
    }
}

} // namespace RVX::Physics
