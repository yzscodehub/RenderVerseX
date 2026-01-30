/**
 * @file SkeletonPose.cpp
 * @brief SkeletonPose implementation
 */

#include "Animation/Runtime/SkeletonPose.h"

namespace RVX::Animation
{

// Static identity transform for out-of-bounds access
static const TransformSample s_identityTransform = TransformSample::Identity();

SkeletonPose::SkeletonPose(Skeleton::ConstPtr skeleton)
    : m_skeleton(skeleton)
{
    if (m_skeleton)
    {
        size_t boneCount = m_skeleton->GetBoneCount();
        m_localTransforms.resize(boneCount);
        m_globalTransforms.resize(boneCount, Mat4(1.0f));
        m_skinningMatrices.resize(boneCount, Mat4(1.0f));
        ResetToBindPose();
    }
}

SkeletonPose::SkeletonPose(size_t boneCount)
{
    m_localTransforms.resize(boneCount);
    m_globalTransforms.resize(boneCount, Mat4(1.0f));
    m_skinningMatrices.resize(boneCount, Mat4(1.0f));
    ResetToIdentity();
}

void SkeletonPose::SetSkeleton(Skeleton::ConstPtr skeleton)
{
    m_skeleton = skeleton;
    if (m_skeleton)
    {
        size_t boneCount = m_skeleton->GetBoneCount();
        m_localTransforms.resize(boneCount);
        m_globalTransforms.resize(boneCount, Mat4(1.0f));
        m_skinningMatrices.resize(boneCount, Mat4(1.0f));
        ResetToBindPose();
    }
    m_globalsDirty = true;
    m_skinningDirty = true;
}

const TransformSample& SkeletonPose::GetLocalTransform(size_t boneIndex) const
{
    if (boneIndex < m_localTransforms.size())
    {
        return m_localTransforms[boneIndex];
    }
    return s_identityTransform;
}

TransformSample& SkeletonPose::GetLocalTransform(size_t boneIndex)
{
    static TransformSample s_dummy;
    if (boneIndex < m_localTransforms.size())
    {
        m_globalsDirty = true;
        m_skinningDirty = true;
        return m_localTransforms[boneIndex];
    }
    s_dummy = TransformSample::Identity();
    return s_dummy;
}

void SkeletonPose::SetLocalTransform(size_t boneIndex, const TransformSample& transform)
{
    if (boneIndex < m_localTransforms.size())
    {
        m_localTransforms[boneIndex] = transform;
        m_globalsDirty = true;
        m_skinningDirty = true;
    }
}

const TransformSample* SkeletonPose::GetLocalTransform(const std::string& boneName) const
{
    if (!m_skeleton) return nullptr;
    
    int index = m_skeleton->FindBoneIndex(boneName);
    if (index >= 0 && static_cast<size_t>(index) < m_localTransforms.size())
    {
        return &m_localTransforms[index];
    }
    return nullptr;
}

void SkeletonPose::ComputeGlobalTransforms()
{
    if (!m_globalsDirty) return;
    if (!m_skeleton || m_localTransforms.empty()) return;

    const auto& bones = m_skeleton->bones;
    
    for (size_t i = 0; i < bones.size(); ++i)
    {
        const Bone& bone = bones[i];
        Mat4 local = m_localTransforms[i].ToMatrix();

        if (bone.parentIndex >= 0 && static_cast<size_t>(bone.parentIndex) < i)
        {
            m_globalTransforms[i] = m_globalTransforms[bone.parentIndex] * local;
        }
        else
        {
            m_globalTransforms[i] = local;
        }
    }

    m_globalsDirty = false;
}

void SkeletonPose::ComputeSkinningMatrices()
{
    if (!m_skinningDirty) return;
    
    // Ensure globals are computed first
    ComputeGlobalTransforms();
    
    if (!m_skeleton) return;

    const auto& bones = m_skeleton->bones;
    
    for (size_t i = 0; i < bones.size(); ++i)
    {
        m_skinningMatrices[i] = m_globalTransforms[i] * bones[i].inverseBindPose;
    }

    m_skinningDirty = false;
}

void SkeletonPose::ResetToBindPose()
{
    if (!m_skeleton) return;

    const auto& bones = m_skeleton->bones;
    for (size_t i = 0; i < bones.size(); ++i)
    {
        m_localTransforms[i] = bones[i].localBindPose;
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

void SkeletonPose::ResetToIdentity()
{
    for (auto& transform : m_localTransforms)
    {
        transform = TransformSample::Identity();
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

void SkeletonPose::CopyFrom(const SkeletonPose& other)
{
    m_skeleton = other.m_skeleton;
    m_localTransforms = other.m_localTransforms;
    m_globalTransforms = other.m_globalTransforms;
    m_skinningMatrices = other.m_skinningMatrices;
    m_globalsDirty = other.m_globalsDirty;
    m_skinningDirty = other.m_skinningDirty;
}

void SkeletonPose::BlendWith(const SkeletonPose& other, float weight)
{
    if (weight <= 0.0f) return;
    if (weight >= 1.0f)
    {
        CopyFrom(other);
        return;
    }

    size_t count = std::min(m_localTransforms.size(), other.m_localTransforms.size());
    for (size_t i = 0; i < count; ++i)
    {
        m_localTransforms[i] = TransformSample::Lerp(
            m_localTransforms[i], 
            other.m_localTransforms[i], 
            weight);
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

void SkeletonPose::Blend(const SkeletonPose& a, const SkeletonPose& b, 
                          float weight, SkeletonPose& result)
{
    size_t count = std::min(a.m_localTransforms.size(), b.m_localTransforms.size());
    
    if (result.m_localTransforms.size() != count)
    {
        result.m_localTransforms.resize(count);
    }

    for (size_t i = 0; i < count; ++i)
    {
        result.m_localTransforms[i] = TransformSample::Lerp(
            a.m_localTransforms[i], 
            b.m_localTransforms[i], 
            weight);
    }

    result.m_skeleton = a.m_skeleton ? a.m_skeleton : b.m_skeleton;
    result.m_globalsDirty = true;
    result.m_skinningDirty = true;
}

void SkeletonPose::AdditivelBlend(const SkeletonPose& additivePose, float weight)
{
    if (weight <= 0.0f) return;

    size_t count = std::min(m_localTransforms.size(), additivePose.m_localTransforms.size());
    for (size_t i = 0; i < count; ++i)
    {
        m_localTransforms[i] = TransformSample::Additive(
            m_localTransforms[i],
            additivePose.m_localTransforms[i],
            weight);
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

void SkeletonPose::AdditiveBlend(const SkeletonPose& base, const SkeletonPose& additive,
                                  float weight, SkeletonPose& result)
{
    result.CopyFrom(base);
    result.AdditivelBlend(additive, weight);
}

void SkeletonPose::BlendWithMask(const SkeletonPose& other, const std::vector<float>& weights)
{
    size_t count = std::min({m_localTransforms.size(), 
                            other.m_localTransforms.size(), 
                            weights.size()});
    
    for (size_t i = 0; i < count; ++i)
    {
        float w = weights[i];
        if (w > 0.0f)
        {
            m_localTransforms[i] = TransformSample::Lerp(
                m_localTransforms[i], 
                other.m_localTransforms[i], 
                w);
        }
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

void SkeletonPose::CopyBones(const SkeletonPose& other, const std::vector<int>& boneIndices)
{
    for (int index : boneIndices)
    {
        if (index >= 0 && 
            static_cast<size_t>(index) < m_localTransforms.size() &&
            static_cast<size_t>(index) < other.m_localTransforms.size())
        {
            m_localTransforms[index] = other.m_localTransforms[index];
        }
    }

    m_globalsDirty = true;
    m_skinningDirty = true;
}

} // namespace RVX::Animation
