#pragma once

/**
 * @file SceneEntity.h
 * @brief Base class for all scene entities
 * 
 * SceneEntity implements ISpatialEntity for spatial indexing integration.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Scene/Actor.h"
#include "Scene/Component.h"
#include "Scene/SceneComponent.h"
#include "Spatial/Index/ISpatialEntity.h"

#include <atomic>
#include <memory>
#include <string>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace RVX
{
    // Forward declarations
    class Transform;
    class SceneManager;
    class Component;

    /**
     * @brief Entity type enumeration
     */
    enum class EntityType : uint8_t
    {
        Node = 0,
        StaticMesh,
        SkeletalMesh,
        Light,
        Camera,
        Probe,
        Decal,
        Custom
    };

    /**
     * @brief Base class for all scene entities
     * 
     * Implements ISpatialEntity for integration with spatial indexing.
     * All renderable objects in the scene should derive from this class.
     */
    class SceneEntity : public Actor, public Spatial::ISpatialEntity
    {
    public:
        using Handle = Spatial::EntityHandle;
        using Ptr = std::shared_ptr<SceneEntity>;
        using WeakPtr = std::weak_ptr<SceneEntity>;

        static constexpr Handle InvalidHandle = Spatial::InvalidHandle;

        // =====================================================================
        // Construction
        // =====================================================================

        explicit SceneEntity(const std::string& name = "Entity");
        ~SceneEntity() override;

        // Non-copyable, non-movable. SceneEntity identity is address-stable for
        // component owner pointers, parent/child links, and spatial index users.
        SceneEntity(const SceneEntity&) = delete;
        SceneEntity& operator=(const SceneEntity&) = delete;
        SceneEntity(SceneEntity&&) = delete;
        SceneEntity& operator=(SceneEntity&&) = delete;

        // =====================================================================
        // ISpatialEntity Implementation
        // =====================================================================

        Handle GetHandle() const override { return m_handle; }
        AABB GetWorldBounds() const override;
        uint32_t GetLayerMask() const override { return m_layerMask; }
        uint32_t GetTypeMask() const override { return 1u << static_cast<uint8_t>(GetEntityType()); }
        bool IsSpatialDirty() const override { return m_spatialDirty; }
        void ClearSpatialDirty() override { m_spatialDirty = false; }
        void* GetUserData() const override { return m_userData; }

        // =====================================================================
        // Basic Properties
        // =====================================================================

        const std::string& GetName() const override { return m_name; }
        void SetName(const std::string& name) override
        {
            Actor::SetName(name);
            m_name = name;
        }

        const char* GetClassName() const override { return "SceneEntity"; }
        virtual EntityType GetEntityType() const { return EntityType::Node; }

        bool IsActive() const override { return m_active; }
        void SetActive(bool active) override
        {
            Actor::SetActive(active);
            m_active = active;
        }

        void SetLayerMask(uint32_t mask) { m_layerMask = mask; }
        void SetLayer(uint32_t layer) { m_layerMask = 1u << layer; }
        void AddLayer(uint32_t layer) { m_layerMask |= (1u << layer); }
        void RemoveLayer(uint32_t layer) { m_layerMask &= ~(1u << layer); }
        bool IsInLayer(uint32_t layer) const { return (m_layerMask & (1u << layer)) != 0; }

        void SetUserData(void* data) { m_userData = data; }

        // =====================================================================
        // Transform (Local - relative to parent)
        // =====================================================================

        const Vec3& GetPosition() const;
        void SetPosition(const Vec3& position) override;

        const Quat& GetRotation() const;
        void SetRotation(const Quat& rotation) override;

        const Vec3& GetScale() const;
        void SetScale(const Vec3& scale) override;

        /// Get local transform matrix (relative to parent)
        Mat4 GetLocalMatrix() const;

        /// Get world transform matrix (includes parent transforms)
        Mat4 GetWorldMatrix() const override;

        /// Get world position (includes parent transforms)
        Vec3 GetWorldPosition() const override;

        /// Get world rotation (includes parent transforms)
        Quat GetWorldRotation() const override;

        /// Get world scale (includes parent transforms)
        Vec3 GetWorldScale() const override;

        void Translate(const Vec3& delta) override;
        void Rotate(const Quat& delta);
        void RotateAround(const Vec3& axis, float angle);

        // =====================================================================
        // Bounds (Hybrid Mode - bounds come from components)
        // =====================================================================

        /// Set manual local bounds (optional, components can provide bounds)
        void SetLocalBounds(const AABB& bounds);
        const AABB& GetLocalBounds() const { return m_localBounds; }

        /// Mark bounds as needing recalculation
        void MarkBoundsDirty();

        /// Compute bounds from all components that provide bounds
        /// (Called automatically by GetWorldBounds when dirty)
        AABB ComputeBoundsFromComponents() const;

        // =====================================================================
        // Scene Management
        // =====================================================================

        SceneManager* GetSceneManager() const { return m_sceneManager; }

        // =====================================================================
        // Hierarchy
        // =====================================================================

        /// Get parent entity (nullptr if root)
        SceneEntity* GetParent() const { return m_parent; }

        /// Get all child entities
        const std::vector<SceneEntity*>& GetChildren() const { return m_children; }

        /// Get child count
        size_t GetChildCount() const { return m_children.size(); }

        /// Add a child entity
        void AddChild(SceneEntity* child);

        /// Remove a child entity
        bool RemoveChild(SceneEntity* child);

        /// Set parent (nullptr to make root)
        void SetParent(SceneEntity* parent);

        /// Check if this entity is a root (no parent)
        bool IsRoot() const { return m_parent == nullptr; }

        /// Check if this entity is an ancestor of another
        bool IsAncestorOf(const SceneEntity* entity) const;

        /// Check if this entity is a descendant of another
        bool IsDescendantOf(const SceneEntity* entity) const;

        /// Get root entity of this hierarchy
        SceneEntity* GetRoot();
        const SceneEntity* GetRoot() const;

        // =====================================================================
        // Component System
        // =====================================================================

        /// Add a component of type T
        template<typename T, typename... Args>
        T* AddComponent(Args&&... args);

        /// Add an already-created legacy component and transfer ownership.
        Component* AddOwnedComponent(std::unique_ptr<Component> component);

        /// Get a component of type T (returns nullptr if not found)
        template<typename T>
        T* GetComponent() const;

        /// Check if entity has a component of type T
        template<typename T>
        bool HasComponent() const;

        /// Remove a component of type T
        template<typename T>
        bool RemoveComponent();

        /// Get all components
        const std::unordered_map<std::type_index, std::unique_ptr<Component>>& GetComponents() const
        {
            return m_components;
        }

        /// Get component count
        size_t GetComponentCount() const { return m_components.size(); }

        /// Tick all components
        void TickComponents(float deltaTime);

        void SetRootComponent(SceneComponent* rootComponent) override;
        void NotifySceneComponentTransformChanged(SceneComponent* component);

    protected:
        bool ShouldAutoRegisterComponent(ActorComponent* component) const override;
        void MarkSpatialDirty() { m_spatialDirty = true; }
        void MarkTransformDirty();  // Also marks children as dirty

        // Allow SceneManager to set itself
        friend class SceneManager;
        void SetSceneManager(SceneManager* manager) { m_sceneManager = manager; }

    private:
        // Identity
        Handle m_handle;
        std::string m_name;
        bool m_active = true;

        // Filtering
        uint32_t m_layerMask = ~0u;

        // Transform
        Vec3 m_position{0.0f};
        Quat m_rotation{1.0f, 0.0f, 0.0f, 0.0f};
        Vec3 m_scale{1.0f};
        mutable Mat4 m_worldMatrix{1.0f};
        mutable bool m_transformDirty = true;

        // Bounds (hybrid mode with caching)
        AABB m_localBounds;
        mutable AABB m_cachedWorldBounds;
        mutable bool m_boundsDirty = true;
        mutable bool m_spatialDirty = true;

        // Scene manager reference
        SceneManager* m_sceneManager = nullptr;

        // User data
        void* m_userData = nullptr;

        // Hierarchy
        SceneEntity* m_parent = nullptr;
        std::vector<SceneEntity*> m_children;

        // Components
        std::unordered_map<std::type_index, std::unique_ptr<Component>> m_components;
        SceneComponent* m_compatRootComponent = nullptr;

        // Helper to mark all children's transforms as dirty
        void MarkChildrenTransformDirty();

        // Handle generation
        static Handle GenerateHandle();
        static std::atomic<Handle> s_nextHandle;
    };

    // =========================================================================
    // Template Implementations (must be in header)
    // =========================================================================

    template<typename T, typename... Args>
    T* SceneEntity::AddComponent(Args&&... args)
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        if constexpr (std::is_base_of_v<Component, T>)
        {
            std::type_index typeIndex(typeid(T));

            // Check if component already exists
            auto it = m_components.find(typeIndex);
            if (it != m_components.end())
            {
                return static_cast<T*>(it->second.get());
            }

            // Create new component
            auto component = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = component.get();

            // Set owner and call OnAttach
            component->SetOwner(this);
            component->OnComponentCreated();
            component->OnAttach();

            // Store component
            m_components[typeIndex] = std::move(component);

            // Notify bounds may have changed
            MarkBoundsDirty();

            return ptr;
        }
        else
        {
            return Actor::AddComponent<T>(std::forward<Args>(args)...);
        }
    }

    template<typename T>
    T* SceneEntity::GetComponent() const
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        if constexpr (std::is_base_of_v<Component, T>)
        {
            std::type_index typeIndex(typeid(T));
            auto it = m_components.find(typeIndex);
            if (it != m_components.end())
            {
                return static_cast<T*>(it->second.get());
            }
            return nullptr;
        }
        else
        {
            return Actor::GetComponent<T>();
        }
    }

    template<typename T>
    bool SceneEntity::HasComponent() const
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        if constexpr (std::is_base_of_v<Component, T>)
        {
            std::type_index typeIndex(typeid(T));
            return m_components.find(typeIndex) != m_components.end();
        }
        else
        {
            return Actor::GetComponent<T>() != nullptr;
        }
    }

    template<typename T>
    bool SceneEntity::RemoveComponent()
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        if constexpr (std::is_base_of_v<Component, T>)
        {
            std::type_index typeIndex(typeid(T));
            auto it = m_components.find(typeIndex);
            if (it != m_components.end())
            {
                // Call OnDetach before removal
                it->second->OnDetach();
                it->second->OnComponentDestroyed();
                it->second->SetOwner(nullptr);
                m_components.erase(it);

                // Notify bounds may have changed
                MarkBoundsDirty();
                return true;
            }
            return false;
        }
        else
        {
            return Actor::RemoveComponent<T>();
        }
    }

} // namespace RVX
