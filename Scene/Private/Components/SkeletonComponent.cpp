#include "Scene/Components/SkeletonComponent.h"
#include "Scene/SceneEntity.h"
#include "Animation/Data/Skeleton.h"
#include "Animation/Runtime/SkeletonPose.h"

namespace RVX
{

void SkeletonComponent::OnAttach()
{
    // Initialize to bind pose if skeleton is set
    if (m_skeleton)
    {
        ResetToBindPose();
    }
}

void SkeletonComponent::OnDetach()
{
    // Clear attachments
    m_attachments.clear();
}

void SkeletonComponent::Tick(float deltaTime)
{
    (void)deltaTime;

    // Update skinning matrices if pose has changed
    if (m_skinningDirty)
    {
        UpdateSkinningMatrices();
    }

    // Update attached entities
    if (!m_attachments.empty())
    {
        UpdateAttachments();
    }
}

AABB SkeletonComponent::GetLocalBounds() const
{
    if (!m_skeleton || m_skeleton->IsEmpty())
    {
        return AABB();
    }

    // Compute bounds from bone positions
    if (m_boundsDirty)
    {
        Vec3 minBounds(std::numeric_limits<float>::max());
        Vec3 maxBounds(std::numeric_limits<float>::lowest());

        for (size_t i = 0; i < m_globalPoses.size(); ++i)
        {
            Vec3 bonePos(m_globalPoses[i][3]);

            // Include bone bounding radius
            float radius = m_skeleton->bones[i].boundingRadius;
            if (radius <= 0.0f) radius = 0.1f;  // Default radius

            minBounds = min(minBounds, bonePos - Vec3(radius));
            maxBounds = max(maxBounds, bonePos + Vec3(radius));
        }

        m_cachedBounds = AABB(minBounds, maxBounds);
        m_boundsDirty = false;
    }

    return m_cachedBounds;
}

void SkeletonComponent::SetSkeleton(std::shared_ptr<Animation::Skeleton> skeleton)
{
    m_skeleton = skeleton;

    if (m_skeleton)
    {
        size_t boneCount = m_skeleton->GetBoneCount();

        // Allocate storage
        m_globalPoses.resize(boneCount, Mat4(1.0f));
        m_skinningMatrices.resize(boneCount, Mat4(1.0f));
        m_boneOverrides.resize(boneCount);

        // Create pose
        m_currentPose = std::make_unique<Animation::SkeletonPose>(m_skeleton);

        // Initialize to bind pose
        ResetToBindPose();
    }
    else
    {
        m_globalPoses.clear();
        m_skinningMatrices.clear();
        m_boneOverrides.clear();
        m_currentPose.reset();
    }

    m_posesDirty = true;
    m_skinningDirty = true;
    m_boundsDirty = true;
    NotifyBoundsChanged();
}

size_t SkeletonComponent::GetBoneCount() const
{
    return m_skeleton ? m_skeleton->GetBoneCount() : 0;
}

void SkeletonComponent::SetPose(const Animation::SkeletonPose& pose)
{
    if (!m_currentPose)
    {
        return;
    }

    // Copy pose data
    *m_currentPose = pose;

    m_posesDirty = true;
    m_skinningDirty = true;
    m_boundsDirty = true;
}

void SkeletonComponent::ResetToBindPose()
{
    if (!m_skeleton || !m_currentPose)
    {
        return;
    }

    // Copy bind pose from skeleton
    m_currentPose->ResetToBindPose();

    m_posesDirty = true;
    m_skinningDirty = true;
    m_boundsDirty = true;
}

int SkeletonComponent::FindBoneIndex(const std::string& boneName) const
{
    if (!m_skeleton)
    {
        return -1;
    }
    return m_skeleton->FindBoneIndex(boneName);
}

Mat4 SkeletonComponent::GetBoneLocalTransform(int boneIndex) const
{
    if (!m_currentPose || boneIndex < 0 || boneIndex >= static_cast<int>(m_currentPose->GetLocalTransforms().size()))
    {
        return Mat4(1.0f);
    }

    return m_currentPose->GetLocalTransforms()[boneIndex].ToMatrix();
}

Mat4 SkeletonComponent::GetBoneLocalTransform(const std::string& boneName) const
{
    return GetBoneLocalTransform(FindBoneIndex(boneName));
}

Mat4 SkeletonComponent::GetBoneWorldTransform(int boneIndex) const
{
    if (!m_skeleton || boneIndex < 0 || boneIndex >= static_cast<int>(m_globalPoses.size()))
    {
        return Mat4(1.0f);
    }

    // Ensure global poses are up to date
    if (m_posesDirty)
    {
        const_cast<SkeletonComponent*>(this)->ComputeGlobalPoses();
    }

    // Multiply by entity world transform
    if (SceneEntity* owner = GetOwner())
    {
        return owner->GetWorldMatrix() * m_globalPoses[boneIndex];
    }

    return m_globalPoses[boneIndex];
}

Mat4 SkeletonComponent::GetBoneWorldTransform(const std::string& boneName) const
{
    return GetBoneWorldTransform(FindBoneIndex(boneName));
}

Vec3 SkeletonComponent::GetBoneWorldPosition(int boneIndex) const
{
    Mat4 worldTransform = GetBoneWorldTransform(boneIndex);
    return Vec3(worldTransform[3]);
}

Vec3 SkeletonComponent::GetBoneWorldPosition(const std::string& boneName) const
{
    return GetBoneWorldPosition(FindBoneIndex(boneName));
}

Quat SkeletonComponent::GetBoneWorldRotation(int boneIndex) const
{
    Mat4 worldTransform = GetBoneWorldTransform(boneIndex);
    return Mat4ToQuat(worldTransform);
}

Quat SkeletonComponent::GetBoneWorldRotation(const std::string& boneName) const
{
    return GetBoneWorldRotation(FindBoneIndex(boneName));
}

void SkeletonComponent::SetBoneLocalRotation(int boneIndex, const Quat& rotation)
{
    if (boneIndex < 0 || boneIndex >= static_cast<int>(m_boneOverrides.size()))
    {
        return;
    }

    m_boneOverrides[boneIndex].hasRotation = true;
    m_boneOverrides[boneIndex].rotation = rotation;
    m_posesDirty = true;
    m_skinningDirty = true;
}

void SkeletonComponent::SetBoneLocalRotation(const std::string& boneName, const Quat& rotation)
{
    SetBoneLocalRotation(FindBoneIndex(boneName), rotation);
}

void SkeletonComponent::SetBoneLocalPosition(int boneIndex, const Vec3& position)
{
    if (boneIndex < 0 || boneIndex >= static_cast<int>(m_boneOverrides.size()))
    {
        return;
    }

    m_boneOverrides[boneIndex].hasPosition = true;
    m_boneOverrides[boneIndex].position = position;
    m_posesDirty = true;
    m_skinningDirty = true;
}

void SkeletonComponent::SetBoneLocalPosition(const std::string& boneName, const Vec3& position)
{
    SetBoneLocalPosition(FindBoneIndex(boneName), position);
}

void SkeletonComponent::ClearBoneOverrides()
{
    for (auto& override : m_boneOverrides)
    {
        override.hasPosition = false;
        override.hasRotation = false;
    }
    m_posesDirty = true;
    m_skinningDirty = true;
}

const std::vector<Mat4>& SkeletonComponent::GetSkinningMatrices() const
{
    if (m_skinningDirty)
    {
        const_cast<SkeletonComponent*>(this)->UpdateSkinningMatrices();
    }
    return m_skinningMatrices;
}

void SkeletonComponent::UpdateSkinningMatrices()
{
    if (m_posesDirty)
    {
        ComputeGlobalPoses();
    }
    ComputeSkinningMatrices();
}

void SkeletonComponent::AttachToBone(const std::string& boneName, SceneEntity* entity)
{
    AttachToBone(FindBoneIndex(boneName), entity);
}

void SkeletonComponent::AttachToBone(int boneIndex, SceneEntity* entity)
{
    if (!entity || boneIndex < 0)
    {
        return;
    }

    // Check if already attached
    for (auto& attachment : m_attachments)
    {
        if (attachment.entity == entity)
        {
            attachment.boneIndex = boneIndex;
            return;
        }
    }

    // Add new attachment
    BoneAttachment attachment;
    attachment.boneIndex = boneIndex;
    attachment.entity = entity;
    m_attachments.push_back(attachment);
}

void SkeletonComponent::DetachFromBone(SceneEntity* entity)
{
    m_attachments.erase(
        std::remove_if(m_attachments.begin(), m_attachments.end(),
            [entity](const BoneAttachment& a) { return a.entity == entity; }),
        m_attachments.end());
}

void SkeletonComponent::UpdateAttachments()
{
    for (auto& attachment : m_attachments)
    {
        if (!attachment.entity || attachment.boneIndex < 0)
        {
            continue;
        }

        // Get bone world transform
        Mat4 boneWorld = GetBoneWorldTransform(attachment.boneIndex);

        // Apply local offset
        Mat4 localTransform = translate(Mat4(1.0f), attachment.localOffset) *
                              QuatToMat4(attachment.localRotation);
        Mat4 finalTransform = boneWorld * localTransform;

        // Set entity transform
        // Note: For parented entities, we'd need to convert to local space
        Vec3 position(finalTransform[3]);
        Quat rotation = Mat4ToQuat(finalTransform);

        attachment.entity->SetPosition(position);
        attachment.entity->SetRotation(rotation);
    }
}

void SkeletonComponent::ComputeGlobalPoses()
{
    if (!m_skeleton || !m_currentPose)
    {
        return;
    }

    const auto& bones = m_skeleton->bones;
    const auto& localTransforms = m_currentPose->GetLocalTransforms();

    for (size_t i = 0; i < bones.size(); ++i)
    {
        // Get local transform from pose
        Mat4 local = localTransforms[i].ToMatrix();

        // Apply overrides
        if (m_boneOverrides[i].hasRotation)
        {
            // Replace rotation component
            local = MakeTranslation(Vec3(local[3])) * 
                    QuatToMat4(m_boneOverrides[i].rotation) * 
                    MakeScale(Vec3(length(Vec3(local[0])), length(Vec3(local[1])), length(Vec3(local[2]))));
        }
        if (m_boneOverrides[i].hasPosition)
        {
            local[3] = Vec4(m_boneOverrides[i].position, 1.0f);
        }

        // Compute global pose
        int parentIndex = bones[i].parentIndex;
        if (parentIndex >= 0 && parentIndex < static_cast<int>(i))
        {
            m_globalPoses[i] = m_globalPoses[parentIndex] * local;
        }
        else
        {
            m_globalPoses[i] = local;
        }
    }

    m_posesDirty = false;
    m_boundsDirty = true;
}

void SkeletonComponent::ComputeSkinningMatrices()
{
    if (!m_skeleton)
    {
        return;
    }

    const auto& bones = m_skeleton->bones;

    for (size_t i = 0; i < bones.size(); ++i)
    {
        // Skinning matrix = GlobalPose * InverseBindPose
        m_skinningMatrices[i] = m_globalPoses[i] * bones[i].inverseBindPose;
    }

    m_skinningDirty = false;
}

} // namespace RVX
