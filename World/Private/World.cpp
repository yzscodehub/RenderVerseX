/**
 * @file World.cpp
 * @brief World implementation
 */

#include "World/World.h"

#include "Core/Camera/Camera.h"
#include "Core/Log.h"
#include "Resource/ResourceManager.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/ResourceSceneAdapters.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "Scene/SceneRuntime.h"
#include "World/PhysicsSubsystem.h"
#include "World/SpatialSubsystem.h"

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

bool World::Initialize(const WorldConfig& config)
{
    if (m_initialized)
    {
        RVX_CORE_WARN("World already initialized");
        return true;
    }

    m_config = config;

    RVX_CORE_INFO("Initializing World: {}", m_config.name);

    // Create the single authoritative scene runtime.
    m_scene = std::make_unique<Scene>();
    if (!m_scene->Initialize())
    {
        RVX_CORE_ERROR("World scene initialization failed: {}", m_config.name);
        m_scene.reset();
        return false;
    }
    ResourceSceneAdapters::RegisterDefaults();

    // Add physics subsystem before spatial queries so physics-driven transform
    // updates are visible before spatial indexing observes the scene.
    if (m_config.autoInitializePhysics)
    {
        PhysicsSubsystem* physics = AddSubsystem<PhysicsSubsystem>();
        physics->SetConfig(m_config.physics);
    }

    // Add spatial subsystem
    if (m_config.autoInitializeSpatial)
    {
        AddSubsystem<SpatialSubsystem>();
    }

    // Initialize all subsystems
    if (!m_subsystems.InitializeAll())
    {
        RVX_CORE_ERROR("World subsystem initialization failed: {}", m_config.name);
        m_subsystems.DeinitializeAll();
        if (m_scene)
        {
            m_scene->Shutdown();
            m_scene.reset();
        }
        m_initialized = false;
        return false;
    }

    m_initialized = true;

    RVX_CORE_INFO("World initialized: {}", m_config.name);
    return true;
}

void World::Load(const std::string& path)
{
    RVX_CORE_INFO("Loading world from: {}", path);

    if (!m_initialized || !m_scene)
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

    auto newScene = std::make_unique<Scene>();
    if (!newScene->Initialize())
    {
        RVX_CORE_WARN("World::Load failed to initialize replacement scene: {}", path);
        return;
    }

    SceneAssetInstance instance =
        SceneAssetInstantiator::InstantiateModel(*newScene, *model);
    Actor* rootActor = newScene->ResolveActor(instance.rootActor);
    if (!instance.IsValid() || !rootActor)
    {
        newScene->Shutdown();
        RVX_CORE_WARN("World::Load loaded a model but failed to instantiate it: {}", path);
        return;
    }

    if (m_scene)
    {
        if (auto* physics = GetPhysics())
        {
            physics->Deinitialize();
        }
        m_scene->Shutdown();
    }
    m_scene = std::move(newScene);
    if (auto* physics = GetPhysics())
    {
        physics->Initialize();
    }

    RVX_CORE_INFO("Loaded world model root actor: {}", rootActor->GetName());
}

void World::Unload()
{
    RVX_CORE_INFO("Unloading world: {}", m_config.name);

    if (m_scene)
    {
        m_scene->Shutdown();
        m_scene->Initialize();
    }
}

void World::Tick(float deltaTime)
{
    if (!m_initialized)
        return;

    m_subsystems.TickPhase(TickPhase::PreUpdate, deltaTime);

    if (m_scene)
    {
        m_scene->Tick(deltaTime);
    }

    m_subsystems.TickPhase(TickPhase::Update, deltaTime);
    m_subsystems.TickPhase(TickPhase::PostUpdate, deltaTime);
    m_subsystems.TickPhase(TickPhase::PreRender, deltaTime);
    m_subsystems.TickPhase(TickPhase::PostRender, deltaTime);
}

void World::Shutdown()
{
    if (!m_initialized)
        return;

    RVX_CORE_INFO("Shutting down World: {}", m_config.name);

    // Shutdown subsystems
    m_subsystems.DeinitializeAll();

    // Shutdown the authoritative scene after subsystem bridges detach.
    if (m_scene)
    {
        m_scene->Shutdown();
        m_scene.reset();
    }

    m_initialized = false;

    RVX_CORE_INFO("World shutdown complete: {}", m_config.name);
}

SpatialSubsystem* World::GetSpatial() const
{
    return m_subsystems.GetSubsystem<SpatialSubsystem>();
}

PhysicsSubsystem* World::GetPhysics() const
{
    return m_subsystems.GetSubsystem<PhysicsSubsystem>();
}

SceneEntity* World::SpawnActor(const ActorSpawnParams& params)
{
    if (!m_initialized || !m_scene)
        return nullptr;

    return m_scene->SpawnActor(params);
}

Actor* World::SpawnActorByClassName(const std::string& className,
                                    const ActorSpawnParams& params)
{
    return m_initialized && m_scene
               ? m_scene->SpawnActorByClassName(className, params)
               : nullptr;
}

bool World::DestroyActor(Actor* actor)
{
    return m_initialized && m_scene ? m_scene->DestroyActor(actor) : false;
}

Actor* World::GetActor(Actor::Handle handle) const
{
    return m_scene ? m_scene->ResolveActor(handle) : nullptr;
}

void World::ForEachActor(const std::function<void(Actor*)>& callback)
{
    if (m_scene)
        m_scene->ForEachActor(callback);
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
    return m_scene ? m_scene->CreateCamera(name) : nullptr;
}

Camera* World::GetCamera(const std::string& name) const
{
    return m_scene ? m_scene->GetCamera(name) : nullptr;
}

void World::DestroyCamera(const std::string& name)
{
    if (m_scene)
        m_scene->DestroyCamera(name);
}

void World::SetActiveCamera(Camera* camera)
{
    if (m_scene)
        m_scene->SetActiveCamera(camera);
}

bool World::SetActiveCamera(ComponentHandle camera)
{
    return m_scene && m_scene->SetActiveCamera(camera);
}

} // namespace RVX
