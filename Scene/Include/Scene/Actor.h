#pragma once

/**
 * @file Actor.h
 * @brief UE-style object that owns actor components
 */

#include "Core/MathTypes.h"
#include "Scene/ActorComponent.h"
#include "Spatial/Index/ISpatialEntity.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace RVX
{
    class SceneComponent;

    /**
     * @brief Base scene object for the UE-style component system.
     *
     * Actor owns ActorComponent instances, dispatches lifecycle events, and
     * stores a root SceneComponent pointer for transform hierarchies.
     */
    class Actor
    {
    public:
        using Handle = Spatial::EntityHandle;

        static constexpr Handle InvalidHandle = Spatial::InvalidHandle;

        // =====================================================================
        // Construction
        // =====================================================================

        explicit Actor(const std::string& name = "Actor");
        virtual ~Actor();

        Actor(const Actor&) = delete;
        Actor& operator=(const Actor&) = delete;
        Actor(Actor&&) = delete;
        Actor& operator=(Actor&&) = delete;

        // =====================================================================
        // Type Information
        // =====================================================================

        virtual const char* GetClassName() const { return "Actor"; }

        // =====================================================================
        // Identity and State
        // =====================================================================

        virtual Handle GetHandle() const { return m_handle; }
        virtual const std::string& GetName() const { return m_name; }
        virtual void SetName(const std::string& name) { m_name = name; }

        virtual bool IsActive() const { return m_active; }
        virtual void SetActive(bool active) { m_active = active; }

        // =====================================================================
        // Components
        // =====================================================================

        template<typename T, typename... Args>
        T* AddComponent(Args&&... args);

        /// Add an already-created pure actor component and transfer ownership.
        /// Legacy Component subclasses must use SceneEntity::AddComponent<T>().
        ActorComponent* AddOwnedComponent(std::unique_ptr<ActorComponent> component);

        template<typename T>
        T* GetComponent() const;

        template<typename T>
        bool RemoveComponent();

        const std::vector<std::unique_ptr<ActorComponent>>& GetActorComponents() const
        {
            return m_components;
        }

        size_t GetActorComponentCount() const { return m_components.size(); }

        // =====================================================================
        // Root Component
        // =====================================================================

        SceneComponent* GetRootComponent() const { return m_rootComponent; }
        virtual void SetRootComponent(SceneComponent* rootComponent);

        // =====================================================================
        // Transform Forwarding
        // =====================================================================

        virtual Vec3 GetWorldPosition() const;
        virtual Quat GetWorldRotation() const;
        virtual Vec3 GetWorldScale() const;
        virtual Mat4 GetWorldMatrix() const;

        virtual void SetPosition(const Vec3& position);
        virtual void SetRotation(const Quat& rotation);
        virtual void SetScale(const Vec3& scale);
        virtual void Translate(const Vec3& delta);

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void RegisterAllComponents();
        void UnregisterAllComponents();
        bool HasBegunPlay() const { return m_hasBegunPlay; }
        void BeginPlay();
        void Tick(float deltaTime);
        void EndPlay();

    protected:
        virtual bool ShouldAutoRegisterComponent(ActorComponent* component) const;

    private:
        bool RemoveComponentInstance(ActorComponent* component);
        void DestroyAllComponents();

        Handle m_handle = InvalidHandle;
        std::string m_name;
        bool m_active = true;
        bool m_hasBegunPlay = false;
        SceneComponent* m_rootComponent = nullptr;
        std::vector<std::unique_ptr<ActorComponent>> m_components;

        static Handle GenerateHandle();
        static std::atomic<Handle> s_nextHandle;
    };

    // =========================================================================
    // Template Implementations
    // =========================================================================

    template<typename T, typename... Args>
    T* Actor::AddComponent(Args&&... args)
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = component.get();
        ptr->SetOwnerActor(this);
        m_components.push_back(std::move(component));
        ptr->OnComponentCreated();

        if (ShouldAutoRegisterComponent(ptr))
        {
            ptr->SetRegistered(true);
            ptr->OnRegister();
            if (!ptr->IsInitialized())
            {
                ptr->InitializeComponent();
                ptr->SetInitialized(true);
            }
        }

        return ptr;
    }

    template<typename T>
    T* Actor::GetComponent() const
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        for (const auto& component : m_components)
        {
            if (auto* typed = dynamic_cast<T*>(component.get()))
            {
                return typed;
            }
        }
        return nullptr;
    }

    template<typename T>
    bool Actor::RemoveComponent()
    {
        static_assert(std::is_base_of_v<ActorComponent, T>, "T must derive from ActorComponent");

        auto it = std::find_if(m_components.begin(), m_components.end(), [](const auto& component) {
            return dynamic_cast<T*>(component.get()) != nullptr;
        });

        if (it == m_components.end())
            return false;

        return RemoveComponentInstance(it->get());
    }

} // namespace RVX
