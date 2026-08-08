#pragma once

/**
 * @file ActorComponent.h
 * @brief UE-style base class for actor-owned components
 */

#include "Core/Types.h"
#include "Scene/SceneIdentity.h"
#include "Scene/SceneSystemScheduler.h"

#include <string>
#include <utility>

namespace RVX
{
    class Actor;
    class Scene;

    /**
     * @brief Base class for components owned by an Actor.
     *
     * ActorComponent stores owner, registration, enabled, and tick state. Higher
     * level subclasses add transform or render behavior.
     */
    class ActorComponent
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        using ComponentId = PersistentComponentId;

        static constexpr ComponentId InvalidComponentId = InvalidPersistentComponentId;

        ActorComponent();
        virtual ~ActorComponent() = default;

        ActorComponent(const ActorComponent&) = delete;
        ActorComponent& operator=(const ActorComponent&) = delete;

        // =====================================================================
        // Type Information
        // =====================================================================

        virtual const char* GetClassName() const { return "ActorComponent"; }

        // =====================================================================
        // Prefab Serialization
        // =====================================================================

        virtual std::string SerializePrefabData() const { return {}; }
        virtual void DeserializePrefabData(const std::string& data) { (void)data; }

        // =====================================================================
        // Owner and State
        // =====================================================================

        ComponentId GetComponentId() const { return m_componentId; }
        void SetComponentIdForSerialization(ComponentId componentId);
        ComponentHandle GetComponentHandle() const { return m_componentHandle; }

        const std::string& GetName() const { return m_name; }
        void SetName(std::string name) { m_name = std::move(name); }

        Actor* GetOwner() const { return m_owner; }

        bool IsInitialized() const { return m_initialized; }
        bool IsRegistered() const { return m_registered; }
        bool HasBegunPlay() const { return m_hasBegunPlay; }
        bool IsEnabled() const { return m_enabled; }
        virtual void SetEnabled(bool enabled);

        bool CanEverTick() const { return m_canEverTick; }
        void SetCanEverTick(bool canEverTick);

        bool IsTickEnabled() const { return m_tickEnabled; }
        void SetTickEnabled(bool enabled);

        // =====================================================================
        // Lifecycle
        // =====================================================================

        virtual void OnComponentCreated() {}
        virtual void OnRegister() {}
        virtual void InitializeComponent() {}
        virtual void BeginPlay() {}
        virtual void TickComponent(float deltaTime) { (void)deltaTime; }
        virtual void EndPlay() {}
        virtual void OnUnregister() {}
        virtual void OnComponentDestroyed() {}

        /** @brief Fixed scene phase used when this component is scene-owned. */
        [[nodiscard]] virtual SceneUpdatePhase GetSceneUpdatePhase() const
        {
            return SceneUpdatePhase::Gameplay;
        }
        /** @brief Stable order within the selected component phase. */
        [[nodiscard]] virtual int32 GetSceneUpdateOrder() const { return 0; }
        /** @brief False when a dedicated backend bridge owns all ticking. */
        [[nodiscard]] virtual bool ShouldSceneDispatchTick() const { return true; }

    protected:
        /** @brief Advance the authoritative Scene revision for value changes. */
        void NotifySceneStateChanged();
        void SetOwnerActor(Actor* owner) { m_owner = owner; }
        void SetInitialized(bool initialized) { m_initialized = initialized; }
        void SetRegistered(bool registered) { m_registered = registered; }
        void SetHasBegunPlay(bool hasBegunPlay) { m_hasBegunPlay = hasBegunPlay; }

    private:
        friend class Actor;
        friend class Scene;

        void AssignComponentHandle(ComponentHandle handle) { m_componentHandle = handle; }

        ComponentId m_componentId = InvalidComponentId;
        ComponentHandle m_componentHandle = InvalidComponentHandle;
        std::string m_name;
        Actor* m_owner = nullptr;
        bool m_enabled = true;
        bool m_initialized = false;
        bool m_registered = false;
        bool m_hasBegunPlay = false;
        bool m_canEverTick = false;
        bool m_tickEnabled = false;
    };

} // namespace RVX
