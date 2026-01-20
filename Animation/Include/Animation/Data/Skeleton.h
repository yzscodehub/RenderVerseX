/**
 * @file Skeleton.h
 * @brief Skeleton definition for skeletal animation
 * 
 * Migrated from found::animation::Skeleton
 */

#pragma once

#include "Core/MathTypes.h"
#include "Animation/Core/TransformSample.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Animation
{

/**
 * @brief Single bone definition in a skeleton
 */
struct Bone
{
    /// Unique bone name
    std::string name;

    /// Index of parent bone (-1 for root bones)
    int parentIndex{-1};

    /// Indices of child bones
    std::vector<int> childIndices;

    /// Local bind pose transform
    TransformSample localBindPose;

    /// Inverse of the global bind pose matrix (for skinning)
    Mat4 inverseBindPose{1.0f};

    /// Bounding radius for culling
    float boundingRadius{0.0f};

    Bone() = default;
    Bone(const std::string& boneName, int parent = -1)
        : name(boneName), parentIndex(parent) {}

    bool IsRoot() const { return parentIndex < 0; }
    bool IsLeaf() const { return childIndices.empty(); }
};

/**
 * @brief Skeleton definition containing hierarchical bone structure
 */
struct Skeleton
{
    using Ptr = std::shared_ptr<Skeleton>;
    using ConstPtr = std::shared_ptr<const Skeleton>;

    /// All bones in topological order (parents before children)
    std::vector<Bone> bones;

    /// Name to index mapping
    std::unordered_map<std::string, int> boneNameMap;

    /// Indices of root bones
    std::vector<int> rootBoneIndices;

    // ========================================================================
    // Construction
    // ========================================================================

    Skeleton() = default;

    void Reserve(size_t boneCount)
    {
        bones.reserve(boneCount);
        boneNameMap.reserve(boneCount);
    }

    int AddBone(const Bone& bone)
    {
        int index = static_cast<int>(bones.size());
        bones.push_back(bone);
        boneNameMap[bone.name] = index;

        if (bone.IsRoot())
        {
            rootBoneIndices.push_back(index);
        }
        else if (bone.parentIndex >= 0 && bone.parentIndex < static_cast<int>(bones.size()))
        {
            bones[bone.parentIndex].childIndices.push_back(index);
        }

        return index;
    }

    int AddBone(const std::string& name, int parentIndex = -1)
    {
        return AddBone(Bone(name, parentIndex));
    }

    void BuildHierarchy()
    {
        rootBoneIndices.clear();
        for (auto& bone : bones) bone.childIndices.clear();

        for (size_t i = 0; i < bones.size(); ++i)
        {
            const auto& bone = bones[i];
            if (bone.IsRoot())
            {
                rootBoneIndices.push_back(static_cast<int>(i));
            }
            else if (bone.parentIndex >= 0 && bone.parentIndex < static_cast<int>(i))
            {
                bones[bone.parentIndex].childIndices.push_back(static_cast<int>(i));
            }
        }
    }

    // ========================================================================
    // Lookup
    // ========================================================================

    size_t GetBoneCount() const { return bones.size(); }
    bool IsEmpty() const { return bones.empty(); }

    int FindBoneIndex(const std::string& name) const
    {
        auto it = boneNameMap.find(name);
        return (it != boneNameMap.end()) ? it->second : -1;
    }

    const Bone* GetBone(int index) const
    {
        if (index >= 0 && index < static_cast<int>(bones.size()))
            return &bones[index];
        return nullptr;
    }

    Bone* GetBone(int index)
    {
        if (index >= 0 && index < static_cast<int>(bones.size()))
            return &bones[index];
        return nullptr;
    }

    const Bone* GetBone(const std::string& name) const
    {
        return GetBone(FindBoneIndex(name));
    }

    // ========================================================================
    // Hierarchy Queries
    // ========================================================================

    bool IsAncestor(int ancestorIndex, int descendantIndex) const
    {
        if (ancestorIndex < 0 || descendantIndex < 0) return false;

        int current = descendantIndex;
        while (current >= 0)
        {
            if (current == ancestorIndex) return true;
            const Bone* bone = GetBone(current);
            if (!bone) break;
            current = bone->parentIndex;
        }
        return false;
    }

    int GetBoneDepth(int boneIndex) const
    {
        int depth = 0;
        int current = boneIndex;
        while (current >= 0)
        {
            const Bone* bone = GetBone(current);
            if (!bone) break;
            current = bone->parentIndex;
            depth++;
        }
        return depth - 1;
    }

    // ========================================================================
    // Bind Pose Computation
    // ========================================================================

    std::vector<Mat4> ComputeGlobalBindPoses() const
    {
        std::vector<Mat4> globalPoses(bones.size(), Mat4(1.0f));

        for (size_t i = 0; i < bones.size(); ++i)
        {
            const Bone& bone = bones[i];
            Mat4 local = bone.localBindPose.ToMatrix();

            if (bone.parentIndex >= 0 && bone.parentIndex < static_cast<int>(i))
            {
                globalPoses[i] = globalPoses[bone.parentIndex] * local;
            }
            else
            {
                globalPoses[i] = local;
            }
        }

        return globalPoses;
    }

    void ComputeInverseBindPoses()
    {
        std::vector<Mat4> globalPoses = ComputeGlobalBindPoses();

        for (size_t i = 0; i < bones.size(); ++i)
        {
            bones[i].inverseBindPose = inverse(globalPoses[i]);
        }
    }

    // ========================================================================
    // Validation
    // ========================================================================

    bool Validate() const
    {
        for (size_t i = 0; i < bones.size(); ++i)
        {
            int parent = bones[i].parentIndex;
            if (parent >= static_cast<int>(i)) return false;
            if (parent >= 0 && parent >= static_cast<int>(bones.size())) return false;
        }
        return boneNameMap.size() == bones.size();
    }

    // ========================================================================
    // Factory
    // ========================================================================

    static Ptr Create() { return std::make_shared<Skeleton>(); }
};

} // namespace RVX::Animation
