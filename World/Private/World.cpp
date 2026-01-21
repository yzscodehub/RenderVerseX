/**
 * @file World.cpp
 * @brief World implementation
 */

#include "World/World.h"
#include "World/SpatialSubsystem.h"
#include "Scene/SceneManager.h"
#include "Runtime/Camera/Camera.h"
#include "Core/Log.h"

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
    // TODO: Implement world loading from asset
}

void World::Unload()
{
    RVX_CORE_INFO("Unloading world: {}", m_config.name);
    
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
