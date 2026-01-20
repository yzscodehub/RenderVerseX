#pragma once

/**
 * @file Node.h
 * @brief Scene graph node with transform and component system
 * 
 * Migrated from found::model::Node
 */

#include "Core/MathTypes.h"
#include "Scene/Mesh.h"
#include "Scene/Material.h"
#include "Scene/BoundingBox.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <optional>
#include <functional>
#include <typeinfo>

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
    // Node Component Base
    // =========================================================================

    /**
     * @brief Base class for node components
     */
    class NodeComponent
    {
    public:
        virtual ~NodeComponent() = default;
        virtual const char* GetTypeName() const = 0;
    };

    // =========================================================================
    // Mesh Component
    // =========================================================================

    /**
     * @brief Mesh rendering component
     */
    class MeshComponent : public NodeComponent
    {
    public:
        explicit MeshComponent(Mesh::Ptr mesh);

        const char* GetTypeName() const override { return "MeshComponent"; }

        Mesh::Ptr GetMesh() const { return m_mesh; }
        void SetMesh(Mesh::Ptr mesh) { m_mesh = std::move(mesh); }

        bool IsVisible() const { return m_visible; }
        void SetVisible(bool visible) { m_visible = visible; }

        bool CastsShadows() const { return m_castShadows; }
        void SetCastsShadows(bool castShadows) { m_castShadows = castShadows; }

        bool ReceivesShadows() const { return m_receiveShadows; }
        void SetReceivesShadows(bool receiveShadows) { m_receiveShadows = receiveShadows; }

    private:
        Mesh::Ptr m_mesh;
        bool m_visible = true;
        bool m_castShadows = true;
        bool m_receiveShadows = true;
    };

    // =========================================================================
    // Bone Component
    // =========================================================================

    /**
     * @brief Bone component - marks a node as part of a skeleton
     */
    class BoneComponent : public NodeComponent
    {
    public:
        explicit BoneComponent(int boneIndex) : m_boneIndex(boneIndex) {}

        const char* GetTypeName() const override { return "BoneComponent"; }

        int GetBoneIndex() const { return m_boneIndex; }
        void SetBoneIndex(int index) { m_boneIndex = index; }

    private:
        int m_boneIndex = -1;
    };

    // =========================================================================
    // Scene Node
    // =========================================================================

    /**
     * @brief Scene graph node
     * 
     * Features:
     * - Hierarchical parent-child relationships
     * - Component-based extension system
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
        // Component System
        // =====================================================================

        template<typename T, typename... Args>
        T* AddComponent(Args&&... args)
        {
            static_assert(std::is_base_of_v<NodeComponent, T>, "T must derive from NodeComponent");
            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            T* result = component.get();
            m_components[typeid(T).name()] = std::move(component);
            return result;
        }

        template<typename T>
        T* GetComponent() const
        {
            static_assert(std::is_base_of_v<NodeComponent, T>, "T must derive from NodeComponent");
            auto it = m_components.find(typeid(T).name());
            return it != m_components.end() ? static_cast<T*>(it->second.get()) : nullptr;
        }

        template<typename T>
        bool HasComponent() const
        {
            return GetComponent<T>() != nullptr;
        }

        template<typename T>
        bool RemoveComponent()
        {
            auto it = m_components.find(typeid(T).name());
            if (it != m_components.end())
            {
                m_components.erase(it);
                return true;
            }
            return false;
        }

        // =====================================================================
        // Mesh Convenience Methods
        // =====================================================================

        void SetMesh(Mesh::Ptr mesh);
        Mesh::Ptr GetMesh() const;
        void RemoveMesh() { RemoveComponent<MeshComponent>(); }

        // =====================================================================
        // Bone Convenience Methods
        // =====================================================================

        void SetBone(int boneIndex);
        int GetBoneIndex() const;
        bool IsBone() const { return HasComponent<BoneComponent>(); }
        void RemoveBone() { RemoveComponent<BoneComponent>(); }

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

    private:
        std::string m_name;
        uint32_t m_id;
        bool m_active = true;

        Transform m_localTransform;
        mutable Mat4 m_worldMatrix{1.0f};
        mutable bool m_worldMatrixDirty = true;

        Node* m_parent = nullptr;
        std::vector<Ptr> m_children;

        std::unordered_map<std::string, std::unique_ptr<NodeComponent>> m_components;

        void UpdateWorldMatrix() const;
        void MarkWorldMatrixDirty();
        static uint32_t GenerateId();
    };

} // namespace RVX
