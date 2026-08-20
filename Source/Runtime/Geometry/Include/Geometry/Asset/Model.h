#pragma once

/**
 * @file Model.h
 * @brief Model container class - holds scene graph and associated data
 *
 * Migrated from found::model::Model
 */

#include "Core/Math/AABB.h"
#include "Geometry/Asset/Material.h"
#include "Geometry/Asset/Mesh.h"
#include "Geometry/Asset/Node.h"
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
         * @brief Find a node by name
         */
        Node::Ptr GetNodeByName(const std::string& name) const;

        // =====================================================================
        // Bounding Box
        // =====================================================================

        /**
         * @brief Resolve indexed node meshes and compute their world-space union.
         *
         * A non-empty explicit mesh-index list is authoritative for a node;
         * otherwise its singular mesh index is used. Duplicate indices within
         * one node are resolved once, while repeated indices on separate nodes
         * remain distinct because each node has its own world transform.
         *
         * @return True only when every referenced mesh resolves to valid bounds
         *         and the model contains at least one indexed mesh bound.
         */
        [[nodiscard]] bool ComputeBoundingBox(const std::vector<Mesh::Ptr>& meshes);
        void SetBoundingBox(const BoundingBox& bbox) { m_bbox = bbox; }
        const BoundingBox& GetBoundingBox() const { return m_bbox; }

        // =====================================================================
        // Animation (optional, requires Animation module)
        // =====================================================================

        bool HasAnimation() const { return m_hasAnimation; }
        void SetHasAnimation(bool has) { m_hasAnimation = has; }

        // Legacy skeleton data
        // Note: For full animation support, include Animation module

    private:
        Node::Ptr m_root;
        BoundingBox m_bbox;
        std::vector<Material::Ptr> m_materials;
        bool m_hasAnimation = false;

        void CollectAllNodesRecursive(const Node::Ptr& node, std::vector<Node::Ptr>& outNodes) const;

        Node::Ptr FindNodeByNameRecursive(const Node::Ptr& node, const std::string& name) const;
    };

} // namespace RVX
