/**
 * @file TwoBoneIK.cpp
 * @brief Two-bone IK solver implementation
 */

#include "Animation/IK/TwoBoneIK.h"
#include <cmath>
#include <algorithm>

namespace RVX::Animation
{

TwoBoneIK::TwoBoneIK(Skeleton::ConstPtr skeleton)
{
    SetSkeleton(skeleton);
}

void TwoBoneIK::SetBones(int rootIndex, int midIndex, int tipIndex)
{
    m_rootBoneIndex = rootIndex;
    m_midBoneIndex = midIndex;
    m_tipBoneIndex = tipIndex;
    m_lengthsComputed = false;
}

void TwoBoneIK::SetBones(const std::string& rootName, const std::string& midName, const std::string& tipName)
{
    if (!m_skeleton) return;

    SetBones(
        m_skeleton->FindBoneIndex(rootName),
        m_skeleton->FindBoneIndex(midName),
        m_skeleton->FindBoneIndex(tipName)
    );
}

void TwoBoneIK::SetPoleTarget(const Vec3& polePosition)
{
    m_poleTarget = polePosition;
    m_usePoleTarget = true;
}

bool TwoBoneIK::Solve(SkeletonPose& pose, const IKTarget& target)
{
    if (!m_enabled || m_weight <= 0.001f)
        return true;

    if (m_rootBoneIndex < 0 || m_midBoneIndex < 0 || m_tipBoneIndex < 0)
        return false;

    // Compute bone lengths if needed
    if (!m_lengthsComputed)
    {
        ComputeBoneLengths(pose);
    }

    // Get current global positions
    pose.ComputeGlobalTransforms();
    const auto& globalTransforms = pose.GetGlobalTransforms();

    Vec3 rootPos = Vec3(globalTransforms[m_rootBoneIndex][3]);
    Vec3 midPos = Vec3(globalTransforms[m_midBoneIndex][3]);
    Vec3 tipPos = Vec3(globalTransforms[m_tipBoneIndex][3]);

    Vec3 targetPos = target.position;

    // Calculate distance to target
    Vec3 toTarget = targetPos - rootPos;
    float targetDist = length(toTarget);

    float chainLength = m_upperLength + m_lowerLength;

    // Apply softness for when target is beyond reach
    float softDist = chainLength;
    if (m_softness > 0.0f && targetDist > chainLength - m_softness)
    {
        float soft = chainLength - m_softness;
        float x = targetDist - soft;
        softDist = soft + m_softness * (1.0f - std::exp(-x / m_softness));
    }

    // Clamp to reachable distance
    m_fullyExtended = targetDist >= chainLength;
    targetDist = std::min(targetDist, softDist);

    if (targetDist < 0.0001f)
    {
        m_lastError = 0.0f;
        return true;
    }

    // Normalize direction to target
    Vec3 targetDir = toTarget / length(toTarget);

    // Calculate the angle at the mid joint using law of cosines
    // a^2 = b^2 + c^2 - 2bc*cos(A)
    float cosAngle = (m_upperLength * m_upperLength + m_lowerLength * m_lowerLength - 
                      targetDist * targetDist) / (2.0f * m_upperLength * m_lowerLength);
    cosAngle = std::clamp(cosAngle, -1.0f, 1.0f);
    float midAngle = std::acos(cosAngle);

    // Calculate the angle at the root
    float cosRootAngle = (targetDist * targetDist + m_upperLength * m_upperLength - 
                          m_lowerLength * m_lowerLength) / (2.0f * targetDist * m_upperLength);
    cosRootAngle = std::clamp(cosRootAngle, -1.0f, 1.0f);
    float rootAngle = std::acos(cosRootAngle);

    // Determine bend axis
    Vec3 bendAxis;
    if (m_usePoleTarget)
    {
        Vec3 toPole = m_poleTarget - rootPos;
        Vec3 projPole = toPole - targetDir * dot(toPole, targetDir);
        float projLen = length(projPole);
        if (projLen > 0.0001f)
        {
            bendAxis = cross(targetDir, normalize(projPole));
        }
        else
        {
            bendAxis = m_bendHint;
        }
    }
    else
    {
        // Use current bend direction
        Vec3 currentToMid = midPos - rootPos;
        Vec3 projMid = currentToMid - targetDir * dot(currentToMid, targetDir);
        float projLen = length(projMid);
        if (projLen > 0.0001f)
        {
            bendAxis = cross(targetDir, normalize(projMid));
        }
        else
        {
            bendAxis = m_bendHint;
        }
    }
    bendAxis = normalize(bendAxis);

    // Calculate new positions
    Quat rootRotation = glm::angleAxis(rootAngle, bendAxis);
    Vec3 upperDir = rootRotation * targetDir;
    Vec3 newMidPos = rootPos + upperDir * m_upperLength;

    Vec3 lowerDir = normalize(targetPos - newMidPos);
    Vec3 newTipPos = newMidPos + lowerDir * m_lowerLength;

    // Calculate rotations and apply to pose
    auto& localTransforms = pose.GetLocalTransforms();

    // Root bone rotation
    {
        Vec3 currentUpperDir = normalize(midPos - rootPos);
        Quat deltaRot = SolveBoneRotation(currentUpperDir, upperDir, bendAxis);
        
        if (m_weight < 1.0f)
        {
            deltaRot = slerp(Quat(1, 0, 0, 0), deltaRot, m_weight);
        }
        
        localTransforms[m_rootBoneIndex].rotation = 
            normalize(deltaRot * localTransforms[m_rootBoneIndex].rotation);
    }

    // Mid bone rotation
    {
        Vec3 currentLowerDir = normalize(tipPos - midPos);
        Vec3 newLowerDir = normalize(newTipPos - newMidPos);
        Quat deltaRot = SolveBoneRotation(currentLowerDir, newLowerDir, bendAxis);
        
        if (m_weight < 1.0f)
        {
            deltaRot = slerp(Quat(1, 0, 0, 0), deltaRot, m_weight);
        }
        
        localTransforms[m_midBoneIndex].rotation = 
            normalize(deltaRot * localTransforms[m_midBoneIndex].rotation);
    }

    // Apply tip rotation for end effector orientation
    if (target.rotationWeight > 0.001f)
    {
        Quat targetRot = target.rotation;
        Quat currentRot = localTransforms[m_tipBoneIndex].rotation;
        Quat finalRot = slerp(currentRot, targetRot, target.rotationWeight * m_weight);
        localTransforms[m_tipBoneIndex].rotation = normalize(finalRot);
    }

    // Apply twist offset
    if (std::abs(m_twistOffset) > 0.001f)
    {
        Vec3 tipDir = normalize(newTipPos - newMidPos);
        Quat twist = glm::angleAxis(m_twistOffset * m_weight, tipDir);
        localTransforms[m_tipBoneIndex].rotation = 
            normalize(twist * localTransforms[m_tipBoneIndex].rotation);
    }

    pose.MarkGlobalTransformsDirty();

    // Calculate error
    pose.ComputeGlobalTransforms();
    Vec3 finalTipPos = Vec3(pose.GetGlobalTransforms()[m_tipBoneIndex][3]);
    m_lastError = length(finalTipPos - targetPos);

    return m_lastError < m_tolerance;
}

Vec3 TwoBoneIK::GetEndEffectorPosition(const SkeletonPose& pose) const
{
    if (m_tipBoneIndex < 0) return Vec3(0.0f);
    
    const auto& globals = pose.GetGlobalTransforms();
    if (static_cast<size_t>(m_tipBoneIndex) < globals.size())
    {
        return Vec3(globals[m_tipBoneIndex][3]);
    }
    return Vec3(0.0f);
}

void TwoBoneIK::ComputeBoneLengths(const SkeletonPose& pose)
{
    if (m_rootBoneIndex < 0 || m_midBoneIndex < 0 || m_tipBoneIndex < 0)
        return;

    // Ensure globals are computed
    const_cast<SkeletonPose&>(pose).ComputeGlobalTransforms();
    const auto& globals = pose.GetGlobalTransforms();

    Vec3 rootPos = Vec3(globals[m_rootBoneIndex][3]);
    Vec3 midPos = Vec3(globals[m_midBoneIndex][3]);
    Vec3 tipPos = Vec3(globals[m_tipBoneIndex][3]);

    m_upperLength = length(midPos - rootPos);
    m_lowerLength = length(tipPos - midPos);
    m_lengthsComputed = true;
}

Quat TwoBoneIK::SolveBoneRotation(const Vec3& boneDir, const Vec3& targetDir, const Vec3& bendAxis)
{
    float d = dot(boneDir, targetDir);
    d = std::clamp(d, -1.0f, 1.0f);

    if (d > 0.9999f)
    {
        return Quat(1, 0, 0, 0);  // Already aligned
    }

    if (d < -0.9999f)
    {
        // 180 degree rotation
        return glm::angleAxis(3.14159265f, bendAxis);
    }

    Vec3 axis = normalize(cross(boneDir, targetDir));
    float angle = std::acos(d);
    
    return glm::angleAxis(angle, axis);
}

} // namespace RVX::Animation
