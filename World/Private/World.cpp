/**
 * @file World.cpp
 * @brief World implementation
 */

#include "World/World.h"

#include "Core/Log.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "Runtime/Camera/Camera.h"
#include "Scene/ActorFactory.h"
#include "Scene/SceneManager.h"
#include "World/SpatialSubsystem.h"

#include <algorithm>

namespace RVX
{

World::World() = default;

World::~World()
{
    if (m_initialized)
    {
        Shutdown();
    }
}

void World::Initialize(const WorldConfig& config)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("World already initialized");
        return;
    }

    m_config = config;

    RVX_CORE_INFO("Initializing World: {}", m_config.name);

    // Create scene manager
    m_sceneManager = std::make_unique<SceneManager>();
    m_sceneManager->Initialize();

    // Add spatial subsystem
    if (m_config.autoInitializeSpatial)
    {
        AddSubsystem<SpatialSubsystem>();
    }

    // Initialize all subsystems
    m_subsystems.InitializeAll();

    m_initialized = true;

    RVX_CORE_INFO("World initialized: {}", m_config.name);
}

void World::Load(const std::string& path)
{
    RVX_CORE_INFO("Loading world from: {}", path);

    if (!m_initialized || !m_sceneManager)
    {
        RVX_CORE_WARN("Cannot load world content before World is initialized");
        return;
    }

    if (path.empty())
    {
        RVX_CORE_WARN("Cannot load world content from an empty path");
        return;
    }

    auto& resourceManager = Resource::ResourceManager::Get();
    if (!resourceManager.IsInitialized())
    {
        RVX_CORE_WARN("Cannot load world content because ResourceManager is not initialized");
        return;
    }

    Resource::IResource* resource = resourceManager.LoadResource(path);
    auto* model = dynamic_cast<Resource::ModelResource*>(resource);
    if (!model)
    {
        RVX_CORE_WARN("World::Load only supports ModelResource paths in this phase: {}", path);
        return;
    }

    auto newSceneManager = std::make_unique<SceneManager>();
    newSceneManager->Initialize();

    Actor* rootActor = model->InstantiateActor(newSceneManager.get());
    if (!rootActor)
    {
        newSceneManager->Shutdown();
        RVX_CORE_WARN("World::Load loaded a model but failed to instantiate it: {}", path);
        return;
    }

    ClearPureActors();
    if (m_sceneManager)
    {
        m_sceneManager->Shutdown();
    }
    m_sceneManager = std::move(newSceneManager);

    RVX_CORE_INFO("Loaded world model root actor: {}", rootActor->GetName());
}

void World::Unload()
{
    RVX_CORE_INFO("Unloading world: {}", m_config.name);

    ClearPureActors();

    // Clear scene
    if (m_sceneManager)
    {
        m_sceneManager->Shutdown();
        m_sceneManager->Initialize();
    }
}

void World::Tick(float deltaTime)
{
    if (!m_initialized)
        return;

    UpdatePureActorLifecycles(deltaTime);

    // Update scene
    if (m_sceneManager)
    {
        m_sceneManager->Update(deltaTime);
    }

    // Tick subsystems
    m_subsystems.TickAll(deltaTime);
}

void World::Shutdown()
{
    if (!m_initialized)
        return;

    RVX_CORE_INFO("Shutting down World: {}", m_config.name);

    // Clear cameras
    m_activeCamera = nullptr;
    m_cameras.clear();

    ClearPureActors();

    // Shutdown subsystems
    m_subsystems.DeinitializeAll();

    // Shutdown scene manager
    if (m_sceneManager)
    {
        m_sceneManager->Shutdown();
        m_sceneManager.reset();
    }

    m_initialized = false;

    RVX_CORE_INFO("World shutdown complete: {}", m_config.name);
}

SpatialSubsystem* World::GetSpatial() const
{
    return m_subsystems.GetSubsystem<SpatialSubsystem>();
}

SceneEntity* World::SpawnActor(const ActorSpawnParams& params)
{
    if (!m_initialized || !m_sceneManager)
        return nullptr;

    return m_sceneManager->SpawnActor(params);
}

Actor* World::SpawnActorByClassName(const std::string& className,
                                    const ActorSpawnParams& params)
{
    if (!m_initialized || className.empty())
        return nullptr;

    auto actor = ActorFactory::Create(className);
    if (!actor)
        return nullptr;

    if (auto* sceneEntity = dynamic_cast<SceneEntity*>(actor.get()))
    {
        if (!m_sceneManager)
            return nullptr;

        if (params.parent && m_sceneManager->GetEntity(params.parent->GetHandle()) != params.parent)
            return nullptr;

        actor.release();
        SceneEntity::Ptr entity(sceneEntity);
        sceneEntity->SetName(params.name);

        SceneEntity* spawned = m_sceneManager->AddEntity(std::move(entity));
        if (!spawned)
            return nullptr;

        if (params.parent)
        {
            params.parent->AddChild(spawned);
        }

        spawned->SetPosition(params.localPosition);
        spawned->SetRotation(params.localRotation);
        spawned->SetScale(params.localScale);
        return static_cast<Actor*>(spawned);
    }

    if (params.parent)
        return nullptr;

    actor->SetName(params.name);
    actor->SetAutoRegisterComponents(true);
    actor->RegisterAllComponents();
    actor->SetPosition(params.localPosition);
    actor->SetRotation(params.localRotation);
    actor->SetScale(params.localScale);

    Actor* spawned = actor.get();
    m_actors[spawned->GetHandle()] = std::move(actor);
    return spawned;
}

bool World::DestroyActor(Actor* actor)
{
    if (!m_initialized || !actor)
        return false;

    if (auto* entity = dynamic_cast<SceneEntity*>(actor))
    {
        return m_sceneManager ? m_sceneManager->DestroyActor(entity) : false;
    }

    const auto handle = actor->GetHandle();
    auto it = m_actors.find(handle);
    if (it == m_actors.end() || it->second.get() != actor || IsActorDestroyPending(handle))
        return false;

    if (m_isDispatchingActorLifecycles)
    {
        QueuePendingActorDestroy(handle);
        return true;
    }

    DestroyPureActorImmediate(handle);
    return true;
}

Actor* World::GetActor(Actor::Handle handle) const
{
    auto it = m_actors.find(handle);
    if (it != m_actors.end())
    {
        return it->second.get();
    }

    return m_sceneManager ? static_cast<Actor*>(m_sceneManager->GetEntity(handle)) : nullptr;
}

void World::ForEachActor(const std::function<void(Actor*)>& callback)
{
    if (!callback)
        return;

    std::vector<Actor::Handle> pureActorHandles;
    pureActorHandles.reserve(m_actors.size());
    for (const auto& [handle, actor] : m_actors)
    {
        if (actor)
        {
            pureActorHandles.push_back(handle);
        }
    }

    for (Actor::Handle handle : pureActorHandles)
    {
        Actor* actor = GetActor(handle);
        if (actor && m_actors.find(handle) != m_actors.end() && !IsActorDestroyPending(handle))
        {
            callback(actor);
        }
    }

    if (!m_sceneManager)
        return;

    std::vector<SceneEntity::Handle> sceneActorHandles;
    sceneActorHandles.reserve(m_sceneManager->GetEntityCount());
    for (const auto& [handle, entity] : m_sceneManager->GetEntities())
    {
        if (entity)
        {
            sceneActorHandles.push_back(handle);
        }
    }

    for (SceneEntity::Handle handle : sceneActorHandles)
    {
        SceneEntity* entity = m_sceneManager->GetEntity(handle);
        if (entity && !m_sceneManager->IsDestroyPending(handle))
        {
            callback(static_cast<Actor*>(entity));
        }
    }
}

bool World::IsActorDestroyPending(Actor::Handle handle) const
{
    return std::find(m_pendingDestroyActors.begin(), m_pendingDestroyActors.end(), handle) !=
           m_pendingDestroyActors.end();
}

void World::QueuePendingActorDestroy(Actor::Handle handle)
{
    auto it = m_actors.find(handle);
    if (it == m_actors.end() || IsActorDestroyPending(handle))
        return;

    m_pendingDestroyActors.push_back(handle);
}

void World::FlushPendingActorDestroys()
{
    if (m_pendingDestroyActors.empty())
        return;

    auto pending = std::move(m_pendingDestroyActors);
    m_pendingDestroyActors.clear();

    for (Actor::Handle handle : pending)
    {
        DestroyPureActorImmediate(handle);
    }
}

void World::DestroyPureActorImmediate(Actor::Handle handle)
{
    auto it = m_actors.find(handle);
    if (it == m_actors.end())
        return;

    auto actor = std::move(it->second);
    m_actors.erase(it);

    if (actor)
    {
        actor->EndPlay();
        actor->UnregisterAllComponents();
    }
}

void World::UpdatePureActorLifecycles(float deltaTime)
{
    std::vector<Actor::Handle> actorHandles;
    actorHandles.reserve(m_actors.size());
    for (const auto& [handle, actor] : m_actors)
    {
        if (actor)
        {
            actorHandles.push_back(handle);
        }
    }

    m_isDispatchingActorLifecycles = true;
    for (Actor::Handle handle : actorHandles)
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

        actor = it->second.get();
        actor->Tick(deltaTime);
    }
    m_isDispatchingActorLifecycles = false;

    FlushPendingActorDestroys();
}

void World::ClearPureActors()
{
    m_isDispatchingActorLifecycles = false;
    m_pendingDestroyActors.clear();

    auto actors = std::move(m_actors);
    m_actors.clear();

    for (auto& [handle, actor] : actors)
    {
        (void)handle;
        if (actor)
        {
            actor->EndPlay();
            actor->UnregisterAllComponents();
        }
    }
}

bool World::Pick(const Ray& ray, RaycastHit& outResult)
{
    auto* spatial = GetSpatial();
    if (spatial)
    {
        return spatial->Raycast(ray, outResult);
    }
    return false;
}

bool World::PickScreen(const Camera& camera,
                       float screenX, float screenY,
                       float screenWidth, float screenHeight,
                       RaycastHit& outResult)
{
    auto* spatial = GetSpatial();
    if (spatial)
    {
        return spatial->PickScreen(camera, screenX, screenY, screenWidth, screenHeight, outResult);
    }
    return false;
}

// =============================================================================
// Camera Management
// =============================================================================

Camera* World::CreateCamera(const std::string& name)
{
    if (m_cameras.find(name) != m_cameras.end())
    {
        RVX_CORE_WARN("Camera '{}' already exists in world '{}'", name, m_config.name);
        return m_cameras[name].get();
    }

    auto camera = std::make_unique<Camera>();
    Camera* ptr = camera.get();
    m_cameras[name] = std::move(camera);

    RVX_CORE_DEBUG("Created camera '{}' in world '{}'", name, m_config.name);

    // If no active camera, set this one
    if (m_activeCamera == nullptr)
    {
        m_activeCamera = ptr;
    }

    return ptr;
}

Camera* World::GetCamera(const std::string& name) const
{
    auto it = m_cameras.find(name);
    if (it != m_cameras.end())
    {
        return it->second.get();
    }
    return nullptr;
}

void World::DestroyCamera(const std::string& name)
{
    auto it = m_cameras.find(name);
    if (it == m_cameras.end())
    {
        RVX_CORE_WARN("Camera '{}' not found in world '{}'", name, m_config.name);
        return;
    }

    // Clear active camera if it's being destroyed
    if (m_activeCamera == it->second.get())
    {
        m_activeCamera = nullptr;
    }

    m_cameras.erase(it);
    RVX_CORE_DEBUG("Destroyed camera '{}' in world '{}'", name, m_config.name);
}

void World::SetActiveCamera(Camera* camera)
{
    m_activeCamera = camera;
}

} // namespace RVX
