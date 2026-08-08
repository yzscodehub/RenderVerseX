#pragma once

/**
 * @file SceneRuntime.h
 * @brief Authoritative runtime owner for actors and compatibility cameras
 */

#include "Scene/ActorFactory.h"
#include "Scene/SceneManager.h"
#include "Scene/SceneSystemScheduler.h"
#include "Scene/TransformStore.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <thread>

namespace RVX
{
    class Camera;
    class CameraComponent;

    enum class SceneComponentChangeKind : uint8
    {
        Registered = 0,
        Updated,
        Unregistered
    };

    struct SceneComponentChange
    {
        SceneComponentChangeKind kind = SceneComponentChangeKind::Registered;
        ComponentHandle component = InvalidComponentHandle;
        Actor::Handle actor = Actor::InvalidHandle;
        std::type_index componentType{typeid(void)};
        uint64 sceneRevision = 0;
    };

    /**
     * @brief Authoritative scene runtime owned by a World.
     *
     * Scene owns the common actor handle pool. SceneManager remains available as
     * a compatibility facade for spatial SceneEntity APIs while pure Actor and
     * SceneEntity identities share the same generation-safe allocation domain.
     */
    class Scene
    {
    public:
        Scene();
        ~Scene();

        Scene(const Scene&) = delete;
        Scene& operator=(const Scene&) = delete;
        Scene(Scene&&) = delete;
        Scene& operator=(Scene&&) = delete;

        bool Initialize(const SceneConfig& config = {});
        void Shutdown();
        bool IsInitialized() const { return m_initialized; }
        bool IsUpdateThread() const { return std::this_thread::get_id() == m_updateThreadId; }
        bool IsUpdating() const { return m_isUpdating; }

        SceneManager* GetSceneManager() { return &m_sceneManager; }
        const SceneManager* GetSceneManager() const { return &m_sceneManager; }

        SceneEntity* SpawnActor(const ActorSpawnParams& params = {});
        Actor* SpawnActorByClassName(const std::string& className,
                                     const ActorSpawnParams& params = {});

        template<typename T = SceneEntity>
        T* SpawnActor(const ActorSpawnParams& params = {})
        {
            static_assert(std::is_base_of_v<Actor, T>, "T must derive from Actor");

            if (!m_initialized || !IsUpdateThread())
                return nullptr;

            // Mutations requested while any scene phase is iterating become
            // visible at the next BeginFrame. The returned construction
            // object intentionally has no runtime identity until adoption.
            if (m_isUpdating)
            {
                if constexpr (!std::is_base_of_v<SceneEntity, T>)
                {
                    if (params.parent)
                        return nullptr;
                }

                auto actor = std::make_unique<T>(params.name);
                T* pending = actor.get();
                pending->SetPosition(params.localPosition);
                pending->SetRotation(params.localRotation);
                pending->SetScale(params.localScale);
                return static_cast<T*>(
                    QueueActorSpawn(std::move(actor), params.parent));
            }

            if constexpr (std::is_base_of_v<SceneEntity, T>)
            {
                return m_sceneManager.SpawnActor<T>(params);
            }
            else
            {
                if (params.parent)
                    return nullptr;

                auto actor = std::make_unique<T>(params.name);
                T* spawned = actor.get();
                spawned->AssignHandle(m_actorHandles.Allocate());
                spawned->AssignScene(this);
                AttachActor(spawned);
                spawned->SetAutoRegisterComponents(true);
                spawned->RegisterAllComponents();
                spawned->SetPosition(params.localPosition);
                spawned->SetRotation(params.localRotation);
                spawned->SetScale(params.localScale);

                const Actor::Handle handle = spawned->GetHandle();
                m_actors.emplace(handle, std::move(actor));
                return spawned;
            }
        }

        bool DestroyActor(Actor* actor);
        Actor* ResolveActor(Actor::Handle handle) const;
        Actor* GetActor(Actor::Handle handle) const { return ResolveActor(handle); }

        size_t GetPureActorCount() const { return m_actors.size(); }
        size_t GetActorCount() const { return m_actors.size() + m_sceneManager.GetEntityCount(); }
        void ForEachActor(const std::function<void(Actor*)>& callback);
        void Tick(float deltaTime);

        SceneSystemHandle RegisterSystem(std::string name,
                                         SceneUpdatePhase phase,
                                         SceneSystemCallback callback,
                                         int32 order = 0);
        bool UnregisterSystem(SceneSystemHandle handle);
        void EnqueueMutation(std::function<void(Scene&)> mutation);

        uint64 GetRevision() const { return m_sceneRevision; }
        TransformStore& GetTransformStore() { return m_transformStore; }
        const TransformStore& GetTransformStore() const { return m_transformStore; }
        void NotifyTransformChanged(ComponentHandle handle);
        void NotifyComponentChanged(ComponentHandle handle);
        ActorComponent* ResolveComponent(ComponentHandle handle) const;
        const std::vector<SceneComponentChange>& GetComponentChanges() const
        {
            return m_componentChanges;
        }
        void ClearComponentChanges() { m_componentChanges.clear(); }

        template<typename T>
        std::vector<T*> GetComponents() const
        {
            static_assert(std::is_base_of_v<ActorComponent, T>,
                          "T must derive from ActorComponent");

            std::vector<T*> result;
            auto it = m_componentsByType.find(std::type_index(typeid(T)));
            if (it == m_componentsByType.end())
                return result;

            result.reserve(it->second.size());
            for (ComponentHandle handle : it->second)
            {
                if (auto* component = ResolveComponent(handle))
                    result.push_back(static_cast<T*>(component));
            }
            return result;
        }

        /** @brief Query the component registry by base class or interface. */
        template<typename T>
        std::vector<T*> GetComponentsImplementing() const
        {
            std::vector<std::pair<ComponentHandle, T*>> ordered;
            ordered.reserve(m_components.size());
            for (const auto& [handle, component] : m_components)
            {
                if (auto* typed = dynamic_cast<T*>(component))
                    ordered.emplace_back(handle, typed);
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const auto& left, const auto& right)
                      {
                          return left.first < right.first;
                      });

            std::vector<T*> result;
            result.reserve(ordered.size());
            for (const auto& [handle, component] : ordered)
            {
                (void)handle;
                result.push_back(component);
            }
            return result;
        }

        Camera* CreateCamera(const std::string& name = "Main");
        Camera* GetCamera(const std::string& name = "Main") const;
        void DestroyCamera(const std::string& name);
        void SetActiveCamera(Camera* camera);
        Camera* GetActiveCamera() const { return m_activeCamera; }

        /** @brief Select the authoritative active CameraComponent by handle. */
        bool SetActiveCamera(ComponentHandle camera);
        [[nodiscard]] ComponentHandle GetActiveCameraHandle() const
        {
            return m_activeCameraComponent;
        }
        [[nodiscard]] CameraComponent* GetActiveCameraComponent() const;

    private:
        friend class Actor;
        friend class SceneManager;

        void RegisterComponent(ActorComponent* component);
        void UnregisterComponent(ActorComponent* component);
        Actor* QueueActorSpawn(std::unique_ptr<Actor> actor,
                               SceneEntity* parent);
        bool CancelPendingActorSpawn(Actor* actor);
        void FlushPendingActorSpawns();
        void FlushPendingComponentRegistrations();
        void AttachActor(Actor* actor);
        void DetachActor(Actor* actor);
        bool IsActorDestroyPending(Actor::Handle handle) const;
        void QueuePendingActorDestroy(Actor::Handle handle);
        void FlushPendingActorDestroys();
        void DestroyPureActorImmediate(Actor::Handle handle);
        void UpdatePureActorLifecycles(float deltaTime);
        void TickComponentsForPhase(SceneUpdatePhase phase, float deltaTime);
        void ApplyMutationCommands();
        void ClearPureActors();

        HandlePool<Actor::Handle> m_actorHandles;
        SceneManager m_sceneManager;
        std::unordered_map<Actor::Handle, std::unique_ptr<Actor>> m_actors;
        struct PendingActorSpawn
        {
            std::unique_ptr<Actor> actor;
            SceneEntity* parent = nullptr;
        };
        std::vector<PendingActorSpawn> m_pendingActorSpawns;
        std::vector<Actor::Handle> m_pendingDestroyActors;
        HandlePool<ComponentHandle> m_componentHandles;
        TransformStore m_transformStore;
        std::unordered_map<ComponentHandle, ActorComponent*> m_components;
        std::unordered_map<std::type_index, std::vector<ComponentHandle>> m_componentsByType;
        std::vector<ActorComponent*> m_pendingComponentRegistrations;
        std::vector<SceneComponentChange> m_componentChanges;
        std::unordered_map<std::string, std::unique_ptr<Camera>> m_cameras;
        Camera* m_activeCamera = nullptr;
        ComponentHandle m_activeCameraComponent = InvalidComponentHandle;
        SceneSystemScheduler m_systemScheduler;
        std::mutex m_mutationMutex;
        std::vector<std::function<void(Scene&)>> m_pendingMutations;
        std::thread::id m_updateThreadId;
        uint64 m_sceneRevision = 0;
        bool m_isDispatchingActorLifecycles = false;
        bool m_isUpdating = false;
        bool m_initialized = false;
    };
} // namespace RVX
