/**
 * @file ConstraintSolver.cpp
 * @brief Constraint solver for joint simulation
 */

#include "Physics/Constraints/IConstraint.h"
#include "Physics/RigidBody.h"
#include <vector>
#include <algorithm>

namespace RVX::Physics
{

/**
 * @brief Sequential impulse constraint solver
 * 
 * Iteratively solves all constraints to find a valid solution
 * that satisfies all joint limits and motor targets.
 */
class ConstraintSolver
{
public:
    ConstraintSolver() = default;

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set number of velocity iterations
     */
    void SetVelocityIterations(int iterations)
    {
        m_velocityIterations = std::max(1, iterations);
    }

    /**
     * @brief Set number of position iterations
     */
    void SetPositionIterations(int iterations)
    {
        m_positionIterations = std::max(1, iterations);
    }

    /**
     * @brief Enable/disable warm starting
     */
    void SetWarmStartEnabled(bool enabled)
    {
        m_warmStartEnabled = enabled;
    }

    // =========================================================================
    // Solving
    // =========================================================================

    /**
     * @brief Solve all constraints for one time step
     */
    void Solve(std::vector<IConstraint::Ptr>& constraints, float deltaTime)
    {
        if (constraints.empty())
            return;

        // Pre-solve: prepare constraints
        for (auto& constraint : constraints)
        {
            if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
            {
                constraint->PreSolve(deltaTime);
            }
        }

        // Velocity iterations
        for (int i = 0; i < m_velocityIterations; ++i)
        {
            for (auto& constraint : constraints)
            {
                if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
                {
                    constraint->SolveVelocity(deltaTime);
                }
            }
        }

        // Position iterations (for stability)
        for (int i = 0; i < m_positionIterations; ++i)
        {
            for (auto& constraint : constraints)
            {
                if (constraint && constraint->IsEnabled() && !constraint->IsBroken())
                {
                    constraint->SolvePosition(deltaTime);
                }
            }
        }

        // Check for broken constraints
        CheckBreaking(constraints, deltaTime);
    }

    /**
     * @brief Solve a single constraint (for testing)
     */
    void SolveSingle(IConstraint& constraint, float deltaTime)
    {
        if (!constraint.IsEnabled() || constraint.IsBroken())
            return;

        constraint.PreSolve(deltaTime);

        for (int i = 0; i < m_velocityIterations; ++i)
        {
            constraint.SolveVelocity(deltaTime);
        }

        for (int i = 0; i < m_positionIterations; ++i)
        {
            constraint.SolvePosition(deltaTime);
        }
    }

private:
    /**
     * @brief Check for constraint breaking
     */
    void CheckBreaking(std::vector<IConstraint::Ptr>& constraints, float deltaTime)
    {
        for (auto& constraint : constraints)
        {
            if (!constraint || !constraint->IsEnabled() || constraint->IsBroken())
                continue;

            float breakingForce = constraint->GetBreakingForce();
            if (breakingForce > 0.0f)
            {
                float appliedForce = constraint->GetAppliedImpulse() / deltaTime;
                if (appliedForce > breakingForce)
                {
                    // Constraint will be marked as broken internally
                    // Could emit an event here
                }
            }
        }
    }

    int m_velocityIterations = 8;
    int m_positionIterations = 3;
    bool m_warmStartEnabled = true;
};

/**
 * @brief Island for constraint solving
 * 
 * Groups connected bodies and constraints for efficient parallel solving.
 */
class ConstraintIsland
{
public:
    void AddBody(RigidBody* body)
    {
        if (body && !body->IsStatic())
        {
            m_bodies.push_back(body);
        }
    }

    void AddConstraint(IConstraint::Ptr constraint)
    {
        if (constraint)
        {
            m_constraints.push_back(constraint);
        }
    }

    void Clear()
    {
        m_bodies.clear();
        m_constraints.clear();
    }

    bool IsEmpty() const
    {
        return m_bodies.empty() && m_constraints.empty();
    }

    bool CanSleep() const
    {
        for (auto* body : m_bodies)
        {
            if (!body->CanSleep())
                return false;

            // Check velocity thresholds
            float linSpeed = length(body->GetLinearVelocity());
            float angSpeed = length(body->GetAngularVelocity());
            if (linSpeed > 0.1f || angSpeed > 0.1f)
                return false;
        }
        return true;
    }

    void PutToSleep()
    {
        for (auto* body : m_bodies)
        {
            body->SetSleeping(true);
        }
    }

    void WakeUp()
    {
        for (auto* body : m_bodies)
        {
            body->WakeUp();
        }
    }

private:
    std::vector<RigidBody*> m_bodies;
    std::vector<IConstraint::Ptr> m_constraints;
};

} // namespace RVX::Physics
