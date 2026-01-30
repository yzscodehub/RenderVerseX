/**
 * @file IKSolver.h
 * @brief Inverse Kinematics solver interface
 * 
 * Base interface for IK solvers that can be applied to skeleton poses.
 */

#pragma once

#include "Animation/Runtime/SkeletonPose.h"
#include "Animation/Data/Skeleton.h"
#include "Core/MathTypes.h"
#include <memory>
#include <string>
#include <vector>

namespace RVX::Animation
{

/**
 * @brief IK target definition
 */
struct IKTarget
{
    Vec3 position{0.0f};            ///< Target position in world space
    Quat rotation{1, 0, 0, 0};      ///< Target rotation (optional)
    float positionWeight = 1.0f;     ///< Position constraint weight
    float rotationWeight = 0.0f;     ///< Rotation constraint weight
    
    IKTarget() = default;
    IKTarget(const Vec3& pos, float weight = 1.0f)
        : position(pos), positionWeight(weight) {}
    IKTarget(const Vec3& pos, const Quat& rot, float posWeight = 1.0f, float rotWeight = 1.0f)
        : position(pos), rotation(rot), positionWeight(posWeight), rotationWeight(rotWeight) {}
};

/**
 * @brief IK chain definition
 */
struct IKChain
{
    std::vector<int> boneIndices;    ///< Indices of bones in the chain (root to tip)
    int endEffectorIndex = -1;        ///< Index of the end effector bone
    
    /// Get chain length
    size_t GetLength() const { return boneIndices.size(); }
    
    /// Is chain valid
    bool IsValid() const { return !boneIndices.empty() && endEffectorIndex >= 0; }
};

/**
 * @brief Base IK solver interface
 */
class IKSolver
{
public:
    using Ptr = std::shared_ptr<IKSolver>;

    virtual ~IKSolver() = default;

    /// Get solver type name
    virtual const char* GetTypeName() const = 0;

    // =========================================================================
    // Configuration
    // =========================================================================

    /// Set the skeleton
    virtual void SetSkeleton(Skeleton::ConstPtr skeleton) { m_skeleton = skeleton; }
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }

    /// Enable/disable the solver
    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    /// Set global weight
    void SetWeight(float weight) { m_weight = weight; }
    float GetWeight() const { return m_weight; }

    /// Maximum iterations
    void SetMaxIterations(int iterations) { m_maxIterations = iterations; }
    int GetMaxIterations() const { return m_maxIterations; }

    /// Convergence threshold
    void SetTolerance(float tolerance) { m_tolerance = tolerance; }
    float GetTolerance() const { return m_tolerance; }

    // =========================================================================
    // Solving
    // =========================================================================

    /**
     * @brief Solve IK and apply to pose
     * @param pose The pose to modify
     * @param target The IK target
     * @return True if solved within tolerance
     */
    virtual bool Solve(SkeletonPose& pose, const IKTarget& target) = 0;

    /**
     * @brief Get the end effector position after solve
     */
    virtual Vec3 GetEndEffectorPosition(const SkeletonPose& pose) const = 0;

    /**
     * @brief Get the error distance after solve
     */
    virtual float GetError() const { return m_lastError; }

protected:
    Skeleton::ConstPtr m_skeleton;
    bool m_enabled = true;
    float m_weight = 1.0f;
    int m_maxIterations = 10;
    float m_tolerance = 0.001f;
    float m_lastError = 0.0f;
};

} // namespace RVX::Animation
