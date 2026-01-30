/**
 * @file TwoBoneIK.h
 * @brief Two-bone IK solver (analytical solution)
 * 
 * Common use cases:
 * - Arm IK (shoulder -> elbow -> wrist)
 * - Leg IK (hip -> knee -> ankle)
 */

#pragma once

#include "Animation/IK/IKSolver.h"

namespace RVX::Animation
{

/**
 * @brief Two-bone IK solver
 * 
 * Uses an analytical solution for a two-bone chain.
 * Fast and stable, ideal for limbs.
 * 
 * Usage:
 * @code
 * TwoBoneIK solver(skeleton);
 * solver.SetBones("UpperArm", "LowerArm", "Hand");
 * solver.SetPoleTarget(polePosition);  // For bend direction
 * 
 * IKTarget target(targetPosition);
 * solver.Solve(pose, target);
 * @endcode
 */
class TwoBoneIK : public IKSolver
{
public:
    TwoBoneIK() = default;
    explicit TwoBoneIK(Skeleton::ConstPtr skeleton);

    const char* GetTypeName() const override { return "TwoBoneIK"; }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set the bone chain by indices
     * @param root Root bone (e.g., shoulder)
     * @param mid Middle bone (e.g., elbow)
     * @param tip Tip bone (e.g., wrist)
     */
    void SetBones(int rootIndex, int midIndex, int tipIndex);

    /**
     * @brief Set the bone chain by names
     */
    void SetBones(const std::string& rootName, const std::string& midName, const std::string& tipName);

    /// Get bone indices
    int GetRootBoneIndex() const { return m_rootBoneIndex; }
    int GetMidBoneIndex() const { return m_midBoneIndex; }
    int GetTipBoneIndex() const { return m_tipBoneIndex; }

    /**
     * @brief Set pole target (controls bend direction)
     * 
     * The pole target is a position that the middle joint (elbow/knee)
     * should point towards. This controls the bend direction.
     */
    void SetPoleTarget(const Vec3& polePosition);
    void SetPoleTargetEnabled(bool enabled) { m_usePoleTarget = enabled; }
    bool IsPoleTargetEnabled() const { return m_usePoleTarget; }
    const Vec3& GetPoleTarget() const { return m_poleTarget; }

    /**
     * @brief Set soft limit for when target is beyond reach
     * 
     * When the target is beyond the chain's reach, a soft limit
     * prevents snapping by gradually extending.
     */
    void SetSoftness(float softness) { m_softness = softness; }
    float GetSoftness() const { return m_softness; }

    /**
     * @brief Set bend direction hint (used when no pole target)
     */
    void SetBendHint(const Vec3& hint) { m_bendHint = hint; }
    const Vec3& GetBendHint() const { return m_bendHint; }

    /// Twist offset for the tip bone
    void SetTwistOffset(float radians) { m_twistOffset = radians; }
    float GetTwistOffset() const { return m_twistOffset; }

    // =========================================================================
    // IKSolver Interface
    // =========================================================================

    bool Solve(SkeletonPose& pose, const IKTarget& target) override;
    Vec3 GetEndEffectorPosition(const SkeletonPose& pose) const override;

    // =========================================================================
    // Query
    // =========================================================================

    /// Get the chain length
    float GetChainLength() const { return m_upperLength + m_lowerLength; }

    /// Get individual bone lengths
    float GetUpperLength() const { return m_upperLength; }
    float GetLowerLength() const { return m_lowerLength; }

    /// Is the chain fully extended
    bool IsFullyExtended() const { return m_fullyExtended; }

private:
    void ComputeBoneLengths(const SkeletonPose& pose);
    Quat SolveBoneRotation(const Vec3& boneDir, const Vec3& targetDir, const Vec3& bendAxis);

    int m_rootBoneIndex = -1;
    int m_midBoneIndex = -1;
    int m_tipBoneIndex = -1;

    Vec3 m_poleTarget{0, 0, 1};
    Vec3 m_bendHint{0, 0, 1};
    bool m_usePoleTarget = false;

    float m_upperLength = 0.0f;
    float m_lowerLength = 0.0f;
    float m_softness = 0.0f;
    float m_twistOffset = 0.0f;

    bool m_fullyExtended = false;
    bool m_lengthsComputed = false;
};

} // namespace RVX::Animation
