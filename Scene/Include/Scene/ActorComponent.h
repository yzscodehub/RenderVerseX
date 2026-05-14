#pragma once

/**
 * @file ActorComponent.h
 * @brief UE-style base class for actor-owned components
 */

namespace RVX
{
    class Actor;

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

        ActorComponent() = default;
        virtual ~ActorComponent() = default;

        ActorComponent(const ActorComponent&) = delete;
        ActorComponent& operator=(const ActorComponent&) = delete;

        // =====================================================================
        // Type Information
        // =====================================================================

        virtual const char* GetClassName() const { return "ActorComponent"; }

        // =====================================================================
        // Owner and State
        // =====================================================================

        Actor* GetOwner() const { return m_owner; }

        bool IsInitialized() const { return m_initialized; }
        bool IsRegistered() const { return m_registered; }
        bool HasBegunPlay() const { return m_hasBegunPlay; }
        bool IsEnabled() const { return m_enabled; }
        void SetEnabled(bool enabled) { m_enabled = enabled; }

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

    protected:
        void SetOwnerActor(Actor* owner) { m_owner = owner; }
        void SetInitialized(bool initialized) { m_initialized = initialized; }
        void SetRegistered(bool registered) { m_registered = registered; }
        void SetHasBegunPlay(bool hasBegunPlay) { m_hasBegunPlay = hasBegunPlay; }

    private:
        friend class Actor;

        Actor* m_owner = nullptr;
        bool m_enabled = true;
        bool m_initialized = false;
        bool m_registered = false;
        bool m_hasBegunPlay = false;
        bool m_canEverTick = false;
        bool m_tickEnabled = false;
    };

} // namespace RVX
