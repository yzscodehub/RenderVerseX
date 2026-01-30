/**
 * @file FABRIK.h
 * @brief FABRIK (Forward And Backward Reaching Inverse Kinematics) solver
 * 
 * Iterative solver for chains of any length.
 * Good for spines, tails, tentacles, etc.
 */

#pragma once

#include "Animation/IK/IKSolver.h"

namespace RVX::Animation
{

/**
 * @brief Joint constraint for FABRIK
 */
struct FABRIKConstraint
{
    /// Cone constraint (angle limit from parent direction)
    float coneAngle = 3.14159f;  // Default: no limit (180 degrees)
    
    /// Twist limits
    float minTwist = -3.14159f;
    float maxTwist = 3.14159f;
    
    /// Hinge axis (for hinge joints like elbows/knees)
    Vec3 hingeAxis{0, 0, 0};  // Zero = no hinge
    bool isHinge = false;
    
    static FABRIKConstraint Cone(float angleDegrees)
    {
        FABRIKConstraint c;
        c.coneAngle = angleDegrees * 0.0174533f;  // deg to rad
        return c;
    }
    
    static FABRIKConstraint Hinge(const Vec3& axis, float minAngle = -180.0f, float maxAngle = 180.0f)
    {
        FABRIKConstraint c;
        c.hingeAxis = normalize(axis);
        c.isHinge = true;
        c.minTwist = minAngle * 0.0174533f;
        c.maxTwist = maxAngle * 0.0174533f;
        return c;
    }
};

/**
 * @brief FABRIK IK solver
 * 
 * Forward And Backward Reaching Inverse Kinematics solver.
 * Works with chains of any length and supports constraints.
 * 
 * Algorithm:
 * 1. Forward reaching: Move end effector to target, propagate up chain
 * 2. Backward reaching: Fix root position, propagate down chain
 * 3. Repeat until converged or max iterations
 * 
 * Usage:
 * @code
 * FABRIK solver(skeleton);
 * solver.SetChain({"Spine1", "Spine2", "Spine3", "Neck", "Head"});
 * 
 * // Optional: add constraints
 * solver.SetConstraint(0, FABRIKConstraint::Cone(30.0f));
 * 
 * IKTarget target(targetPosition);
 * solver.Solve(pose, target);
 * @endcode
 */
class FABRIK : public IKSolver
{
public:
    FABRIK() = default;
    explicit FABRIK(Skeleton::ConstPtr skeleton);

    const char* GetTypeName() const override { return "FABRIK"; }

    // =========================================================================
    // Chain Configuration
    // =========================================================================

    /**
     * @brief Set the bone chain by indices
     * @param boneIndices Indices from root to tip
     */
    void SetChain(const std::vector<int>& boneIndices);

    /**
     * @brief Set the bone chain by names
     */
    void SetChain(const std::vector<std::string>& boneNames);

    /**
     * @brief Auto-build chain from start to end bone
     */
    void BuildChain(const std::string& startBone, const std::string& endBone);

    /// Get the chain
    const IKChain& GetChain() const { return m_chain; }

    /// Get chain length (number of bones)
    size_t GetChainLength() const { return m_chain.GetLength(); }

    // =========================================================================
    // Constraints
    // =========================================================================

    /**
     * @brief Set constraint for a bone in the chain
     * @param chainIndex Index in the chain (0 = root)
     * @param constraint The constraint
     */
    void SetConstraint(size_t chainIndex, const FABRIKConstraint& constraint);

    /**
     * @brief Get constraint for a bone
     */
    const FABRIKConstraint* GetConstraint(size_t chainIndex) const;

    /**
     * @brief Clear all constraints
     */
    void ClearConstraints();

    /**
     * @brief Enable/disable constraint enforcement
     */
    void SetConstraintsEnabled(bool enabled) { m_constraintsEnabled = enabled; }
    bool AreConstraintsEnabled() const { return m_constraintsEnabled; }

    // =========================================================================
    // Sub-base Mode
    // =========================================================================

    /**
     * @brief Set sub-base mode
     * 
     * When enabled, the root of the chain can move towards the target
     * if the target is out of reach.
     */
    void SetSubBaseEnabled(bool enabled) { m_subBaseEnabled = enabled; }
    bool IsSubBaseEnabled() const { return m_subBaseEnabled; }

    /// Sub-base movement weight
    void SetSubBaseWeight(float weight) { m_subBaseWeight = weight; }
    float GetSubBaseWeight() const { return m_subBaseWeight; }

    // =========================================================================
    // IKSolver Interface
    // =========================================================================

    bool Solve(SkeletonPose& pose, const IKTarget& target) override;
    Vec3 GetEndEffectorPosition(const SkeletonPose& pose) const override;

    // =========================================================================
    // Multi-target
    // =========================================================================

    /**
     * @brief Add an intermediate target
     * @param chainIndex Index in chain
     * @param target Target for this bone
     */
    void AddIntermediateTarget(size_t chainIndex, const IKTarget& target);

    /**
     * @brief Clear intermediate targets
     */
    void ClearIntermediateTargets();

private:
    void ComputeBoneLengths(const SkeletonPose& pose);
    void ForwardReach(std::vector<Vec3>& positions, const Vec3& target);
    void BackwardReach(std::vector<Vec3>& positions, const Vec3& root);
    void ApplyConstraints(std::vector<Vec3>& positions, size_t index);
    void ApplyToSkeleton(SkeletonPose& pose, const std::vector<Vec3>& positions);

    IKChain m_chain;
    std::vector<float> m_boneLengths;
    std::unordered_map<size_t, FABRIKConstraint> m_constraints;
    std::unordered_map<size_t, IKTarget> m_intermediateTargets;

    bool m_constraintsEnabled = true;
    bool m_subBaseEnabled = false;
    float m_subBaseWeight = 0.5f;
    bool m_lengthsComputed = false;
};

} // namespace RVX::Animation
