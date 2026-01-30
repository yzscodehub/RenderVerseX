/**
 * @file Integrator.cpp
 * @brief Physics integration (velocity/position updates)
 */

#include "Physics/PhysicsTypes.h"
#include "Physics/RigidBody.h"
#include <cmath>

namespace RVX::Physics
{

/**
 * @brief Semi-implicit Euler integrator for physics simulation
 */
class Integrator
{
public:
    /**
     * @brief Integrate a single body
     */
    static void IntegrateBody(RigidBody& body, const Vec3& gravity, float deltaTime)
    {
        if (!body.IsDynamic() || body.IsSleeping())
            return;

        // Apply gravity
        Vec3 acceleration = gravity * body.GetGravityScale();

        // Get current state
        Vec3 velocity = body.GetLinearVelocity();
        Vec3 angularVelocity = body.GetAngularVelocity();
        Vec3 position = body.GetPosition();
        Quat rotation = body.GetRotation();

        // Semi-implicit Euler: update velocity first, then position
        // v(t+dt) = v(t) + a * dt
        velocity += acceleration * deltaTime;

        // Apply damping
        float linearDamping = 1.0f - body.GetLinearDamping() * deltaTime;
        float angularDamping = 1.0f - body.GetAngularDamping() * deltaTime;
        linearDamping = std::max(0.0f, linearDamping);
        angularDamping = std::max(0.0f, angularDamping);

        velocity *= linearDamping;
        angularVelocity *= angularDamping;

        // x(t+dt) = x(t) + v(t+dt) * dt
        position += velocity * deltaTime;

        // Quaternion integration
        // q(t+dt) = q(t) + 0.5 * w * q(t) * dt
        if (length(angularVelocity) > 0.0001f)
        {
            Quat wQuat(0.0f, angularVelocity.x, angularVelocity.y, angularVelocity.z);
            Quat dq = wQuat * rotation * 0.5f * deltaTime;
            rotation = normalize(rotation + dq);
        }

        // Update body state
        body.SetLinearVelocity(velocity);
        body.SetAngularVelocity(angularVelocity);
        body.SetPosition(position);
        body.SetRotation(rotation);

        // Clear accumulated forces
        body.ClearForces();
    }

    /**
     * @brief Apply accumulated forces to velocity
     */
    static void ApplyForces(RigidBody& body, float deltaTime)
    {
        if (!body.IsDynamic() || body.IsSleeping())
            return;

        // F = ma, so a = F/m = F * invMass
        // Note: Forces should be accumulated in the body before this call
        // The actual implementation would access the body's internal force accumulator
    }

    /**
     * @brief Check if body should go to sleep
     */
    static bool ShouldSleep(const RigidBody& body, float sleepThreshold = 0.1f)
    {
        if (!body.CanSleep() || !body.IsDynamic())
            return false;

        float linearSpeed = length(body.GetLinearVelocity());
        float angularSpeed = length(body.GetAngularVelocity());

        return linearSpeed < sleepThreshold && angularSpeed < sleepThreshold;
    }

    /**
     * @brief Verlet integration for position-based dynamics
     */
    static void IntegrateVerlet(Vec3& position, Vec3& previousPosition,
                                const Vec3& acceleration, float deltaTime)
    {
        Vec3 temp = position;
        position = position * 2.0f - previousPosition + acceleration * deltaTime * deltaTime;
        previousPosition = temp;
    }
};

} // namespace RVX::Physics
