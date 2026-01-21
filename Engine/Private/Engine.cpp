/**
 * @file Engine.cpp
 * @brief Engine implementation
 */

#include "Engine/Engine.h"
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

    // Initialize new-style subsystems
    InitializeSubsystems();

    // Initialize legacy systems (for backward compatibility)
    m_legacySystems.InitAll();

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
    // Process deferred events
    EventBus::Get().ProcessDeferredEvents();

    // Tick new-style subsystems
    TickSubsystems(deltaTime);

    // Tick legacy systems
    m_legacySystems.UpdateAll(deltaTime);
    m_legacySystems.RenderAll();

    m_frameNumber++;
}

void Engine::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    RVX_CORE_INFO("=== RenderVerseX Engine Shutting Down ===");

    // Shutdown legacy systems first
    m_legacySystems.ShutdownAll();
    m_legacySystems.Clear();

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

void Engine::Run(const std::function<bool()>& shouldExit, 
                 const std::function<float()>& getDeltaTime)
{
    Initialize();

    while (!shouldExit || !shouldExit())
    {
        if (getDeltaTime)
        {
            Tick(getDeltaTime());
        }
        else
        {
            Tick();  // Use internal time
        }

        if (m_shouldShutdown)
        {
            break;
        }
    }

    Shutdown();
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
