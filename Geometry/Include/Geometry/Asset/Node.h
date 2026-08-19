#pragma once

/**
 * @file Node.h
 * @brief Scene graph node with transform and indexed resource identity
 *
 * Migrated from found::model::Node
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
    // Forward declarations
    class Node;

    // =========================================================================
    // Transform Component
    // =========================================================================

    /**
     * @brief Transform component - handles position, rotation, scale
     */
    class Transform
    {
    public:
        Transform() = default;

        // TRS constructor
        Transform(const Vec3& position, const Quat& rotation = Quat(1, 0, 0, 0),
                 const Vec3& scale = Vec3(1.0f));

        // Matrix constructor
        explicit Transform(const Mat4& matrix);

        // =====================================================================
        // Position
        // =====================================================================

        const Vec3& GetPosition() const;
        void SetPosition(const Vec3& position);
        void Translate(const Vec3& translation);

        // =====================================================================
        // Rotation
        // =====================================================================

        const Quat& GetRotation() const;
        void SetRotation(const Quat& rotation);
        void SetRotation(const Vec3& euler);
        void Rotate(const Quat& rotation);
        void Rotate(const Vec3& axis, float angle);

        // =====================================================================
        // Scale
        // =====================================================================

        const Vec3& GetScale() const;
        void SetScale(const Vec3& scale);
        void SetScale(float uniformScale);

        // =====================================================================
        // Matrix
        // =====================================================================

        const Mat4& GetMatrix() const;
        void SetMatrix(const Mat4& matrix);

        // =====================================================================
        // Utility
        // =====================================================================

        Vec3 GetForward() const;
        Vec3 GetRight() const;
        Vec3 GetUp() const;
        void LookAt(const Vec3& target, const Vec3& up = Vec3(0, 1, 0));

        bool IsDirty() const { return m_dirty || m_dirtyTrs; }
        void MarkClean() const { m_dirty = m_dirtyTrs = false; }

    private:
        mutable Vec3 m_position{0.0f};
        mutable Quat m_rotation{1, 0, 0, 0};
        mutable Vec3 m_scale{1.0f};
        mutable Mat4 m_matrix{1.0f};
        mutable bool m_dirty = false;      // TRS -> Matrix needs update
        mutable bool m_dirtyTrs = false;   // Matrix -> TRS needs update

        void UpdateMatrix() const;
        void UpdateTRS() const;
        static void DecomposeMatrix(const Mat4& matrix, Vec3& position,
                                   Quat& rotation, Vec3& scale);
    };

    // =========================================================================
    // Scene Node
    // =========================================================================

    /**
     * @brief Scene graph node
     *
     * Features:
     * - Hierarchical parent-child relationships
     * - Indexed mesh/material/skin source identity
     * - Cached world matrix computation
     * - Traversal methods (depth-first, breadth-first)
     */
    class Node
    {
    public:
        using Ptr = std::shared_ptr<Node>;
        using ConstPtr = std::shared_ptr<const Node>;
        using WeakPtr = std::weak_ptr<Node>;

        // =====================================================================
        // Construction
        // =====================================================================

        explicit Node(const std::string& name = "Node");
        ~Node() = default;

        Node(Node&&) = default;
        Node& operator=(Node&&) = default;
        Node(const Node&) = delete;
        Node& operator=(const Node&) = delete;

        // =====================================================================
        // Basic Properties
        // =====================================================================

        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& name) { m_name = name; }

        uint32_t GetId() const { return m_id; }

        bool IsActive() const { return m_active; }
        void SetActive(bool active) { m_active = active; }

        // =====================================================================
        // Transform
        // =====================================================================

        Transform& GetLocalTransform() { return m_localTransform; }
        const Transform& GetLocalTransform() const { return m_localTransform; }

        const Mat4& GetWorldMatrix() const;
        Vec3 GetWorldPosition() const;
        Quat GetWorldRotation() const;
        Vec3 GetWorldScale() const;

        // =====================================================================
        // Hierarchy
        // =====================================================================

        Node* GetParent() const { return m_parent; }
        const std::vector<Ptr>& GetChildren() const { return m_children; }
        size_t GetChildCount() const { return m_children.size(); }

        Node* GetChild(size_t index) const;
        Node* GetChild(const std::string& name) const;
        void AddChild(Ptr child);
        bool RemoveChild(Node* child);
        void RemoveFromParent();

        // =====================================================================
        // Traversal
        // =====================================================================

        void TraverseDepthFirst(const std::function<void(Node*)>& visitor);
        void TraverseBreadthFirst(const std::function<void(Node*)>& visitor);

        // =====================================================================
        // Search
        // =====================================================================

        Node* FindChild(const std::string& name, bool recursive = false) const;
        Node* FindChildByPath(const std::string& path) const;

        // =====================================================================
        // Utility
        // =====================================================================

        std::string GetPath() const;
        size_t GetDepth() const;
        bool IsAncestorOf(const Node* node) const;
        bool IsDescendantOf(const Node* node) const;

        // =====================================================================
        // Bounding Box
        // =====================================================================

        std::optional<BoundingBox> ComputeWorldBoundingBox() const;

        // =====================================================================
        // Resource Indices (for Prefab/Instantiate pattern)
        // =====================================================================

        /// Get mesh index (-1 means no mesh), references ModelResource.meshes[]
        int GetMeshIndex() const { return m_meshIndex; }
        void SetMeshIndex(int index) { m_meshIndex = index; }

        /// Get the explicit engine mesh index for every source primitive.
        /// An empty list preserves the historical single-mesh/submesh contract
        /// represented by GetMeshIndex() and GetMaterialIndices().
        const std::vector<int>& GetMeshIndices() const { return m_meshIndices; }
        void SetMeshIndices(std::vector<int> indices)
        {
            m_meshIndices = std::move(indices);
        }
        bool HasExplicitMeshIndices() const { return !m_meshIndices.empty(); }

        /// Get material indices for each submesh, references ModelResource.materials[]
        const std::vector<int>& GetMaterialIndices() const { return m_materialIndices; }
        void SetMaterialIndices(const std::vector<int>& indices) { m_materialIndices = indices; }
        void SetMaterialIndex(size_t submeshIndex, int materialIndex);

        /// Check if this node uses index-based resource references
        bool UsesIndexMode() const
        {
            return m_meshIndex >= 0 || HasExplicitMeshIndices();
        }

        /// Check if this node has indexed mesh data.
        bool HasMeshData() const
        {
            return UsesIndexMode();
        }

        /// glTF skin index (-1 when this node is not skinned).  This remains a
        /// source-hierarchy property so ModelResource instantiation can bind the
        /// canonical skeleton without guessing from vertex attributes.
        int GetSkinIndex() const { return m_skinIndex; }
        void SetSkinIndex(int index) { m_skinIndex = index; }
        bool HasSkin() const { return m_skinIndex >= 0; }

    private:
        std::string m_name;
        uint32_t m_id;
        bool m_active = true;

        Transform m_localTransform;
        mutable Mat4 m_worldMatrix{1.0f};
        mutable bool m_worldMatrixDirty = true;

        Node* m_parent = nullptr;
        std::vector<Ptr> m_children;

        // Resource indices (for Prefab/Instantiate pattern)
        int m_meshIndex = -1;                   ///< Index into ModelResource.meshes[], -1 = no mesh
        std::vector<int> m_meshIndices;         ///< Explicit index per source primitive (optional)
        std::vector<int> m_materialIndices;    ///< Per-submesh material indices into ModelResource.materials[]
        int m_skinIndex = -1;                   ///< Source glTF skin index, -1 = no skin

        void UpdateWorldMatrix() const;
        void MarkWorldMatrixDirty();
        static uint32_t GenerateId();
    };

} // namespace RVX
