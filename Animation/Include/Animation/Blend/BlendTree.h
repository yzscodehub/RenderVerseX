/**
 * @file BlendTree.h
 * @brief Blend tree container for complex animation blending
 * 
 * A BlendTree is a container for blend nodes that produces a final pose.
 * It manages parameters, evaluation, and root node execution.
 */

#pragma once

#include "Animation/Blend/BlendNode.h"
#include "Animation/Blend/Blend1D.h"
#include "Animation/Blend/Blend2D.h"
#include "Animation/Runtime/SkeletonPose.h"
#include <memory>
#include <unordered_map>

namespace RVX::Animation
{

/**
 * @brief Blend tree for complex animation blending
 * 
 * The BlendTree manages a hierarchy of blend nodes and provides:
 * - Parameter management (float values that drive blending)
 * - Root node evaluation
 * - Caching and optimization
 * 
 * Usage:
 * @code
 * BlendTree tree(skeleton);
 * 
 * // Create a 1D blend for locomotion
 * auto locomotion = std::make_shared<Blend1D>("Speed");
 * locomotion->AddClip(0.0f, idleClip);
 * locomotion->AddClip(1.0f, walkClip);
 * locomotion->AddClip(2.0f, runClip);
 * 
 * tree.SetRootNode(locomotion);
 * 
 * // Each frame
 * tree.SetParameter("Speed", currentSpeed);
 * tree.Update(deltaTime);
 * 
 * // Get the result
 * const SkeletonPose& pose = tree.GetOutputPose();
 * @endcode
 */
class BlendTree
{
public:
    using Ptr = std::shared_ptr<BlendTree>;

    // =========================================================================
    // Construction
    // =========================================================================

    BlendTree() = default;
    explicit BlendTree(Skeleton::ConstPtr skeleton);

    static Ptr Create(Skeleton::ConstPtr skeleton)
    {
        return std::make_shared<BlendTree>(skeleton);
    }

    // =========================================================================
    // Skeleton
    // =========================================================================

    void SetSkeleton(Skeleton::ConstPtr skeleton);
    Skeleton::ConstPtr GetSkeleton() const { return m_skeleton; }

    // =========================================================================
    // Root Node
    // =========================================================================

    /// Set the root blend node
    void SetRootNode(BlendNodePtr root);
    BlendNodePtr GetRootNode() const { return m_rootNode; }

    /// Check if tree has a valid root
    bool HasRoot() const { return m_rootNode != nullptr; }

    // =========================================================================
    // Parameters
    // =========================================================================

    /// Set a float parameter
    void SetParameter(const std::string& name, float value);

    /// Get a parameter value
    float GetParameter(const std::string& name, float defaultValue = 0.0f) const;

    /// Check if parameter exists
    bool HasParameter(const std::string& name) const;

    /// Get all parameter names
    std::vector<std::string> GetParameterNames() const;

    /// Clear all parameters
    void ClearParameters();

    // =========================================================================
    // Evaluation
    // =========================================================================

    /**
     * @brief Update the blend tree
     * @param deltaTime Time since last update in seconds
     */
    void Update(float deltaTime);

    /**
     * @brief Get the output pose
     */
    const SkeletonPose& GetOutputPose() const { return m_outputPose; }
    SkeletonPose& GetOutputPose() { return m_outputPose; }

    /**
     * @brief Reset the tree (restart all animations)
     */
    void Reset();

    /**
     * @brief Get the duration of the blend tree in seconds
     * @return Maximum duration from all clips in the tree, or 0 if empty
     */
    float GetDuration() const;

    // =========================================================================
    // Utility
    // =========================================================================

    /// Find a node by name
    BlendNode* FindNode(const std::string& name) const;

    /// Get the blend context (for debugging)
    const BlendContext& GetContext() const { return m_context; }

private:
    BlendNode* FindNodeRecursive(BlendNode* node, const std::string& name) const;

    Skeleton::ConstPtr m_skeleton;
    BlendNodePtr m_rootNode;
    BlendContext m_context;
    SkeletonPose m_outputPose;
};

} // namespace RVX::Animation
