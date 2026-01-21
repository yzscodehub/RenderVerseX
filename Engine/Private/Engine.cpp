/**
 * @file Engine.cpp
 * @brief Engine implementation
 */

#include "Engine/Engine.h"
#include "World/World.h"
#include "Render/RenderSubsystem.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Core/Log.h"
#include "Core/Job/JobSystem.h"
#include "Runtime/Time/Time.h"

namespace RVX
{

// Static instance
Engine* Engine::s_instance = nullptr;

Engine::Engine()
{
    if (s_instance == nullptr)
    {
        s_instance = this;
    }
}

Engine::~Engine()
{
    if (s_instance == this)
    {
        s_instance = nullptr;
    }
}

Engine* Engine::Get()
{
    return s_instance;
}

void Engine::Initialize()
{
    if (m_initialized)
    {
        RVX_CORE_WARN("Engine already initialized");
        return;
    }

    RVX_CORE_INFO("=== RenderVerseX Engine Initializing ===");

    // Initialize time system
    Time::Initialize();

    // Initialize job system if enabled
    if (m_config.enableJobSystem)
    {
        JobSystem::Get().Initialize(m_config.jobWorkerCount);
    }

    // Initialize subsystems (in dependency order)
    InitializeSubsystems();

    m_initialized = true;
    m_shouldShutdown = false;
    m_frameNumber = 0;

    RVX_CORE_INFO("=== RenderVerseX Engine Initialized ===");
}

void Engine::Tick()
{
    // Update time
    Time::Update();
    float deltaTime = Time::DeltaTime();

    Tick(deltaTime);
}

void Engine::Tick(float deltaTime)
{
    // 1. Process deferred events
    EventBus::Get().ProcessDeferredEvents();

    // 2. Tick subsystems (window, input, etc.)
    TickSubsystems(deltaTime);

    // 3. Tick all worlds
    TickWorlds(deltaTime);

    // 4. Process GPU resource uploads (with time budget)
    if (auto* render = GetSubsystem<RenderSubsystem>())
    {
        render->ProcessGPUUploads(2.0f);  // 2ms budget per frame
    }

    // 5. Render active world (if auto-render is enabled)
    if (auto* render = GetSubsystem<RenderSubsystem>())
    {
        if (render->GetConfig().autoRender && m_activeWorld)
        {
            render->RenderFrame(m_activeWorld);
        }
    }

    // 6. Check for window close
    if (auto* window = GetSubsystem<WindowSubsystem>())
    {
        if (window->ShouldClose())
        {
            RequestShutdown();
        }
    }

    m_frameNumber++;
}

void Engine::TickWithoutRender()
{
    Time::Update();
    float deltaTime = Time::DeltaTime();
    TickWithoutRender(deltaTime);
}

void Engine::TickWithoutRender(float deltaTime)
{
    // 1. Process deferred events
    EventBus::Get().ProcessDeferredEvents();

    // 2. Tick subsystems (window, input, etc.)
    TickSubsystems(deltaTime);

    // 3. Tick all worlds
    TickWorlds(deltaTime);

    // 4. Skip rendering

    // 5. Check for window close
    if (auto* window = GetSubsystem<WindowSubsystem>())
    {
        if (window->ShouldClose())
        {
            RequestShutdown();
        }
    }

    m_frameNumber++;
}

void Engine::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    RVX_CORE_INFO("=== RenderVerseX Engine Shutting Down ===");

    // Shutdown worlds first
    ShutdownWorlds();

    // Shutdown subsystems
    ShutdownSubsystems();

    // Shutdown job system
    if (m_config.enableJobSystem)
    {
        JobSystem::Get().Shutdown();
    }

    m_initialized = false;

    RVX_CORE_INFO("=== RenderVerseX Engine Shutdown Complete ===");
}

// =============================================================================
// World Management
// =============================================================================

World* Engine::CreateWorld(const std::string& name)
{
    if (m_worlds.find(name) != m_worlds.end())
    {
        RVX_CORE_WARN("World '{}' already exists", name);
        return m_worlds[name].get();
    }

    auto world = std::make_unique<World>();
    WorldConfig config;
    config.name = name;
    world->Initialize(config);

    World* ptr = world.get();
    m_worlds[name] = std::move(world);

    RVX_CORE_INFO("Created world: {}", name);

    // If no active world, set this one
    if (m_activeWorld == nullptr)
    {
        m_activeWorld = ptr;
    }

    return ptr;
}

World* Engine::GetWorld(const std::string& name) const
{
    auto it = m_worlds.find(name);
    if (it != m_worlds.end())
    {
        return it->second.get();
    }
    return nullptr;
}

void Engine::DestroyWorld(const std::string& name)
{
    auto it = m_worlds.find(name);
    if (it == m_worlds.end())
    {
        RVX_CORE_WARN("World '{}' not found", name);
        return;
    }

    // Clear active world if it's being destroyed
    if (m_activeWorld == it->second.get())
    {
        m_activeWorld = nullptr;
    }

    it->second->Shutdown();
    m_worlds.erase(it);

    RVX_CORE_INFO("Destroyed world: {}", name);
}

void Engine::SetActiveWorld(World* world)
{
    m_activeWorld = world;
}

void Engine::TickWorlds(float deltaTime)
{
    for (auto& [name, world] : m_worlds)
    {
        world->Tick(deltaTime);
    }
}

void Engine::ShutdownWorlds()
{
    m_activeWorld = nullptr;
    for (auto& [name, world] : m_worlds)
    {
        world->Shutdown();
    }
    m_worlds.clear();
}

void Engine::InitializeSubsystems()
{
    // Set engine reference for all subsystems
    for (const auto& subsystem : m_subsystems.GetAll())
    {
        subsystem->SetEngine(this);
    }

    // Initialize in dependency order
    m_subsystems.InitializeAll();
}

void Engine::TickSubsystems(float deltaTime)
{
    m_subsystems.TickAll(deltaTime);
}

void Engine::ShutdownSubsystems()
{
    m_subsystems.DeinitializeAll();
}

} // namespace RVX
