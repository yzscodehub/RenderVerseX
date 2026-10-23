/**
 * @file CollisionResponse.cpp
 * @brief Collision response and impulse resolution
 */

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include <cmath>
#include <algorithm>

namespace RVX::Physics
{

/**
 * @brief Collision response calculator
 * 
 * Implements impulse-based collision resolution with:
 * - Elastic/inelastic collisions (restitution)
 * - Friction (Coulomb model)
 * - Position correction (Baumgarte stabilization)
 */
class CollisionResponse
{
public:
    /**
     * @brief Compute collision impulse between two bodies
     * 
     * @param bodyA First body (can be null for static world)
     * @param bodyB Second body
     * @param normal Collision normal (from A to B)
     * @param pointA Contact point on A
     * @param pointB Contact point on B
     * @param restitution Combined coefficient of restitution
     * @return Impulse magnitude
     */
    static float ComputeImpulse(RigidBody* bodyA, RigidBody* bodyB,
                                const Vec3& normal,
                                const Vec3& pointA, const Vec3& pointB,
                                float restitution)
    {
        // Get velocities at contact points
        Vec3 velA = bodyA ? bodyA->GetVelocityAtPoint(pointA) : Vec3(0);
        Vec3 velB = bodyB ? bodyB->GetVelocityAtPoint(pointB) : Vec3(0);
        
        // Relative velocity
        Vec3 relVel = velB - velA;
        float relVelNormal = dot(relVel, normal);

        // Don't resolve if separating
        if (relVelNormal > 0)
            return 0.0f;

        // Inverse masses
        float invMassA = bodyA ? bodyA->GetInverseMass() : 0.0f;
        float invMassB = bodyB ? bodyB->GetInverseMass() : 0.0f;

        // Skip if both infinite mass
        if (invMassA + invMassB == 0.0f)
            return 0.0f;

        // Relative positions from centers of mass
        Vec3 rA = bodyA ? pointA - bodyA->GetPosition() : Vec3(0);
        Vec3 rB = bodyB ? pointB - bodyB->GetPosition() : Vec3(0);

        // For now, simplified calculation without angular effects
        // Full version would include inertia tensor contributions

        float effectiveMass = invMassA + invMassB;

        // Impulse magnitude
        float j = -(1.0f + restitution) * relVelNormal / effectiveMass;

        return j;
    }

    /**
     * @brief Apply collision impulse to bodies
     */
    static void ApplyImpulse(RigidBody* bodyA, RigidBody* bodyB,
                             const Vec3& impulse,
                             const Vec3& pointA, const Vec3& pointB)
    {
        if (bodyA && bodyA->IsDynamic())
        {
            bodyA->ApplyImpulseAtPoint(-impulse, pointA);
        }

        if (bodyB && bodyB->IsDynamic())
        {
            bodyB->ApplyImpulseAtPoint(impulse, pointB);
        }
    }

    /**
     * @brief Resolve collision between two bodies
     */
    static void ResolveCollision(RigidBody* bodyA, RigidBody* bodyB,
                                 const Vec3& normal, float depth,
                                 const Vec3& pointA, const Vec3& pointB,
                                 float restitution, float friction)
    {
        // Compute and apply normal impulse
        float jn = ComputeImpulse(bodyA, bodyB, normal, pointA, pointB, restitution);
        Vec3 impulseNormal = normal * jn;
        ApplyImpulse(bodyA, bodyB, impulseNormal, pointA, pointB);

        // Compute friction impulse
        if (friction > 0.0f && std::abs(jn) > 0.0001f)
        {
            ApplyFriction(bodyA, bodyB, normal, pointA, pointB, jn, friction);
        }

        // Position correction
        if (depth > 0.001f)
        {
            CorrectPosition(bodyA, bodyB, normal, depth);
        }
    }

    /**
     * @brief Apply friction impulse
     */
    static void ApplyFriction(RigidBody* bodyA, RigidBody* bodyB,
                              const Vec3& normal,
                              const Vec3& pointA, const Vec3& pointB,
                              float normalImpulse, float friction)
    {
        // Get velocities
        Vec3 velA = bodyA ? bodyA->GetVelocityAtPoint(pointA) : Vec3(0);
        Vec3 velB = bodyB ? bodyB->GetVelocityAtPoint(pointB) : Vec3(0);
        Vec3 relVel = velB - velA;

        // Tangent velocity (perpendicular to normal)
        Vec3 tangentVel = relVel - normal * dot(relVel, normal);
        float tangentSpeed = length(tangentVel);

        if (tangentSpeed < 0.0001f)
            return;

        Vec3 tangent = tangentVel / tangentSpeed;

        // Inverse masses
        float invMassA = bodyA ? bodyA->GetInverseMass() : 0.0f;
        float invMassB = bodyB ? bodyB->GetInverseMass() : 0.0f;
        float effectiveMass = invMassA + invMassB;

        if (effectiveMass == 0.0f)
            return;

        // Friction impulse magnitude
        float jt = -tangentSpeed / effectiveMass;

        // Coulomb's law: clamp to friction cone
        float maxFriction = friction * std::abs(normalImpulse);
        jt = std::clamp(jt, -maxFriction, maxFriction);

        // Apply friction impulse
        Vec3 frictionImpulse = tangent * jt;
        ApplyImpulse(bodyA, bodyB, frictionImpulse, pointA, pointB);
    }

    /**
     * @brief Correct penetration with position adjustment
     * 
     * Uses Baumgarte stabilization: move bodies apart by a fraction of penetration
     */
    static void CorrectPosition(RigidBody* bodyA, RigidBody* bodyB,
                                const Vec3& normal, float depth,
                                float slop = 0.005f, float percent = 0.2f)
    {
        float correctionDepth = std::max(depth - slop, 0.0f);
        
        float invMassA = bodyA ? bodyA->GetInverseMass() : 0.0f;
        float invMassB = bodyB ? bodyB->GetInverseMass() : 0.0f;
        float totalInvMass = invMassA + invMassB;

        if (totalInvMass == 0.0f)
            return;

        Vec3 correction = normal * (correctionDepth / totalInvMass) * percent;

        if (bodyA && bodyA->IsDynamic())
        {
            bodyA->SetPosition(bodyA->GetPosition() - correction * invMassA);
        }

        if (bodyB && bodyB->IsDynamic())
        {
            bodyB->SetPosition(bodyB->GetPosition() + correction * invMassB);
        }
    }

    /**
     * @brief Compute combined restitution from two materials
     */
    static float CombineRestitution(float restA, float restB)
    {
        // Average (alternative: max, multiply, etc.)
        return (restA + restB) * 0.5f;
    }

    /**
     * @brief Compute combined friction from two materials
     */
    static float CombineFriction(float fricA, float fricB)
    {
        // Geometric mean (common approach)
        return std::sqrt(fricA * fricB);
    }
};

/**
 * @brief Sequential impulse constraint solver
 */
class SequentialImpulseSolver
{
public:
    /**
     * @brief Solve all constraints iteratively
     */
    template<typename ContactContainer>
    static void Solve(ContactContainer& contacts, int iterations)
    {
        for (int iter = 0; iter < iterations; ++iter)
        {
            for (auto& contact : contacts)
            {
                SolveContact(contact);
            }
        }
    }

private:
    template<typename Contact>
    static void SolveContact(Contact& contact)
    {
        // Each contact would store accumulated impulse for warm starting
        // and clamp impulse to prevent separation

        // Simplified version:
        CollisionResponse::ResolveCollision(
            contact.bodyA, contact.bodyB,
            contact.normal, contact.depth,
            contact.pointA, contact.pointB,
            contact.restitution, contact.friction
        );
    }
};

} // namespace RVX::Physics
