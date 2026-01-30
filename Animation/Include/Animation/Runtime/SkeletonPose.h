/**
 * @file SkeletonPose.h
 * @brief Skeleton pose representation for animation runtime
 * 
 * A pose represents the current state of all bones in a skeleton.
 * Poses can be sampled from animations, blended together, and applied to meshes.
 */

#pragma once

#include "Animation/Core/TransformSample.h"
#include "Animation/Data/Skeleton.h"
#include "Core/MathTypes.h"
#include <vector>
#include <memory>

namespace RVX::Animation
{

/**
 * @brief Represents a pose of a skeleton
 * 
 * Contains local transforms for each bone that can be blended
 * and computed into final skinning matrices.
 */
class SkeletonPose
{
public:
    using Ptr = std::shared_ptr<SkeletonPose>;
    using ConstPtr = std::shared_ptr<const SkeletonPose>;

    // =========================================================================
    // Construction
    // =========================================================================

    SkeletonPose() = default;
    
    /**
     * @brief Create a pose for a skeleton
     * @param skeleton The skeleton this pose is for
     */
    explicit SkeletonPose(Skeleton::ConstPtr skeleton);

    /**
     * @brief Create a pose with a specific number of bones
     */
    explicit SkeletonPose(size_t boneCount);

    /**
     * @brief Factory method
     */
    static Ptr Create(Skeleton::ConstPtr skeleton)
    {
        return std::make_shared<SkeletonPose>(skeleton);
    }

    // =========================================================================
    // Accessors
    // =========================================================================

    /// Get the skeleton this pose is for
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }
    
    /// Set the skeleton reference
    void SetSkeleton(Skeleton::ConstPtr skeleton);

    /// Get number of bones in the pose
    size_t GetBoneCount() const { return m_localTransforms.size(); }

    /// Check if the pose is valid
    bool IsValid() const { return !m_localTransforms.empty(); }

    // =========================================================================
    // Local Transforms (Bone Space)
    // =========================================================================

    /// Get all local transforms
    const std::vector<TransformSample>& GetLocalTransforms() const { return m_localTransforms; }
    std::vector<TransformSample>& GetLocalTransforms() { return m_localTransforms; }

    /// Get a single local transform
    const TransformSample& GetLocalTransform(size_t boneIndex) const;
    TransformSample& GetLocalTransform(size_t boneIndex);

    /// Set a local transform
    void SetLocalTransform(size_t boneIndex, const TransformSample& transform);

    /// Get local transform by bone name
    const TransformSample* GetLocalTransform(const std::string& boneName) const;

    // =========================================================================
    // Global Transforms (Model Space)
    // =========================================================================

    /// Compute global (model space) transforms from local transforms
    void ComputeGlobalTransforms();

    /// Get computed global transforms (call ComputeGlobalTransforms first)
    const std::vector<Mat4>& GetGlobalTransforms() const { return m_globalTransforms; }

    /// Check if global transforms are up to date
    bool AreGlobalTransformsDirty() const { return m_globalsDirty; }

    /// Mark global transforms as dirty (need recomputation)
    void MarkGlobalTransformsDirty() { m_globalsDirty = true; }

    // =========================================================================
    // Skinning Matrices
    // =========================================================================

    /// Compute skinning matrices (global * inverseBindPose)
    void ComputeSkinningMatrices();

    /// Get skinning matrices for GPU upload
    const std::vector<Mat4>& GetSkinningMatrices() const { return m_skinningMatrices; }

    /// Check if skinning matrices are up to date
    bool AreSkinningMatricesDirty() const { return m_skinningDirty; }

    // =========================================================================
    // Pose Operations
    // =========================================================================

    /// Reset to bind pose
    void ResetToBindPose();

    /// Reset to identity pose
    void ResetToIdentity();

    /// Copy from another pose
    void CopyFrom(const SkeletonPose& other);

    // =========================================================================
    // Blending Operations
    // =========================================================================

    /**
     * @brief Blend with another pose
     * @param other The pose to blend with
     * @param weight Blend weight (0 = this, 1 = other)
     */
    void BlendWith(const SkeletonPose& other, float weight);

    /**
     * @brief Static blend between two poses
     */
    static void Blend(const SkeletonPose& a, const SkeletonPose& b, 
                      float weight, SkeletonPose& result);

    /**
     * @brief Additive blend (this + other * weight)
     */
    void AdditivelBlend(const SkeletonPose& additivePose, float weight);

    /**
     * @brief Static additive blend
     */
    static void AdditiveBlend(const SkeletonPose& base, const SkeletonPose& additive,
                               float weight, SkeletonPose& result);

    // =========================================================================
    // Masked Operations
    // =========================================================================

    /**
     * @brief Apply a bone mask during blending
     * @param weights Per-bone blend weights (size must match bone count)
     */
    void BlendWithMask(const SkeletonPose& other, const std::vector<float>& weights);

    /**
     * @brief Copy specific bones from another pose
     * @param other Source pose
     * @param boneIndices Indices of bones to copy
     */
    void CopyBones(const SkeletonPose& other, const std::vector<int>& boneIndices);

private:
    Skeleton::ConstPtr m_skeleton;
    
    std::vector<TransformSample> m_localTransforms;   // Bone-local transforms
    std::vector<Mat4> m_globalTransforms;              // Model-space transforms
    std::vector<Mat4> m_skinningMatrices;              // Final skinning matrices
    
    bool m_globalsDirty = true;
    bool m_skinningDirty = true;
};

} // namespace RVX::Animation
