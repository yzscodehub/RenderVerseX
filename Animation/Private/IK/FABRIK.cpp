/**
 * @file FABRIK.cpp
 * @brief FABRIK IK solver implementation
 */

#include "Animation/IK/FABRIK.h"
#include <cmath>
#include <algorithm>

namespace RVX::Animation
{

FABRIK::FABRIK(Skeleton::ConstPtr skeleton)
{
    SetSkeleton(skeleton);
}

void FABRIK::SetChain(const std::vector<int>& boneIndices)
{
    m_chain.boneIndices = boneIndices;
    m_chain.endEffectorIndex = boneIndices.empty() ? -1 : boneIndices.back();
    m_lengthsComputed = false;
}

void FABRIK::SetChain(const std::vector<std::string>& boneNames)
{
    if (!m_skeleton) return;

    std::vector<int> indices;
    indices.reserve(boneNames.size());
    for (const auto& name : boneNames)
    {
        indices.push_back(m_skeleton->FindBoneIndex(name));
    }
    SetChain(indices);
}

void FABRIK::BuildChain(const std::string& startBone, const std::string& endBone)
{
    if (!m_skeleton) return;

    int startIdx = m_skeleton->FindBoneIndex(startBone);
    int endIdx = m_skeleton->FindBoneIndex(endBone);

    if (startIdx < 0 || endIdx < 0) return;

    // Build chain from end to start (following parent chain)
    std::vector<int> reversedChain;
    int current = endIdx;
    
    while (current >= 0)
    {
        reversedChain.push_back(current);
        if (current == startIdx) break;
        
        const Bone* bone = m_skeleton->GetBone(current);
        if (!bone) break;
        current = bone->parentIndex;
    }

    // Reverse to get start-to-end order
    std::vector<int> chain(reversedChain.rbegin(), reversedChain.rend());
    SetChain(chain);
}

void FABRIK::SetConstraint(size_t chainIndex, const FABRIKConstraint& constraint)
{
    m_constraints[chainIndex] = constraint;
}

const FABRIKConstraint* FABRIK::GetConstraint(size_t chainIndex) const
{
    auto it = m_constraints.find(chainIndex);
    return (it != m_constraints.end()) ? &it->second : nullptr;
}

void FABRIK::ClearConstraints()
{
    m_constraints.clear();
}

bool FABRIK::Solve(SkeletonPose& pose, const IKTarget& target)
{
    if (!m_enabled || m_weight <= 0.001f)
        return true;

    if (!m_chain.IsValid() || m_chain.GetLength() < 2)
        return false;

    // Compute bone lengths if needed
    if (!m_lengthsComputed)
    {
        ComputeBoneLengths(pose);
    }

    // Get current positions
    pose.ComputeGlobalTransforms();
    const auto& globalTransforms = pose.GetGlobalTransforms();

    std::vector<Vec3> positions(m_chain.GetLength());
    for (size_t i = 0; i < m_chain.GetLength(); ++i)
    {
        positions[i] = Vec3(globalTransforms[m_chain.boneIndices[i]][3]);
    }

    Vec3 rootPos = positions[0];
    Vec3 targetPos = target.position;

    // Calculate total chain length
    float totalLength = 0.0f;
    for (float len : m_boneLengths)
    {
        totalLength += len;
    }

    // Check if target is reachable
    float distToTarget = length(targetPos - rootPos);
    
    if (distToTarget > totalLength)
    {
        // Target unreachable - stretch toward it
        Vec3 direction = normalize(targetPos - rootPos);
        for (size_t i = 1; i < positions.size(); ++i)
        {
            positions[i] = positions[i - 1] + direction * m_boneLengths[i - 1];
        }
        
        if (m_subBaseEnabled)
        {
            // Move root towards target
            float excess = distToTarget - totalLength;
            rootPos += direction * (excess * m_subBaseWeight);
            positions[0] = rootPos;
        }
    }
    else
    {
        // FABRIK iterations
        for (int iter = 0; iter < m_maxIterations; ++iter)
        {
            // Check convergence
            float error = length(positions.back() - targetPos);
            if (error < m_tolerance)
                break;

            // Forward reaching (from end to root)
            ForwardReach(positions, targetPos);

            // Backward reaching (from root to end)
            BackwardReach(positions, rootPos);
        }
    }

    // Apply results to skeleton
    ApplyToSkeleton(pose, positions);

    // Calculate final error
    pose.ComputeGlobalTransforms();
    Vec3 finalPos = Vec3(pose.GetGlobalTransforms()[m_chain.endEffectorIndex][3]);
    m_lastError = length(finalPos - targetPos);

    return m_lastError < m_tolerance;
}

Vec3 FABRIK::GetEndEffectorPosition(const SkeletonPose& pose) const
{
    if (m_chain.endEffectorIndex < 0) return Vec3(0.0f);
    
    const auto& globals = pose.GetGlobalTransforms();
    if (static_cast<size_t>(m_chain.endEffectorIndex) < globals.size())
    {
        return Vec3(globals[m_chain.endEffectorIndex][3]);
    }
    return Vec3(0.0f);
}

void FABRIK::AddIntermediateTarget(size_t chainIndex, const IKTarget& target)
{
    m_intermediateTargets[chainIndex] = target;
}

void FABRIK::ClearIntermediateTargets()
{
    m_intermediateTargets.clear();
}

void FABRIK::ComputeBoneLengths(const SkeletonPose& pose)
{
    const_cast<SkeletonPose&>(pose).ComputeGlobalTransforms();
    const auto& globals = pose.GetGlobalTransforms();

    m_boneLengths.clear();
    m_boneLengths.reserve(m_chain.GetLength() - 1);

    for (size_t i = 0; i < m_chain.GetLength() - 1; ++i)
    {
        Vec3 pos1 = Vec3(globals[m_chain.boneIndices[i]][3]);
        Vec3 pos2 = Vec3(globals[m_chain.boneIndices[i + 1]][3]);
        m_boneLengths.push_back(length(pos2 - pos1));
    }

    m_lengthsComputed = true;
}

void FABRIK::ForwardReach(std::vector<Vec3>& positions, const Vec3& target)
{
    // Start from end effector
    positions.back() = target;

    // Process intermediate targets
    for (auto& [index, ikTarget] : m_intermediateTargets)
    {
        if (index < positions.size())
        {
            Vec3 current = positions[index];
            positions[index] = mix(current, ikTarget.position, ikTarget.positionWeight);
        }
    }

    // Move joints from end to root
    for (int i = static_cast<int>(positions.size()) - 2; i >= 0; --i)
    {
        float boneLen = m_boneLengths[i];
        Vec3 direction = normalize(positions[i] - positions[i + 1]);
        positions[i] = positions[i + 1] + direction * boneLen;

        // Apply constraints
        if (m_constraintsEnabled)
        {
            ApplyConstraints(positions, i);
        }
    }
}

void FABRIK::BackwardReach(std::vector<Vec3>& positions, const Vec3& root)
{
    // Fix root position
    positions[0] = root;

    // Move joints from root to end
    for (size_t i = 0; i < positions.size() - 1; ++i)
    {
        float boneLen = m_boneLengths[i];
        Vec3 direction = normalize(positions[i + 1] - positions[i]);
        positions[i + 1] = positions[i] + direction * boneLen;

        // Apply constraints
        if (m_constraintsEnabled)
        {
            ApplyConstraints(positions, i + 1);
        }
    }

    // Process intermediate targets again
    for (auto& [index, ikTarget] : m_intermediateTargets)
    {
        if (index < positions.size() && index > 0)
        {
            Vec3 current = positions[index];
            positions[index] = mix(current, ikTarget.position, ikTarget.positionWeight * 0.5f);
        }
    }
}

void FABRIK::ApplyConstraints(std::vector<Vec3>& positions, size_t index)
{
    auto it = m_constraints.find(index);
    if (it == m_constraints.end()) return;

    const FABRIKConstraint& constraint = it->second;

    if (index < 2) return;  // Need parent direction

    // Get directions
    Vec3 parentDir = normalize(positions[index - 1] - positions[index - 2]);
    Vec3 currentDir = normalize(positions[index] - positions[index - 1]);

    if (constraint.isHinge)
    {
        // Hinge constraint - project onto plane perpendicular to hinge axis
        Vec3 projected = currentDir - constraint.hingeAxis * dot(currentDir, constraint.hingeAxis);
        float projLen = length(projected);
        if (projLen > 0.0001f)
        {
            projected = normalize(projected);
            
            // Calculate angle and clamp
            float angle = std::acos(std::clamp(dot(parentDir, projected), -1.0f, 1.0f));
            angle = std::clamp(angle, constraint.minTwist, constraint.maxTwist);
            
            // Apply clamped rotation
            Quat rotation = glm::angleAxis(angle, constraint.hingeAxis);
            currentDir = rotation * parentDir;
        }
    }
    else
    {
        // Cone constraint
        float angle = std::acos(std::clamp(dot(parentDir, currentDir), -1.0f, 1.0f));
        if (angle > constraint.coneAngle)
        {
            Vec3 axis = cross(parentDir, currentDir);
            float axisLen = length(axis);
            if (axisLen > 0.0001f)
            {
                axis = normalize(axis);
                Quat rotation = glm::angleAxis(constraint.coneAngle, axis);
                currentDir = rotation * parentDir;
            }
        }
    }

    // Update position
    float boneLen = m_boneLengths[index - 1];
    positions[index] = positions[index - 1] + currentDir * boneLen;
}

void FABRIK::ApplyToSkeleton(SkeletonPose& pose, const std::vector<Vec3>& positions)
{
    if (!m_skeleton) return;

    auto& localTransforms = pose.GetLocalTransforms();
    const auto& bones = m_skeleton->bones;

    // Calculate rotations for each bone in the chain
    for (size_t i = 0; i < m_chain.GetLength() - 1; ++i)
    {
        int boneIdx = m_chain.boneIndices[i];
        int childIdx = m_chain.boneIndices[i + 1];

        // Get current and target directions
        pose.ComputeGlobalTransforms();
        const auto& globals = pose.GetGlobalTransforms();

        Vec3 currentChildPos = Vec3(globals[childIdx][3]);
        Vec3 currentPos = Vec3(globals[boneIdx][3]);
        Vec3 currentDir = normalize(currentChildPos - currentPos);

        Vec3 targetDir = normalize(positions[i + 1] - positions[i]);

        // Calculate rotation
        float d = dot(currentDir, targetDir);
        d = std::clamp(d, -1.0f, 1.0f);

        if (d < 0.9999f)
        {
            Quat deltaRot;
            if (d < -0.9999f)
            {
                // 180 degree rotation
                Vec3 up = Vec3(0, 1, 0);
                if (std::abs(dot(currentDir, up)) > 0.9f)
                    up = Vec3(1, 0, 0);
                Vec3 axis = normalize(cross(currentDir, up));
                deltaRot = glm::angleAxis(3.14159265f, axis);
            }
            else
            {
                Vec3 axis = normalize(cross(currentDir, targetDir));
                float angle = std::acos(d);
                deltaRot = glm::angleAxis(angle, axis);
            }

            // Apply weight
            if (m_weight < 1.0f)
            {
                deltaRot = slerp(Quat(1, 0, 0, 0), deltaRot, m_weight);
            }

            // Convert to local space and apply
            // Note: This is simplified - proper implementation needs parent inverse
            localTransforms[boneIdx].rotation = 
                normalize(deltaRot * localTransforms[boneIdx].rotation);
        }
    }

    pose.MarkGlobalTransformsDirty();
}

} // namespace RVX::Animation
