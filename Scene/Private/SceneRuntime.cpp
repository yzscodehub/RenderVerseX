#include "Scene/SceneRuntime.h"

#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/SceneComponent.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace RVX
{

Scene::Scene()
    : m_sceneManager(this, &m_actorHandles)
{
}

Scene::~Scene()
{
    Shutdown();
}

bool Scene::Initialize(const SceneConfig& config)
{
    if (m_initialized)
        return true;

    m_updateThreadId = std::this_thread::get_id();
    m_sceneManager.Initialize(config);
    m_initialized = m_sceneManager.IsInitialized();
    if (m_initialized && m_sceneRevision == 0)
        m_sceneRevision = 1;
    return m_initialized;
}

void Scene::Shutdown()
{
    if (!m_initialized)
        return;

    m_activeCamera = nullptr;
    m_activeCameraComponent = InvalidComponentHandle;
    m_cameras.clear();
    ClearPureActors();
    m_sceneManager.Shutdown();
    {
        std::scoped_lock lock(m_mutationMutex);
        m_pendingMutations.clear();
    }
    m_components.clear();
    m_componentsByType.clear();
    m_pendingComponentRegistrations.clear();
    m_componentChanges.clear();
    m_sceneRevision = 0;
    m_initialized = false;
}

SceneEntity* Scene::SpawnActor(const ActorSpawnParams& params)
{
    return SpawnActor<SceneEntity>(params);
}

Actor* Scene::SpawnActorByClassName(const std::string& className,
                                    const ActorSpawnParams& params)
{
    if (!m_initialized || !IsUpdateThread() || className.empty())
        return nullptr;

    auto actor = ActorFactory::Create(className);
    if (!actor)
        return nullptr;

    if (m_isUpdating)
    {
        if (params.parent && dynamic_cast<SceneEntity*>(actor.get()) == nullptr)
            return nullptr;

        Actor* pending = actor.get();
        pending->SetName(params.name);
        pending->SetPosition(params.localPosition);
        pending->SetRotation(params.localRotation);
        pending->SetScale(params.localScale);
        return QueueActorSpawn(std::move(actor), params.parent);
    }

    if (auto* sceneEntity = dynamic_cast<SceneEntity*>(actor.get()))
    {
        if (params.parent &&
            m_sceneManager.GetEntity(params.parent->GetHandle()) != params.parent)
        {
            return nullptr;
        }

        actor.release();
        SceneEntity::Ptr entity(sceneEntity);
        sceneEntity->SetName(params.name);

        SceneEntity* spawned = m_sceneManager.AddEntity(std::move(entity));
        if (!spawned)
            return nullptr;

        if (params.parent)
        {
            params.parent->AddChild(spawned);
        }

        spawned->SetPosition(params.localPosition);
        spawned->SetRotation(params.localRotation);
        spawned->SetScale(params.localScale);
        return spawned;
    }

    if (params.parent)
        return nullptr;

    actor->SetName(params.name);
    actor->AssignHandle(m_actorHandles.Allocate());
    actor->AssignScene(this);
    AttachActor(actor.get());
    actor->SetAutoRegisterComponents(true);
    actor->RegisterAllComponents();
    actor->SetPosition(params.localPosition);
    actor->SetRotation(params.localRotation);
    actor->SetScale(params.localScale);

    Actor* spawned = actor.get();
    m_actors.emplace(spawned->GetHandle(), std::move(actor));
    return spawned;
}

bool Scene::DestroyActor(Actor* actor)
{
    if (!m_initialized || !IsUpdateThread() || !actor)
        return false;

    if (!actor->GetHandle().IsValid())
        return CancelPendingActorSpawn(actor);

    if (auto* entity = dynamic_cast<SceneEntity*>(actor))
    {
        return m_sceneManager.DestroyActor(entity);
    }

    const Actor::Handle handle = actor->GetHandle();
    auto it = m_actors.find(handle);
    if (it == m_actors.end() || it->second.get() != actor || IsActorDestroyPending(handle))
        return false;

    if (m_isDispatchingActorLifecycles || m_isUpdating)
    {
        QueuePendingActorDestroy(handle);
        return true;
    }

    DestroyPureActorImmediate(handle);
    return true;
}

Actor* Scene::ResolveActor(Actor::Handle handle) const
{
    if (!m_actorHandles.IsValid(handle))
        return nullptr;

    auto it = m_actors.find(handle);
    if (it != m_actors.end() && !IsActorDestroyPending(handle))
    {
        return it->second.get();
    }

    return const_cast<SceneEntity*>(m_sceneManager.GetEntity(handle));
}

void Scene::ForEachActor(const std::function<void(Actor*)>& callback)
{
    if (!callback)
        return;

    std::vector<Actor::Handle> handles;
    handles.reserve(GetActorCount());
    for (const auto& [handle, actor] : m_actors)
    {
        if (actor)
            handles.push_back(handle);
    }
    for (const auto& [handle, entity] : m_sceneManager.GetEntities())
    {
        if (entity)
            handles.push_back(handle);
    }

    for (Actor::Handle handle : handles)
    {
        Actor* actor = ResolveActor(handle);
        if (actor && !m_sceneManager.IsDestroyPending(handle))
        {
            callback(actor);
        }
    }
}

void Scene::Tick(float deltaTime)
{
    if (!m_initialized || !IsUpdateThread())
        return;

    ApplyMutationCommands();
    m_transformStore.BeginFrame();
    m_isUpdating = true;
    m_systemScheduler.Execute(SceneUpdatePhase::BeginFrame, deltaTime);

    UpdatePureActorLifecycles(deltaTime);
    m_sceneManager.UpdateEntityLifecycles(deltaTime, false);
    m_systemScheduler.Execute(SceneUpdatePhase::Gameplay, deltaTime);
    TickComponentsForPhase(SceneUpdatePhase::AnimationPrePhysics, deltaTime);
    m_systemScheduler.Execute(SceneUpdatePhase::AnimationPrePhysics, deltaTime);
    TickComponentsForPhase(SceneUpdatePhase::FixedPhysics, deltaTime);
    m_systemScheduler.Execute(SceneUpdatePhase::FixedPhysics, deltaTime);

    m_transformStore.Resolve();
    m_systemScheduler.Execute(SceneUpdatePhase::TransformResolve, deltaTime);
    TickComponentsForPhase(SceneUpdatePhase::BoundsSpatial, deltaTime);
    if (m_sceneManager.m_config.autoRebuildIndex)
        m_sceneManager.SynchronizeSpatialIndex();
    m_systemScheduler.Execute(SceneUpdatePhase::BoundsSpatial, deltaTime);
    TickComponentsForPhase(SceneUpdatePhase::FeatureSystems, deltaTime);
    m_systemScheduler.Execute(SceneUpdatePhase::FeatureSystems, deltaTime);
    m_systemScheduler.Execute(SceneUpdatePhase::RenderExtraction, deltaTime);
    m_systemScheduler.Execute(SceneUpdatePhase::EndFrame, deltaTime);

    FlushPendingActorDestroys();
    m_sceneManager.FlushPendingDestroyEntities();
    m_isUpdating = false;
}

Actor* Scene::QueueActorSpawn(std::unique_ptr<Actor> actor,
                              SceneEntity* parent)
{
    if (!actor || actor->GetHandle().IsValid() || actor->GetScene())
        return nullptr;

    Actor* pending = actor.get();
    m_pendingActorSpawns.push_back({std::move(actor), parent});
    return pending;
}

bool Scene::CancelPendingActorSpawn(Actor* actor)
{
    const auto found = std::find_if(
        m_pendingActorSpawns.begin(), m_pendingActorSpawns.end(),
        [actor](const PendingActorSpawn& pending)
        {
            return pending.actor.get() == actor;
        });
    if (found == m_pendingActorSpawns.end())
        return false;

    std::unordered_set<Actor*> cancelled{actor};
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (const PendingActorSpawn& pending : m_pendingActorSpawns)
        {
            if (pending.actor && pending.parent &&
                cancelled.contains(pending.parent) &&
                cancelled.insert(pending.actor.get()).second)
            {
                changed = true;
            }
        }
    }

    std::erase_if(m_pendingActorSpawns,
                  [&cancelled](const PendingActorSpawn& pending)
                  {
                      return pending.actor &&
                             cancelled.contains(pending.actor.get());
                  });
    return true;
}

void Scene::FlushPendingActorSpawns()
{
    auto pendingSpawns = std::move(m_pendingActorSpawns);
    m_pendingActorSpawns.clear();

    for (PendingActorSpawn& pending : pendingSpawns)
    {
        if (!pending.actor)
            continue;

        if (pending.parent &&
            (pending.parent->GetScene() != this ||
             ResolveActor(pending.parent->GetHandle()) != pending.parent))
        {
            continue;
        }

        if (auto* sceneEntity =
                dynamic_cast<SceneEntity*>(pending.actor.get()))
        {
            pending.actor.release();
            SceneEntity::Ptr entity(sceneEntity);
            SceneEntity* spawned = m_sceneManager.AddEntity(std::move(entity));
            if (spawned && pending.parent)
                pending.parent->AddChild(spawned);
            continue;
        }

        if (pending.parent)
            continue;

        Actor* actor = pending.actor.get();
        actor->AssignHandle(m_actorHandles.Allocate());
        actor->AssignScene(this);
        AttachActor(actor);
        actor->SetAutoRegisterComponents(true);
        actor->RegisterAllComponents();
        const Actor::Handle handle = actor->GetHandle();
        m_actors.emplace(handle, std::move(pending.actor));
    }
}

void Scene::FlushPendingComponentRegistrations()
{
    auto pending = std::move(m_pendingComponentRegistrations);
    m_pendingComponentRegistrations.clear();

    for (ActorComponent* component : pending)
    {
        if (!component || component->GetComponentHandle().IsValid())
            continue;

        Actor* owner = component->GetOwner();
        if (!owner || owner->GetScene() != this ||
            ResolveActor(owner->GetHandle()) != owner)
        {
            continue;
        }

        const auto& ownedComponents = owner->GetActorComponents();
        const bool actorOwned = std::any_of(
            ownedComponents.begin(), ownedComponents.end(),
            [component](const auto& owned)
            {
                return owned.get() == component;
            });
        const auto* sceneEntity = dynamic_cast<const SceneEntity*>(owner);
        const bool legacyOwned = sceneEntity &&
            std::any_of(sceneEntity->GetComponents().begin(),
                        sceneEntity->GetComponents().end(),
                        [component](const auto& entry)
                        {
                            return entry.second.get() == component;
                        });
        if (!actorOwned && !legacyOwned)
            continue;

        RegisterComponent(component);
        if (auto* legacy = dynamic_cast<Component*>(component))
        {
            legacy->RegisterWithOwner();
        }
        else if (!component->IsRegistered())
        {
            component->SetRegistered(true);
            component->OnRegister();
            if (!component->IsInitialized())
            {
                component->InitializeComponent();
                component->SetInitialized(true);
            }
        }
    }
}

void Scene::TickComponentsForPhase(SceneUpdatePhase phase, float deltaTime)
{
    std::vector<ActorComponent*> components;
    components.reserve(m_components.size());
    for (const auto& [handle, component] : m_components)
    {
        (void)handle;
        if (component && component->GetSceneUpdatePhase() == phase &&
            component->ShouldSceneDispatchTick())
        {
            components.push_back(component);
        }
    }
    std::sort(components.begin(), components.end(),
              [](const ActorComponent* left, const ActorComponent* right)
              {
                  if (left->GetSceneUpdateOrder() != right->GetSceneUpdateOrder())
                  {
                      return left->GetSceneUpdateOrder() <
                             right->GetSceneUpdateOrder();
                  }
                  return left->GetComponentHandle() < right->GetComponentHandle();
              });

    for (ActorComponent* component : components)
    {
        Actor* owner = component->GetOwner();
        if (owner && ResolveActor(owner->GetHandle()) == owner &&
            owner->IsActive() && component->IsEnabled())
        {
            component->TickComponent(deltaTime);
        }
    }
}

SceneSystemHandle Scene::RegisterSystem(std::string name,
                                        SceneUpdatePhase phase,
                                        SceneSystemCallback callback,
                                        int32 order)
{
    if (!IsUpdateThread())
        return InvalidSceneSystemHandle;
    return m_systemScheduler.Register(std::move(name), phase, std::move(callback), order);
}

bool Scene::UnregisterSystem(SceneSystemHandle handle)
{
    return IsUpdateThread() && m_systemScheduler.Unregister(handle);
}

void Scene::EnqueueMutation(std::function<void(Scene&)> mutation)
{
    if (!mutation)
        return;
    std::scoped_lock lock(m_mutationMutex);
    m_pendingMutations.push_back(std::move(mutation));
}

void Scene::ApplyMutationCommands()
{
    FlushPendingActorSpawns();
    FlushPendingComponentRegistrations();

    std::vector<std::function<void(Scene&)>> mutations;
    {
        std::scoped_lock lock(m_mutationMutex);
        mutations.swap(m_pendingMutations);
    }
    for (auto& mutation : mutations)
    {
        if (mutation)
            mutation(*this);
    }
}

ActorComponent* Scene::ResolveComponent(ComponentHandle handle) const
{
    if (!m_componentHandles.IsValid(handle))
        return nullptr;

    auto it = m_components.find(handle);
    return it != m_components.end() ? it->second : nullptr;
}

void Scene::NotifyTransformChanged(ComponentHandle handle)
{
    if (m_transformStore.Contains(handle))
        NotifyComponentChanged(handle);
}

void Scene::NotifyComponentChanged(ComponentHandle handle)
{
    ActorComponent* component = ResolveComponent(handle);
    if (!component)
        return;

    ++m_sceneRevision;
    m_componentChanges.push_back({SceneComponentChangeKind::Updated,
                                  handle,
                                  component->GetOwner()
                                      ? component->GetOwner()->GetHandle()
                                      : Actor::InvalidHandle,
                                  std::type_index(typeid(*component)),
                                  m_sceneRevision});
}

void Scene::RegisterComponent(ActorComponent* component)
{
    if (!component || !component->GetOwner())
        return;

    if (m_isUpdating)
    {
        if (std::find(m_pendingComponentRegistrations.begin(),
                      m_pendingComponentRegistrations.end(), component) ==
            m_pendingComponentRegistrations.end())
        {
            m_pendingComponentRegistrations.push_back(component);
        }
        return;
    }

    const ComponentHandle existingHandle = component->GetComponentHandle();
    if (existingHandle.IsValid())
    {
        auto it = m_components.find(existingHandle);
        if (it != m_components.end() && it->second == component)
            return;
        return;
    }

    const ComponentHandle handle = m_componentHandles.Allocate();
    component->AssignComponentHandle(handle);
    m_components.emplace(handle, component);
    m_componentsByType[std::type_index(typeid(*component))].push_back(handle);
    if (auto* sceneComponent = dynamic_cast<SceneComponent*>(component))
    {
        m_transformStore.Register(handle,
                                  sceneComponent->GetRelativeLocation(),
                                  sceneComponent->GetRelativeRotation(),
                                  sceneComponent->GetRelativeScale());
    }

    ++m_sceneRevision;
    m_componentChanges.push_back({SceneComponentChangeKind::Registered,
                                  handle,
                                  component->GetOwner()->GetHandle(),
                                  std::type_index(typeid(*component)),
                                  m_sceneRevision});
}

void Scene::UnregisterComponent(ActorComponent* component)
{
    if (!component)
        return;

    std::erase(m_pendingComponentRegistrations, component);

    const ComponentHandle handle = component->GetComponentHandle();
    auto componentIt = m_components.find(handle);
    if (!handle.IsValid() || componentIt == m_components.end() ||
        componentIt->second != component)
    {
        return;
    }

    const std::type_index componentType(typeid(*component));
    const Actor::Handle actorHandle = component->GetOwner()
                                          ? component->GetOwner()->GetHandle()
                                          : Actor::InvalidHandle;
    auto typeIt = m_componentsByType.find(componentType);
    if (typeIt != m_componentsByType.end())
    {
        auto& handles = typeIt->second;
        handles.erase(std::remove(handles.begin(), handles.end(), handle), handles.end());
        if (handles.empty())
            m_componentsByType.erase(typeIt);
    }

    m_components.erase(componentIt);
    if (handle == m_activeCameraComponent)
        m_activeCameraComponent = InvalidComponentHandle;
    if (dynamic_cast<SceneComponent*>(component))
        m_transformStore.Unregister(handle);
    m_componentHandles.Free(handle);
    component->AssignComponentHandle(InvalidComponentHandle);

    ++m_sceneRevision;
    m_componentChanges.push_back({SceneComponentChangeKind::Unregistered,
                                  handle,
                                  actorHandle,
                                  componentType,
                                  m_sceneRevision});
}

void Scene::AttachActor(Actor* actor)
{
    if (!actor)
        return;

    actor->AssignScene(this);
    ++m_sceneRevision;
    for (const auto& component : actor->GetActorComponents())
    {
        RegisterComponent(component.get());
    }

    if (auto* entity = dynamic_cast<SceneEntity*>(actor))
    {
        for (const auto& [type, component] : entity->GetComponents())
        {
            (void)type;
            RegisterComponent(component.get());
        }
    }
}

void Scene::DetachActor(Actor* actor)
{
    if (!actor || actor->GetScene() != this)
        return;

    std::vector<ActorComponent*> ownedComponents;
    ownedComponents.reserve(m_components.size());
    for (const auto& [handle, component] : m_components)
    {
        (void)handle;
        if (component && component->GetOwner() == actor)
            ownedComponents.push_back(component);
    }
    for (ActorComponent* component : ownedComponents)
    {
        UnregisterComponent(component);
    }

    actor->AssignScene(nullptr);
    ++m_sceneRevision;
}

bool Scene::IsActorDestroyPending(Actor::Handle handle) const
{
    return std::find(m_pendingDestroyActors.begin(), m_pendingDestroyActors.end(), handle) !=
           m_pendingDestroyActors.end();
}

void Scene::QueuePendingActorDestroy(Actor::Handle handle)
{
    if (m_actors.find(handle) == m_actors.end() || IsActorDestroyPending(handle))
        return;

    DetachActor(m_actors[handle].get());
    m_pendingDestroyActors.push_back(handle);
}

void Scene::FlushPendingActorDestroys()
{
    auto pending = std::move(m_pendingDestroyActors);
    m_pendingDestroyActors.clear();
    for (Actor::Handle handle : pending)
    {
        DestroyPureActorImmediate(handle);
    }
}

void Scene::DestroyPureActorImmediate(Actor::Handle handle)
{
    auto it = m_actors.find(handle);
    if (it == m_actors.end())
        return;

    auto actor = std::move(it->second);
    m_actors.erase(it);
    if (actor)
    {
        DetachActor(actor.get());
        actor->EndPlay();
        actor->UnregisterAllComponents();
        m_actorHandles.Free(handle);
        actor->AssignHandle(Actor::InvalidHandle);
    }
}

void Scene::UpdatePureActorLifecycles(float deltaTime)
{
    std::vector<Actor::Handle> handles;
    handles.reserve(m_actors.size());
    for (const auto& [handle, actor] : m_actors)
    {
        if (actor)
            handles.push_back(handle);
    }

    m_isDispatchingActorLifecycles = true;
    for (Actor::Handle handle : handles)
    {
        auto it = m_actors.find(handle);
        if (it == m_actors.end() || !it->second || !it->second->IsActive() ||
            IsActorDestroyPending(handle))
        {
            continue;
        }

        Actor* actor = it->second.get();
        actor->BeginPlay();

        it = m_actors.find(handle);
        if (it == m_actors.end() || !it->second || !it->second->IsActive() ||
            IsActorDestroyPending(handle))
        {
            continue;
        }

        it->second->Tick(deltaTime);
    }
    m_isDispatchingActorLifecycles = false;
}

void Scene::ClearPureActors()
{
    m_isDispatchingActorLifecycles = false;
    m_pendingActorSpawns.clear();
    m_pendingComponentRegistrations.clear();
    m_pendingDestroyActors.clear();

    auto actors = std::move(m_actors);
    m_actors.clear();
    for (auto& [handle, actor] : actors)
    {
        if (actor)
        {
            DetachActor(actor.get());
            actor->EndPlay();
            actor->UnregisterAllComponents();
            m_actorHandles.Free(handle);
            actor->AssignHandle(Actor::InvalidHandle);
        }
    }
}

Camera* Scene::CreateCamera(const std::string& name)
{
    auto it = m_cameras.find(name);
    if (it != m_cameras.end())
        return it->second.get();

    auto camera = std::make_unique<Camera>();
    Camera* result = camera.get();
    m_cameras.emplace(name, std::move(camera));
    if (!m_activeCamera)
        m_activeCamera = result;
    return result;
}

Camera* Scene::GetCamera(const std::string& name) const
{
    auto it = m_cameras.find(name);
    return it != m_cameras.end() ? it->second.get() : nullptr;
}

void Scene::DestroyCamera(const std::string& name)
{
    auto it = m_cameras.find(name);
    if (it == m_cameras.end())
        return;

    if (m_activeCamera == it->second.get())
        m_activeCamera = nullptr;
    m_cameras.erase(it);
}

void Scene::SetActiveCamera(Camera* camera)
{
    if (!camera)
    {
        m_activeCamera = nullptr;
        m_activeCameraComponent = InvalidComponentHandle;
        return;
    }

    const auto owned = std::find_if(m_cameras.begin(), m_cameras.end(),
                                    [camera](const auto& entry) {
                                        return entry.second.get() == camera;
                                    });
    if (owned != m_cameras.end())
    {
        m_activeCamera = camera;
        m_activeCameraComponent = InvalidComponentHandle;
    }
}

bool Scene::SetActiveCamera(ComponentHandle camera)
{
    if (!camera.IsValid())
    {
        m_activeCameraComponent = InvalidComponentHandle;
        return false;
    }

    if (dynamic_cast<CameraComponent*>(ResolveComponent(camera)) == nullptr)
        return false;

    m_activeCameraComponent = camera;
    m_activeCamera = nullptr;
    return true;
}

CameraComponent* Scene::GetActiveCameraComponent() const
{
    return dynamic_cast<CameraComponent*>(
        ResolveComponent(m_activeCameraComponent));
}

} // namespace RVX
