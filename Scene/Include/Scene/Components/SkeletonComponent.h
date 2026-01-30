#pragma once

/**
 * @file SkeletonComponent.h
 * @brief Skeleton component for skeletal mesh rendering
 * 
 * SkeletonComponent holds skeleton data and current pose for
 * GPU skinning and skeletal animation.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include <memory>
#include <vector>
#include <string>

// Forward declarations
namespace RVX::Animation
{
    struct Skeleton;
    class SkeletonPose;
}

namespace RVX
{

/**
 * @brief Skeleton update mode
 */
enum class SkeletonUpdateMode : uint8_t
{
    Auto = 0,       // Updated by AnimatorComponent
    Manual          // Manually controlled poses
};

/**
 * @brief Skeleton component for skeletal mesh rendering
 * 
 * Features:
 * - Holds skeleton definition and current pose
 * - Computes skinning matrices for GPU skinning
 * - Supports bone attachment points
 * - Provides bone transform queries
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Character");
 * auto* skel = entity->AddComponent<SkeletonComponent>();
 * skel->SetSkeleton(characterSkeleton);
 * 
 * // Get bone transform for attachment
 * Mat4 handTransform = skel->GetBoneWorldTransform("RightHand");
 * @endcode
 */
class SkeletonComponent : public Component
{
public:
    SkeletonComponent() = default;
    ~SkeletonComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "Skeleton"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Spatial Bounds
    // =========================================================================

    bool ProvidesBounds() const override { return true; }
    AABB GetLocalBounds() const override;

    // =========================================================================
    // Skeleton Data
    // =========================================================================

    /// Set the skeleton definition
    void SetSkeleton(std::shared_ptr<Animation::Skeleton> skeleton);
    std::shared_ptr<Animation::Skeleton> GetSkeleton() const { return m_skeleton; }

    /// Check if skeleton is valid
    bool HasSkeleton() const { return m_skeleton != nullptr; }

    /// Get bone count
    size_t GetBoneCount() const;

    // =========================================================================
    // Pose Control
    // =========================================================================

    /// Set the current pose (typically called by AnimatorComponent)
    void SetPose(const Animation::SkeletonPose& pose);

    /// Get current pose
    const Animation::SkeletonPose* GetPose() const { return m_currentPose.get(); }

    /// Reset to bind pose
    void ResetToBindPose();

    /// Get update mode
    SkeletonUpdateMode GetUpdateMode() const { return m_updateMode; }
    void SetUpdateMode(SkeletonUpdateMode mode) { m_updateMode = mode; }

    // =========================================================================
    // Bone Queries
    // =========================================================================

    /// Find bone index by name
    int FindBoneIndex(const std::string& boneName) const;

    /// Get bone local transform
    Mat4 GetBoneLocalTransform(int boneIndex) const;
    Mat4 GetBoneLocalTransform(const std::string& boneName) const;

    /// Get bone world transform (includes entity transform)
    Mat4 GetBoneWorldTransform(int boneIndex) const;
    Mat4 GetBoneWorldTransform(const std::string& boneName) const;

    /// Get bone position in world space
    Vec3 GetBoneWorldPosition(int boneIndex) const;
    Vec3 GetBoneWorldPosition(const std::string& boneName) const;

    /// Get bone rotation in world space
    Quat GetBoneWorldRotation(int boneIndex) const;
    Quat GetBoneWorldRotation(const std::string& boneName) const;

    // =========================================================================
    // Manual Bone Control
    // =========================================================================

    /// Override a bone's local rotation (for procedural animation)
    void SetBoneLocalRotation(int boneIndex, const Quat& rotation);
    void SetBoneLocalRotation(const std::string& boneName, const Quat& rotation);

    /// Override a bone's local position
    void SetBoneLocalPosition(int boneIndex, const Vec3& position);
    void SetBoneLocalPosition(const std::string& boneName, const Vec3& position);

    /// Clear all bone overrides
    void ClearBoneOverrides();

    // =========================================================================
    // Skinning Matrices
    // =========================================================================

    /// Get skinning matrices for GPU upload
    /// These are: SkinningMatrix[i] = GlobalPose[i] * InverseBindPose[i]
    const std::vector<Mat4>& GetSkinningMatrices() const;

    /// Force update of skinning matrices
    void UpdateSkinningMatrices();

    /// Check if skinning matrices need update
    bool AreSkinningMatricesDirty() const { return m_skinningDirty; }

    // =========================================================================
    // Bone Attachments
    // =========================================================================

    /// Attach an entity to a bone
    void AttachToBone(const std::string& boneName, SceneEntity* entity);
    void AttachToBone(int boneIndex, SceneEntity* entity);

    /// Detach an entity from bones
    void DetachFromBone(SceneEntity* entity);

    /// Update attached entity transforms
    void UpdateAttachments();

    // =========================================================================
    // Debug
    // =========================================================================

    /// Enable debug visualization
    bool IsDebugDrawEnabled() const { return m_debugDraw; }
    void SetDebugDraw(bool enable) { m_debugDraw = enable; }

private:
    void ComputeGlobalPoses();
    void ComputeSkinningMatrices();

    // Skeleton data
    std::shared_ptr<Animation::Skeleton> m_skeleton;
    std::unique_ptr<Animation::SkeletonPose> m_currentPose;

    // Computed poses
    std::vector<Mat4> m_globalPoses;        // World-space bone transforms
    std::vector<Mat4> m_skinningMatrices;   // For GPU skinning
    mutable bool m_posesDirty = true;
    mutable bool m_skinningDirty = true;

    // Bone overrides (for procedural animation)
    struct BoneOverride
    {
        bool hasPosition = false;
        bool hasRotation = false;
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<BoneOverride> m_boneOverrides;

    // Attachments
    struct BoneAttachment
    {
        int boneIndex = -1;
        SceneEntity* entity = nullptr;
        Vec3 localOffset{0.0f};
        Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
    std::vector<BoneAttachment> m_attachments;

    // Settings
    SkeletonUpdateMode m_updateMode = SkeletonUpdateMode::Auto;
    bool m_debugDraw = false;

    // Cached bounds
    mutable AABB m_cachedBounds;
    mutable bool m_boundsDirty = true;
};

} // namespace RVX
