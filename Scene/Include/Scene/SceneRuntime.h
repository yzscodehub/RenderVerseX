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
#include <utility>
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
        uint64 changeSequence = 0;
    };

    /**
     * @brief Authoritative scene runtime owned by a World.
     *
     * Scene owns every Actor in one generation-safe allocation domain.
     * SceneManager is a non-owning compatibility facade for legacy spatial
     * SceneEntity APIs.
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

        size_t GetPureActorCount() const;
        size_t GetActorCount() const { return m_actors.size(); }
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
        [[nodiscard]] uint64 GetLastComponentChangeSequence() const
        {
            return m_componentChangeSequence;
        }
        [[nodiscard]] uint64 GetFirstComponentChangeSequence() const
        {
            return m_componentChanges.empty()
                       ? m_componentChangeSequence + 1
                       : m_componentChanges.front().changeSequence;
        }

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
            const std::type_index queryType(typeid(T));
            auto [viewIt, inserted] =
                m_componentQueryViews.try_emplace(queryType);
            ComponentQueryView& view = viewIt->second;
            if (inserted)
            {
                view.matches = [](ActorComponent* component)
                {
                    return dynamic_cast<T*>(component) != nullptr;
                };
                view.handles.reserve(m_components.size());
                for (const auto& [handle, component] : m_components)
                {
                    if (view.matches(component))
                        view.handles.push_back(handle);
                }
                std::sort(view.handles.begin(), view.handles.end());
            }

            std::vector<T*> result;
            result.reserve(view.handles.size());
            for (ComponentHandle handle : view.handles)
            {
                if (auto* component = ResolveComponent(handle))
                {
                    if (auto* typed = dynamic_cast<T*>(component))
                        result.push_back(typed);
                }
            }
            return result;
        }

        /** @brief Query one actor's registered components without scanning the scene. */
        template<typename T>
        std::vector<T*> GetComponentsForActorImplementing(
            Actor::Handle actor) const
        {
            std::vector<T*> result;
            const auto actorIt = m_componentsByActor.find(actor);
            if (actorIt == m_componentsByActor.end())
                return result;

            result.reserve(actorIt->second.size());
            for (ComponentHandle handle : actorIt->second)
            {
                if (auto* component = ResolveComponent(handle))
                {
                    if (auto* typed = dynamic_cast<T*>(component))
                        result.push_back(typed);
                }
            }
            return result;
        }

        // Authoritative spatial facade. SceneManager remains an internal
        // compatibility implementation detail until the legacy cutover.
        Spatial::ISpatialIndex* GetSpatialIndex()
        {
            return m_sceneManager.GetSpatialIndex();
        }
        const Spatial::ISpatialIndex* GetSpatialIndex() const
        {
            return m_sceneManager.GetSpatialIndex();
        }
        void SetSpatialIndex(Spatial::SpatialIndexPtr index)
        {
            m_sceneManager.SetSpatialIndex(std::move(index));
        }
        void RebuildSpatialIndex() { m_sceneManager.RebuildSpatialIndex(); }
        void SynchronizeSpatialIndex()
        {
            m_sceneManager.SynchronizeSpatialIndex();
        }
        [[nodiscard]] SpatialQueryTarget ResolveSpatialQueryTarget(
            const Spatial::QueryResult& result) const
        {
            return m_sceneManager.ResolveSpatialQueryTarget(result);
        }
        void RegisterSpatialPrimitive(PrimitiveComponent* primitive)
        {
            m_sceneManager.RegisterPrimitive(primitive);
        }
        void UnregisterSpatialPrimitive(PrimitiveComponent* primitive)
        {
            m_sceneManager.UnregisterPrimitive(primitive);
        }
        void MarkSpatialPrimitiveDirty(PrimitiveComponent* primitive)
        {
            m_sceneManager.MarkPrimitiveSpatialDirty(primitive);
        }

#if defined(RVX_ENABLE_LEGACY_SCENE_API)
        /** @brief Legacy-sample facade backed by a scene CameraComponent. */
        Camera* CreateCamera(const std::string& name = "Main");
        Camera* GetCamera(const std::string& name = "Main") const;
        void DestroyCamera(const std::string& name);
        /** @brief Legacy-sample adapter selecting the facade's component. */
        void SetActiveCamera(Camera* camera);
        Camera* GetActiveCamera() const { return m_activeCamera; }
#endif

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
        void AppendComponentChange(SceneComponentChange change);
#if defined(RVX_ENABLE_LEGACY_SCENE_API)
        void SynchronizeLegacyCamera(Camera* camera);
#endif

        struct ComponentQueryView
        {
            std::function<bool(ActorComponent*)> matches;
            std::vector<ComponentHandle> handles;
        };

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
        std::unordered_map<Actor::Handle, std::vector<ComponentHandle>> m_componentsByActor;
        mutable std::unordered_map<std::type_index, ComponentQueryView>
            m_componentQueryViews;
        std::vector<ActorComponent*> m_pendingComponentRegistrations;
        std::vector<SceneComponentChange> m_componentChanges;
#if defined(RVX_ENABLE_LEGACY_SCENE_API)
        std::unordered_map<std::string, std::unique_ptr<Camera>> m_cameras;
        std::unordered_map<Camera*, ComponentHandle> m_legacyCameraComponents;
        Camera* m_activeCamera = nullptr;
#endif
        ComponentHandle m_activeCameraComponent = InvalidComponentHandle;
        SceneSystemScheduler m_systemScheduler;
        std::mutex m_mutationMutex;
        std::vector<std::function<void(Scene&)>> m_pendingMutations;
        std::thread::id m_updateThreadId;
        uint64 m_sceneRevision = 0;
        uint64 m_componentChangeSequence = 0;
        bool m_isDispatchingActorLifecycles = false;
        bool m_isUpdating = false;
        bool m_isShuttingDown = false;
        bool m_initialized = false;
    };
} // namespace RVX
