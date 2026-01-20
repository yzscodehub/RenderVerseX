#pragma once

/**
 * @file Model.h
 * @brief Model container class - holds scene graph and associated data
 * 
 * Migrated from found::model::Model
 */

#include "Scene/Node.h"
#include "Scene/BoundingBox.h"
#include <vector>
#include <memory>
#include <string>

namespace RVX
{
    // Forward declarations for animation system
    namespace Animation
    {
        struct Skeleton;
        class AnimationLibrary;
        class SkinBindingCollection;
    }

    /**
     * @brief Model class - container for scene graph and related data
     * 
     * Holds:
     * - Scene node hierarchy (tree structure)
     * - Materials (optional)
     * - Skeleton and animation data (optional)
     * - Bounding box
     */
    class Model
    {
    public:
        using Ptr = std::shared_ptr<Model>;
        
        Model() = default;
        ~Model() = default;

        // =====================================================================
        // File Info
        // =====================================================================

        std::string suffix;  // File extension
        std::string path;    // File path

        // =====================================================================
        // Scene Graph
        // =====================================================================

        Node::Ptr GetRootNode() const { return m_root; }
        void SetRootNode(Node::Ptr root) { m_root = std::move(root); }

        /**
         * @brief Collect all nodes in the scene graph
         */
        std::vector<Node::Ptr> GetAllNodes() const;

        /**
         * @brief Collect all nodes that have a specific component type
         */
        template<typename T>
        void CollectNodesWithComponent(std::vector<Node::Ptr>& outNodes) const
        {
            if (m_root)
            {
                CollectNodesWithComponentRecursive<T>(m_root, outNodes);
            }
        }

        /**
         * @brief Collect all nodes with mesh components
         */
        void CollectMeshNodes(std::vector<Node::Ptr>& outNodes) const
        {
            CollectNodesWithComponent<MeshComponent>(outNodes);
        }

        /**
         * @brief Find a node by name
         */
        Node::Ptr GetNodeByName(const std::string& name) const;

        // =====================================================================
        // Bounding Box
        // =====================================================================

        void ComputeBoundingBox();
        void SetBoundingBox(const BoundingBox& bbox) { m_bbox = bbox; }
        const BoundingBox& GetBoundingBox() const { return m_bbox; }

        // =====================================================================
        // Animation (optional, requires Animation module)
        // =====================================================================

        bool HasAnimation() const { return m_hasAnimation; }
        void SetHasAnimation(bool has) { m_hasAnimation = has; }

        // Legacy skeleton data
        // Note: For full animation support, include Animation module

        // =====================================================================
        // Bone Queries
        // =====================================================================

        /**
         * @brief Get all bone nodes in the scene graph
         */
        std::vector<Node::Ptr> GetBoneNodes() const;

    private:
        Node::Ptr m_root;
        BoundingBox m_bbox;
        std::vector<Material::Ptr> m_materials;
        bool m_hasAnimation = false;

        void CollectAllNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const;

        template<typename T>
        void CollectNodesWithComponentRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const
        {
            if (node->HasComponent<T>())
            {
                outNodes.push_back(node);
            }
            for (const auto& child : node->GetChildren())
            {
                CollectNodesWithComponentRecursive<T>(child, outNodes);
            }
        }

        void CollectBoneNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const;
        Node::Ptr FindNodeByNameRecursive(const Node::Ptr& node, const std::string& name) const;
    };

} // namespace RVX
