/**
 * @file Engine.cpp
 * @brief Engine implementation
 */

#include "Engine/Engine.h"
#include "RenderRuntimeComposition.h"

#include "Core/Job/JobSystem.h"
#include "Core/Log.h"
#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Time/Time.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "World/World.h"

#include <algorithm>

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

bool Engine::Initialize()
{
    if (m_initialized)
    {
        RVX_CORE_WARN("Engine already initialized");
        return true;
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
    if (!InitializeSubsystems())
    {
        RVX_CORE_ERROR("=== RenderVerseX Engine Initialization Failed ===");
        if (m_config.enableJobSystem)
        {
            JobSystem::Get().Shutdown();
        }
        m_initialized = false;
        m_shouldShutdown = true;
        return false;
    }

    m_initialized = true;
    m_shouldShutdown = false;
    m_frameNumber = 0;

    RVX_CORE_INFO("=== RenderVerseX Engine Initialized ===");
    return true;
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

    // 4. Extract and publish one immutable frame after simulation.
    RenderAfterWorlds(deltaTime);

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

    if (m_renderComposition != nullptr)
    {
        m_lastRenderShutdownResult = m_renderComposition->Shutdown();
    }

    // Render has acknowledged exit while Window and Worlds remain alive.
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
    if (!world->Initialize(config))
    {
        RVX_CORE_ERROR("Failed to create world: {}", name);
        return nullptr;
    }

    World* ptr = world.get();
    m_worlds[name] = std::move(world);

    RVX_CORE_INFO("Created world: {}", name);

    // If no active world, set this one
    if (m_activeWorld == nullptr)
    {
        SetActiveWorld(ptr);
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
        SetActiveWorld(nullptr);
    }

    it->second->Shutdown();
    m_worlds.erase(it);

    RVX_CORE_INFO("Destroyed world: {}", name);
}

void Engine::SetActiveWorld(World* world)
{
    if (world != nullptr)
    {
        const bool owned = std::any_of(
            m_worlds.begin(),
            m_worlds.end(),
            [world](const auto& entry) { return entry.second.get() == world; });
        if (!owned)
        {
            RVX_CORE_WARN("Cannot activate a World not owned by this Engine");
            return;
        }
    }
    if (m_activeWorld == world)
    {
        return;
    }
    m_activeWorld = world;
    if (m_renderComposition != nullptr)
    {
        m_renderComposition->OnActiveWorldChanged();
    }
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

bool Engine::InitializeSubsystems()
{
    // Set engine reference for all subsystems
    for (const auto& subsystem : m_subsystems.GetAll())
    {
        subsystem->SetEngine(this);
    }

    auto* render = GetSubsystem<RenderSubsystem>();
    if (render != nullptr)
    {
        auto* resources = GetSubsystem<Resource::ResourceSubsystem>();
        auto* window = GetSubsystem<WindowSubsystem>();
        if (resources == nullptr || window == nullptr)
        {
            RVX_CORE_ERROR(
                "Dedicated Render composition requires ResourceSubsystem and WindowSubsystem");
            return false;
        }

        auto services = CreateEngineRenderRuntimeCompositionServices(
            *resources,
            *render,
            *window,
            [this]() {
                return m_subsystems.DeinitializeSubsystem<RenderSubsystem>();
            });
        m_renderComposition = std::make_unique<RenderRuntimeComposition>(
            m_config.renderRuntime,
            m_config.initialRenderFrameSettings,
            std::move(services));
        if (!m_renderComposition->PrepareBeforeSubsystemInitialization())
        {
            RVX_CORE_ERROR("Failed to prepare dedicated Render composition");
            return false;
        }
        if (m_activeWorld != nullptr)
        {
            m_renderComposition->OnActiveWorldChanged();
        }
    }

    // Initialize in dependency order. The staged hook captures the native
    // surface after Window initialization and before Render starts.
    return m_subsystems.InitializeAll(
        [this](EngineSubsystem& subsystem) {
            if (m_renderComposition != nullptr &&
                &subsystem == GetSubsystem<RenderSubsystem>())
            {
                m_renderComposition->BeforeRenderSubsystemInitialize();
            }
        });
}

void Engine::TickSubsystems(float deltaTime)
{
    m_subsystems.TickAll(deltaTime);
}

void Engine::ShutdownSubsystems()
{
    m_subsystems.DeinitializeAll();
}

void Engine::RenderAfterWorlds(float deltaTime)
{
    if (m_renderComposition == nullptr)
    {
        return;
    }
    m_renderComposition->TickAfterWorlds(
        m_activeWorld,
        deltaTime,
        static_cast<float32>(Time::ElapsedTime()));
}

bool Engine::SetRenderFrameSettings(
    const RenderFrameSettings& settings) noexcept
{
    if (m_renderComposition != nullptr)
    {
        return m_renderComposition->SetFrameSettings(settings);
    }
    if (!IsValidRenderFrameSettings(settings))
    {
        return false;
    }
    m_config.initialRenderFrameSettings = settings;
    return true;
}

const RenderFrameSettings& Engine::GetRenderFrameSettings() const noexcept
{
    return m_renderComposition != nullptr
               ? m_renderComposition->GetFrameSettings()
               : m_config.initialRenderFrameSettings;
}

uint64 Engine::RequestRenderTemporalReset() noexcept
{
    return m_renderComposition != nullptr
               ? m_renderComposition->RequestTemporalReset()
               : 0;
}

bool Engine::RequestRenderFrameCapture(
    const RenderFrameCaptureRequest& request) noexcept
{
    return m_renderComposition != nullptr &&
           m_renderComposition->QueueCapture(request);
}

} // namespace RVX
