/**
 * @file Engine.cpp
 * @brief Engine-owned pure ECS runtime and render composition lifecycle.
 */

#include "Engine/Engine.h"

#include "ECS/EcsRenderRuntimeComposition.h"
#include "ECS/WorldEcsRuntimeComposition.h"
#include "Core/Job/JobSystem.h"
#include "Core/Log.h"
#include "Particle/ECS/ParticleEcsRuntimeGateway.h"
#include "Render/RenderSubsystem.h"
#include "RenderContracts/RenderFrameValidation.h"
#include "Resource/ResourceSubsystem.h"
#include "Runtime/Time/Time.h"
#include "Runtime/Window/WindowSubsystem.h"
#include "Terrain/ECS/TerrainEcsRuntimeGateway.h"
#include "Water/ECS/WaterEcsRuntimeGateway.h"
#include "World/World.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
Engine* Engine::s_instance = nullptr;

namespace
{
    [[nodiscard]] bool IsTerminalRenderShutdown(RenderShutdownCode code) noexcept
    {
        return code == RenderShutdownCode::None ||
               code == RenderShutdownCode::Completed ||
               code == RenderShutdownCode::AlreadyStopped;
    }

    [[nodiscard]] uint32 ComputeRequestedFixedSteps(
        float64 accumulator,
        float64 fixedDeltaSeconds,
        uint32 maxSubSteps) noexcept
    {
        if (!std::isfinite(accumulator) || !std::isfinite(fixedDeltaSeconds) ||
            fixedDeltaSeconds <= 0.0 || maxSubSteps == 0)
        {
            return 0;
        }

        const float64 available = std::floor(accumulator / fixedDeltaSeconds);
        if (available <= 0.0)
        {
            return 0;
        }

        const float64 bounded = std::min(
            available,
            std::min(static_cast<float64>(maxSubSteps),
                     static_cast<float64>(std::numeric_limits<uint32>::max())));
        return static_cast<uint32>(bounded);
    }
} // namespace

Engine::Engine()
{
    if (s_instance == nullptr)
    {
        s_instance = this;
    }
}

Engine::~Engine()
{
    if (m_initialized || m_initializationRollbackPending)
    {
        Shutdown();
    }

    // A failed initialization never publishes a World entry. A successful
    // shutdown releases entries before this point; retaining them would mean
    // a caller abandoned completion-owned ECS proof evidence.
    RVX_ASSERT_MSG(m_worlds.empty(),
                   "Engine destruction requires all ECS Worlds to finish shutdown");
    RVX_ASSERT_MSG(!m_initializationRollbackPending,
                   "Engine destruction requires a completed ECS initialization rollback");
    m_ecsRenderComposition.reset();
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
    Diagnostics::TraceSpan startupSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "EngineInitialize",
        {{"appName", m_config.appName != nullptr ? m_config.appName : ""}});
    if (m_initialized)
    {
        RVX_CORE_WARN("Engine already initialized");
        startupSpan.SetAttribute("result", "already-initialized");
        return true;
    }
    if (m_initializationRollbackPending)
    {
        if (!RollbackFailedInitialization())
        {
            RVX_CORE_ERROR(
                "Cannot initialize while the prior ECS render rollback remains incomplete");
            startupSpan.SetAttribute("result", "rollback-pending");
            return false;
        }
        if (m_config.enableJobSystem)
        {
            JobSystem::Get().Shutdown();
        }
        m_initializationRollbackPending = false;
    }

    RVX_CORE_INFO("=== RenderVerseX Engine Initializing ===");
    Diagnostics::RecordTraceInstant(m_config.startupTraceContext, "Engine.Time.Initialize");
    Time::Initialize();

    if (m_config.enableJobSystem)
    {
        Diagnostics::RecordTraceInstant(
            m_config.startupTraceContext,
            "Engine.JobSystem.Initialize",
            {{"workerCount", std::to_string(m_config.jobWorkerCount)}});
        JobSystem::Get().Initialize(m_config.jobWorkerCount);
    }

    if (!InitializeSubsystems())
    {
        RVX_CORE_ERROR("=== RenderVerseX Engine Initialization Failed ===");
        const bool rollbackComplete = RollbackFailedInitialization();
        if (rollbackComplete && m_config.enableJobSystem)
        {
            JobSystem::Get().Shutdown();
        }

        EngineShutdownDiagnostics shutdown;
        shutdown.available = true;
        shutdown.initializationAttempted = true;
        shutdown.initializationSucceeded = false;
        shutdown.frameNumber = m_frameNumber;
        shutdown.subsystemCount = m_subsystems.GetCount();
        shutdown.subsystemsStopped = rollbackComplete && !m_subsystems.IsInitialized();
        shutdown.worldCountAfterShutdown = 0;
        shutdown.activeWorldCleared = true;
        shutdown.jobSystemStopped = rollbackComplete && !JobSystem::Get().IsInitialized();
        shutdown.clean = shutdown.subsystemsStopped && shutdown.activeWorldCleared &&
                         shutdown.jobSystemStopped && rollbackComplete;
        m_lastShutdownDiagnostics = std::move(shutdown);
        m_initializationRollbackPending = !rollbackComplete;
        m_shouldShutdown = true;
        startupSpan.SetAttribute("result", "subsystem-initialization-failed");
        return false;
    }

    // ResourceSubsystem now owns an initialized update-thread ResourceManager. Construct one
    // shared scene-qualified request owner and derive the one evaluator resolver from it before
    // any World can be published. Neither object is a World service singleton.
    Resource::ResourceSubsystem* resources = GetSubsystem<Resource::ResourceSubsystem>();
    if (resources != nullptr)
    {
        try
        {
            m_ecsAnimationAssetService =
                std::make_shared<AnimationSceneAdapters::EcsAnimationAssetService>(*resources);
            m_resourceAnimationEvaluator =
                std::make_shared<AnimationSceneAdapters::ResourceAnimationEcsEvaluator>(
                    m_ecsAnimationAssetService->CreateResolver());
        }
        catch (const std::exception& exception)
        {
            RVX_CORE_ERROR("Engine could not construct ECS animation services: {}", exception.what());
            static_cast<void>(RollbackFailedInitialization());
            m_initializationRollbackPending = true;
            m_shouldShutdown = true;
            startupSpan.SetAttribute("result", "animation-service-construction-failed");
            return false;
        }
        catch (...)
        {
            RVX_CORE_ERROR("Engine could not construct ECS animation services");
            static_cast<void>(RollbackFailedInitialization());
            m_initializationRollbackPending = true;
            m_shouldShutdown = true;
            startupSpan.SetAttribute("result", "animation-service-construction-failed");
            return false;
        }
    }

    // Feature bridges are per-World consumers, while the gateways own the only feature runtime
    // side tables. Construct all three only after Resource's update-thread manager is live. A
    // host without that seam remains valid for non-World use, but CreateWorld fails closed rather
    // than constructing a partial feature runtime or falling back to legacy Components.
    if (resources == nullptr || !resources->GetManager().IsInitialized())
    {
        RVX_CORE_WARN(
            "ECS feature runtime gateways are unavailable because ResourceSubsystem is not active; World creation will fail closed.");
    }
    else
    {
        try
        {
            m_particleEcsGateway = std::make_unique<Particle::ParticleEcsRuntimeGateway>(
                resources->GetManager());
            m_waterEcsGateway = std::make_unique<Water::WaterEcsRuntimeGateway>();
            m_terrainEcsGateway = std::make_unique<Terrain::TerrainEcsRuntimeGateway>();
        }
        catch (const std::exception& exception)
        {
            RVX_CORE_ERROR(
                "Engine could not construct ECS feature runtime gateways: {}", exception.what());
            static_cast<void>(RollbackFailedInitialization());
            m_initializationRollbackPending = true;
            m_shouldShutdown = true;
            startupSpan.SetAttribute("result", "feature-gateway-construction-failed");
            return false;
        }
        catch (...)
        {
            RVX_CORE_ERROR("Engine could not construct ECS feature runtime gateways");
            static_cast<void>(RollbackFailedInitialization());
            m_initializationRollbackPending = true;
            m_shouldShutdown = true;
            startupSpan.SetAttribute("result", "feature-gateway-construction-failed");
            return false;
        }
    }

    m_initialized = true;
    m_initializationRollbackPending = false;
    m_shouldShutdown = false;
    m_frameNumber = 0;
    RVX_CORE_INFO("=== RenderVerseX Engine Initialized ===");
    startupSpan.SetAttribute("result", "initialized");
    return true;
}

void Engine::Tick()
{
    Time::Update();
    Tick(Time::DeltaTime());
}

void Engine::Tick(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    EventBus::Get().ProcessDeferredEvents();
    TickSubsystems(deltaTime);
    TickWorlds(deltaTime);
    RenderAfterWorlds(deltaTime);
    FinalizeDestroyRequestedWorlds(deltaTime);

    if (auto* window = GetSubsystem<WindowSubsystem>(); window != nullptr && window->ShouldClose())
    {
        RequestShutdown();
    }
    ++m_frameNumber;
}

void Engine::TickWithoutRender()
{
    Time::Update();
    TickWithoutRender(Time::DeltaTime());
}

void Engine::TickWithoutRender(float deltaTime)
{
    if (!m_initialized)
    {
        return;
    }

    EventBus::Get().ProcessDeferredEvents();
    TickSubsystems(deltaTime);
    TickWorlds(deltaTime);
    // The ECS path has no alternate direct-render owner.  Its publication is
    // also the only route by which Render can acknowledge a World-removal
    // proof, so this simulation-oriented entry point must keep it moving.
    RenderAfterWorlds(deltaTime);
    FinalizeDestroyRequestedWorlds(deltaTime);

    if (auto* window = GetSubsystem<WindowSubsystem>(); window != nullptr && window->ShouldClose())
    {
        RequestShutdown();
    }
    ++m_frameNumber;
}

void Engine::Shutdown()
{
    if (!m_initialized)
    {
        if (m_initializationRollbackPending)
        {
            const bool rollbackComplete = RollbackFailedInitialization();
            if (rollbackComplete && m_config.enableJobSystem)
            {
                JobSystem::Get().Shutdown();
            }
            m_initializationRollbackPending = !rollbackComplete;
        }
        return;
    }

    RVX_CORE_INFO("=== RenderVerseX Engine Shutting Down ===");
    m_shouldShutdown = true;

    EngineShutdownDiagnostics shutdown;
    shutdown.available = true;
    shutdown.initializationAttempted = true;
    shutdown.initializationSucceeded = true;
    shutdown.frameNumber = m_frameNumber;
    shutdown.worldCountBeforeShutdown = m_worlds.size();
    shutdown.subsystemCount = m_subsystems.GetCount();

    // World compositions remain live with Resource and Render.  A failed
    // drain intentionally retains all state so a later owner-thread Tick or
    // Shutdown retry can publish the exact active frozen snapshot again.
    ShutdownWorlds();
    shutdown.worldCountAfterShutdown = m_worlds.size();
    shutdown.activeWorldCleared = m_activeWorld == nullptr;
    if (!m_worlds.empty())
    {
        shutdown.clean = false;
        m_lastShutdownDiagnostics = std::move(shutdown);
        RVX_CORE_WARN("Engine shutdown is awaiting {} ECS World drain(s)", m_worlds.size());
        return;
    }

    if (m_ecsRenderComposition != nullptr)
    {
        if (!m_ecsRenderComposition->TryShutdown())
        {
            shutdown.render = m_ecsRenderComposition->GetStats().lastShutdownResult;
            shutdown.clean = false;
            m_lastShutdownDiagnostics = std::move(shutdown);
            RVX_CORE_WARN("Engine shutdown is awaiting ECS render proof or terminal stop");
            return;
        }
        m_lastRenderShutdownResult = m_ecsRenderComposition->GetStats().lastShutdownResult;
    }
    shutdown.render = m_lastRenderShutdownResult;

    // Every World composition has completed its owner-thread drain, including feature bridge
    // side tables and frozen feature snapshots. The Engine-global gateways can now release their
    // final Resource leases while ResourceSubsystem is still initialized.
    m_terrainEcsGateway.reset();
    m_waterEcsGateway.reset();
    m_particleEcsGateway.reset();

    // All World drains completed above. Release evaluator-owned immutable leases before the
    // request service cancels its remaining scene-qualified subscriptions, while Resource is
    // still fully initialized. No individual World ever shuts these Engine owners down.
    if (m_resourceAnimationEvaluator != nullptr)
    {
        m_resourceAnimationEvaluator->Shutdown();
        m_resourceAnimationEvaluator.reset();
    }
    if (m_ecsAnimationAssetService != nullptr)
    {
        m_ecsAnimationAssetService->Shutdown();
        m_ecsAnimationAssetService.reset();
    }

    // No composition can outlive the subsystems it references.
    m_ecsRenderComposition.reset();
    Resource::ResourceSubsystem* resources = GetSubsystem<Resource::ResourceSubsystem>();
    ShutdownSubsystems();
    shutdown.subsystemsStopped = !m_subsystems.IsInitialized();
    if (resources != nullptr)
    {
        shutdown.resourceSubsystemObserved = true;
        const Resource::ResourceShutdownDiagnostics& resourceShutdown =
            resources->GetLastShutdownDiagnostics();
        shutdown.resourceSubsystemClean = resourceShutdown.available && resourceShutdown.clean;
    }

    if (m_config.enableJobSystem)
    {
        JobSystem::Get().Shutdown();
    }
    shutdown.jobSystemStopped = !JobSystem::Get().IsInitialized();
    m_initialized = false;
    shutdown.clean = IsTerminalRenderShutdown(shutdown.render.code) &&
                     shutdown.subsystemsStopped && shutdown.resourceSubsystemClean &&
                     shutdown.worldCountAfterShutdown == 0 && shutdown.activeWorldCleared &&
                     shutdown.jobSystemStopped;
    m_lastShutdownDiagnostics = std::move(shutdown);
    RVX_CORE_INFO("=== RenderVerseX Engine Shutdown Complete ===");
}

World* Engine::CreateWorld(const std::string& name)
{
    WorldConfig config;
    config.name = name;
    return CreateWorld(config);
}

World* Engine::CreateWorld(const WorldConfig& config)
{
    if (!m_initialized)
    {
        RVX_CORE_ERROR("Cannot create World '{}' before Engine initialization", config.name);
        return nullptr;
    }
    if (m_shouldShutdown)
    {
        RVX_CORE_WARN("Cannot create World '{}' after Engine shutdown was requested", config.name);
        return nullptr;
    }
    if (config.name.empty())
    {
        RVX_CORE_ERROR("Cannot create a World with an empty name");
        return nullptr;
    }
    if (m_ecsRenderComposition == nullptr)
    {
        RVX_CORE_ERROR(
            "Cannot create World '{}' without the Engine-global ECS render composition",
            config.name);
        return nullptr;
    }
    if (m_ecsAnimationAssetService == nullptr || m_resourceAnimationEvaluator == nullptr ||
        m_ecsAnimationAssetService->IsShutdown() || m_resourceAnimationEvaluator->IsShutdown())
    {
        RVX_CORE_ERROR(
            "Cannot create World '{}' without active Engine-owned animation services",
            config.name);
        return nullptr;
    }
    if (m_particleEcsGateway == nullptr || m_waterEcsGateway == nullptr ||
        m_terrainEcsGateway == nullptr)
    {
        RVX_CORE_ERROR(
            "Cannot create World '{}' without all Engine-owned ECS feature runtime gateways",
            config.name);
        return nullptr;
    }

    const auto existing = m_worlds.find(config.name);
    if (existing != m_worlds.end())
    {
        if (existing->second.destroyRequested)
        {
            RVX_CORE_WARN("World '{}' is draining and cannot be recreated yet", config.name);
            return nullptr;
        }
        return existing->second.world.get();
    }

    try
    {
        // Build the complete entry before publishing it into the deterministic
        // map. No observer can acquire a World without its initialized bridge
        // composition and global ECS frame pipeline.
        WorldEntry entry;
        entry.world = std::make_unique<World>(config);

        WorldEcsRuntimeCompositionOptions options;
        options.physicsConfig = config.physics;
        options.animationAssetService = m_ecsAnimationAssetService;
        options.resourceAnimationEvaluator = m_resourceAnimationEvaluator;
        options.particleGateway = m_particleEcsGateway.get();
        options.waterGateway = m_waterEcsGateway.get();
        options.terrainGateway = m_terrainEcsGateway.get();
        Resource::ResourceSubsystem* resources = GetSubsystem<Resource::ResourceSubsystem>();
        entry.ecsRuntime = std::make_unique<WorldEcsRuntimeComposition>(
            entry.world->GetSceneEcsRuntime(),
            m_ecsRenderComposition->GetPipeline(),
            resources,
            std::move(options));
        if (!entry.ecsRuntime->Initialize())
        {
            RVX_CORE_ERROR("Failed to initialize ECS composition for World '{}'", config.name);
            return nullptr;
        }

        World* result = entry.world.get();
        m_worlds.emplace(config.name, std::move(entry));
        if (m_activeWorld == nullptr)
        {
            m_activeWorld = result;
        }
        RVX_CORE_INFO("Created ECS World: {}", config.name);
        return result;
    }
    catch (const std::exception& exception)
    {
        RVX_CORE_ERROR("Failed to construct ECS World '{}': {}", config.name, exception.what());
    }
    catch (...)
    {
        RVX_CORE_ERROR("Failed to construct ECS World '{}'", config.name);
    }
    return nullptr;
}

World* Engine::GetWorld(const std::string& name) const
{
    const auto found = m_worlds.find(name);
    return found != m_worlds.end() ? found->second.world.get() : nullptr;
}

void Engine::DestroyWorld(const std::string& name)
{
    const auto found = m_worlds.find(name);
    if (found == m_worlds.end())
    {
        RVX_CORE_WARN("World '{}' not found", name);
        return;
    }
    World* const previouslyActive = m_activeWorld;
    const bool wasDestroyRequested = found->second.destroyRequested;
    if (!BeginWorldShutdown(found->second))
    {
        RVX_CORE_ERROR("World '{}' could not begin ECS shutdown drain", name);
        return;
    }

    if (!wasDestroyRequested && previouslyActive != found->second.world.get())
    {
        const WorldEntry* previousEntry = FindWorldEntry(previouslyActive);
        if (previousEntry != nullptr && !previousEntry->destroyRequested)
        {
            found->second.resumeActiveWorld = previouslyActive;
        }
    }
    else if (!wasDestroyRequested)
    {
        // Active World destruction has no previous active World to restore.
        // Capture a deterministic live successor now, before this entry is
        // removed, so completion never leaves a live Engine with null active
        // World state.
        const auto successor = std::find_if(
            m_worlds.begin(),
            m_worlds.end(),
            [&found](const auto& item)
            {
                return &item != &*found && !item.second.destroyRequested;
            });
        if (successor != m_worlds.end())
        {
            found->second.resumeActiveWorld = successor->second.world.get();
        }
    }

    // Destruction is asynchronous: Tick keeps this World active until the
    // global ECS owner publishes and observes its exact removal proof.
    m_activeWorld = found->second.world.get();
}

IWorldEcsRuntimeServices* Engine::GetWorldEcsRuntimeServices(World* world) noexcept
{
    WorldEntry* entry = FindWorldEntry(world);
    return entry != nullptr ? entry->ecsRuntime.get() : nullptr;
}

void Engine::SetActiveWorld(World* world)
{
    if (world == nullptr)
    {
        const WorldEntry* current = FindWorldEntry(m_activeWorld);
        if (current != nullptr && current->destroyRequested)
        {
            RVX_CORE_WARN("Cannot clear the active World while its ECS removal proof is draining");
            return;
        }
        m_activeWorld = nullptr;
        return;
    }
    const WorldEntry* current = FindWorldEntry(m_activeWorld);
    if (current != nullptr && current->destroyRequested && m_activeWorld != world)
    {
        RVX_CORE_WARN("Cannot switch active Worlds while an ECS removal proof is draining");
        return;
    }
    WorldEntry* entry = FindWorldEntry(world);
    if (entry == nullptr || entry->destroyRequested)
    {
        RVX_CORE_WARN("Cannot activate a World that is not a live Engine-owned ECS World");
        return;
    }
    m_activeWorld = world;
}

void Engine::TickWorlds(float deltaTime)
{
    for (auto& [name, entry] : m_worlds)
    {
        if (entry.ecsRuntime == nullptr)
        {
            continue;
        }

        WorldEcsRuntimeCompositionTickRequest request;
        request.sceneTick.variableDeltaSeconds = deltaTime;
        const Physics::PhysicsWorldConfig& physics = entry.world->GetConfig().physics;
        request.sceneTick.fixedDeltaSeconds = physics.fixedTimeStep;

        if (std::isfinite(deltaTime) && deltaTime >= 0.0f &&
            std::isfinite(physics.fixedTimeStep) && physics.fixedTimeStep > 0.0f)
        {
            entry.fixedTimeAccumulatorSeconds += static_cast<float64>(deltaTime);
            const uint32 requested = ComputeRequestedFixedSteps(
                entry.fixedTimeAccumulatorSeconds,
                static_cast<float64>(physics.fixedTimeStep),
                physics.maxSubSteps);
            request.sceneTick.fixedStepCount = requested;
        }

        const WorldEcsRuntimeCompositionTickResult result = entry.ecsRuntime->Tick(request);
        // Do not subtract requested work. A processor failure can execute a
        // prefix of fixed steps; only Scene's exact executed count consumes
        // the accumulator, preserving all unexecuted simulation time.
        if (result.scene.fixedStepsExecuted != 0 && physics.fixedTimeStep > 0.0f)
        {
            const float64 executedSeconds = static_cast<float64>(result.scene.fixedStepsExecuted) *
                                            static_cast<float64>(physics.fixedTimeStep);
            entry.fixedTimeAccumulatorSeconds = std::max(
                0.0,
                entry.fixedTimeAccumulatorSeconds - executedSeconds);
        }
    }
}

void Engine::ShutdownWorlds()
{
    for (auto& [name, entry] : m_worlds)
    {
        if (!entry.destroyRequested)
        {
            static_cast<void>(BeginWorldShutdown(entry));
        }
    }
    SelectNextWorldForShutdown();

    // Resource completion and Render proof observation are both required by
    // WorldEcsRuntimeComposition::PrepareForShutdown, so drive one full owner
    // frame while the providers are still alive on every retry.
    TickSubsystems(0.0f);
    TickWorlds(0.0f);
    RenderAfterWorlds(0.0f);
    FinalizeDestroyRequestedWorlds(0.0f);
}

void Engine::FinalizeDestroyRequestedWorlds(float deltaTime)
{
    (void)deltaTime;
    if (m_activeWorld == nullptr)
    {
        SelectNextWorldForShutdown();
    }
    WorldEntry* active = FindWorldEntry(m_activeWorld);
    if (active == nullptr || !active->destroyRequested || active->ecsRuntime == nullptr)
    {
        return;
    }

    WorldEcsRuntimeCompositionTickRequest request;
    const Physics::PhysicsWorldConfig& physics = active->world->GetConfig().physics;
    request.sceneTick.fixedDeltaSeconds = physics.fixedTimeStep;
    if (!active->ecsRuntime->PrepareForShutdown(request))
    {
        return;
    }

    const auto completed = std::find_if(
        m_worlds.begin(),
        m_worlds.end(),
        [active](const auto& item) { return &item.second == active; });
    if (completed == m_worlds.end())
    {
        return;
    }
    World* const resumeActiveWorld = completed->second.resumeActiveWorld;
    RVX_CORE_INFO("Destroyed ECS World: {}", completed->first);
    m_worlds.erase(completed);
    m_activeWorld = nullptr;
    const WorldEntry* resumeEntry = FindWorldEntry(resumeActiveWorld);
    if (resumeEntry != nullptr && !resumeEntry->destroyRequested)
    {
        m_activeWorld = resumeActiveWorld;
    }
    SelectNextWorldForShutdown();
}

bool Engine::BeginWorldShutdown(WorldEntry& entry)
{
    if (entry.destroyRequested)
    {
        return true;
    }
    if (entry.ecsRuntime == nullptr || !entry.ecsRuntime->BeginShutdown())
    {
        return false;
    }
    entry.destroyRequested = true;
    return true;
}

Engine::WorldEntry* Engine::FindWorldEntry(World* world) noexcept
{
    if (world == nullptr)
    {
        return nullptr;
    }
    const auto found = std::find_if(
        m_worlds.begin(),
        m_worlds.end(),
        [world](const auto& item) { return item.second.world.get() == world; });
    return found != m_worlds.end() ? &found->second : nullptr;
}

const Engine::WorldEntry* Engine::FindWorldEntry(World* world) const noexcept
{
    if (world == nullptr)
    {
        return nullptr;
    }
    const auto found = std::find_if(
        m_worlds.begin(),
        m_worlds.end(),
        [world](const auto& item) { return item.second.world.get() == world; });
    return found != m_worlds.end() ? &found->second : nullptr;
}

void Engine::SelectNextWorldForShutdown()
{
    if (m_activeWorld != nullptr)
    {
        WorldEntry* active = FindWorldEntry(m_activeWorld);
        if (active != nullptr)
        {
            return;
        }
    }

    const auto live = std::find_if(
        m_worlds.begin(),
        m_worlds.end(),
        [](const auto& item) { return !item.second.destroyRequested; });
    if (live != m_worlds.end())
    {
        m_activeWorld = live->second.world.get();
        return;
    }

    const auto draining = std::find_if(
        m_worlds.begin(),
        m_worlds.end(),
        [](const auto& item) { return item.second.destroyRequested; });
    if (draining != m_worlds.end())
    {
        m_activeWorld = draining->second.world.get();
    }
    else if (m_activeWorld != nullptr && FindWorldEntry(m_activeWorld) == nullptr)
    {
        m_activeWorld = nullptr;
    }
}

bool Engine::InitializeSubsystems()
{
    Diagnostics::TraceSpan subsystemsSpan = Diagnostics::BeginTraceSpan(
        m_config.startupTraceContext,
        "Engine.Subsystems.Initialize");
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
                "ECS render composition requires ResourceSubsystem and WindowSubsystem");
            subsystemsSpan.SetAttribute("result", "missing-required-subsystem");
            return false;
        }

        const auto windowDependency =
            m_subsystems.AddInitializationDependency<RenderSubsystem, WindowSubsystem>();
        const auto resourceDependency = m_subsystems.AddInitializationDependency<
            RenderSubsystem,
            Resource::ResourceSubsystem>();
        if (!windowDependency.IsAccepted() || !resourceDependency.IsAccepted())
        {
            RVX_CORE_ERROR("Failed to register ECS Render composition dependencies");
            subsystemsSpan.SetAttribute("result", "dependency-registration-failed");
            return false;
        }

        auto services = CreateEngineEcsRenderRuntimeCompositionServices(
            *resources,
            *render,
            *window,
            [this]() { return m_subsystems.DeinitializeSubsystem<RenderSubsystem>(); });
        m_ecsRenderComposition = std::make_unique<EcsRenderRuntimeComposition>(
            m_config.renderRuntime,
            m_config.initialRenderFrameSettings,
            std::move(services));
        if (!m_ecsRenderComposition->PrepareBeforeSubsystemInitialization())
        {
            RVX_CORE_ERROR("Failed to prepare ECS Render composition");
            subsystemsSpan.SetAttribute("result", "render-composition-prepare-failed");
            return false;
        }
    }

    Diagnostics::TraceSpan subsystemInitializationSpan;
    const bool initialized = m_subsystems.InitializeAll(
        [this, &subsystemInitializationSpan](EngineSubsystem& subsystem)
        {
            subsystemInitializationSpan = Diagnostics::BeginTraceSpan(
                m_config.startupTraceContext,
                "EngineSubsystemInitialize",
                {{"name", subsystem.GetName()}});
            if (m_ecsRenderComposition != nullptr &&
                &subsystem == GetSubsystem<RenderSubsystem>())
            {
                m_ecsRenderComposition->BeforeRenderSubsystemInitialize();
            }
        },
        [this, &subsystemInitializationSpan](EngineSubsystem& subsystem)
        {
            subsystemInitializationSpan.SetAttribute("result", "initialized");
            subsystemInitializationSpan.End();
            if (&subsystem == GetSubsystem<WindowSubsystem>())
            {
                Diagnostics::RecordTraceInstant(m_config.startupTraceContext, "WindowReady");
            }
            if (&subsystem == GetSubsystem<RenderSubsystem>())
            {
                Diagnostics::RecordTraceInstant(
                    m_config.startupTraceContext,
                    "EcsRenderRuntimeReady");
            }
        });
    subsystemsSpan.SetAttribute("result", initialized ? "initialized" : "failed");
    return initialized;
}

void Engine::TickSubsystems(float deltaTime)
{
    m_subsystems.TickAll(deltaTime);
}

void Engine::ShutdownSubsystems()
{
    m_subsystems.DeinitializeAll();
}

bool Engine::RollbackFailedInitialization()
{
    // InitializeAll may already have unwound a failed subsystem sequence, but
    // the configured ECS render owner still holds the only proof/lifecycle
    // contract for that sequence. Never reset it directly: first request its
    // ordered Resource -> Render terminal drain while the subsystem objects
    // (and their service seams) are still retained by this Engine.
    if (m_ecsRenderComposition != nullptr &&
        m_ecsRenderComposition->GetStats().renderConfigured)
    {
        if (!m_ecsRenderComposition->TryShutdown())
        {
            m_lastRenderShutdownResult =
                m_ecsRenderComposition->GetStats().lastShutdownResult;
            RVX_CORE_ERROR(
                "Engine initialization rollback is awaiting ECS render terminal shutdown");
            return false;
        }
        m_lastRenderShutdownResult =
            m_ecsRenderComposition->GetStats().lastShutdownResult;
    }

    if (m_ecsRenderComposition != nullptr &&
        m_ecsRenderComposition->GetStats().renderConfigured &&
        !m_ecsRenderComposition->GetStats().shutdownComplete)
    {
        return false;
    }

    // This path runs before ResourceSubsystem deinitialization. It is safe to discard only an
    // un-published or failed-initialization animation owner here; a successful runtime takes the
    // normal World-drain path in Shutdown().
    if (m_resourceAnimationEvaluator != nullptr)
    {
        m_resourceAnimationEvaluator->Shutdown();
        m_resourceAnimationEvaluator.reset();
    }
    if (m_ecsAnimationAssetService != nullptr)
    {
        m_ecsAnimationAssetService->Shutdown();
        m_ecsAnimationAssetService.reset();
    }

    // Failed initialization never publishes a World, so no bridge consumer can retain these
    // owners. Release them before ResourceSubsystem is deinitialized.
    m_terrainEcsGateway.reset();
    m_waterEcsGateway.reset();
    m_particleEcsGateway.reset();

    // An unconfigured prepared owner has no Render state or proof to drain.
    m_ecsRenderComposition.reset();
    m_activeWorld = nullptr;
    m_worlds.clear();
    ShutdownSubsystems();
    return true;
}

void Engine::RenderAfterWorlds(float deltaTime)
{
    if (m_ecsRenderComposition == nullptr)
    {
        return;
    }

    const WorldEntry* active = FindWorldEntry(m_activeWorld);
    if (active == nullptr || active->ecsRuntime == nullptr ||
        (active->destroyRequested &&
         !active->ecsRuntime->NeedsRenderPublicationDuringShutdown()))
    {
        // Final camera retirement deliberately produces a camera-less Scene
        // cleanup tick. Never publish that snapshot; still observe completed
        // progress so a previously published removal can release its proof.
        static_cast<void>(m_ecsRenderComposition->PollCompletedProgress());
        return;
    }

    const std::shared_ptr<const SceneECS::FrozenSceneSnapshot> snapshot =
        active->world->GetSceneEcsRuntime().GetLatestFrozenSnapshot();
    static_cast<void>(m_ecsRenderComposition->Tick(
        snapshot.get(), deltaTime, static_cast<float32>(Time::ElapsedTime())));
}

bool Engine::SetRenderFrameSettings(const RenderFrameSettings& settings) noexcept
{
    if (m_ecsRenderComposition != nullptr)
    {
        return m_ecsRenderComposition->SetFrameSettings(settings);
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
    return m_ecsRenderComposition != nullptr ? m_ecsRenderComposition->GetFrameSettings() :
                                               m_config.initialRenderFrameSettings;
}

uint64 Engine::RequestRenderTemporalReset() noexcept
{
    return m_ecsRenderComposition != nullptr ? m_ecsRenderComposition->RequestTemporalReset() : 0;
}

bool Engine::RequestRenderFrameCapture(const RenderFrameCaptureRequest& request) noexcept
{
    return m_ecsRenderComposition != nullptr && m_ecsRenderComposition->QueueCapture(request);
}

bool Engine::RequestRenderSurfaceResize(uint32 width, uint32 height)
{
    return m_ecsRenderComposition != nullptr &&
           m_ecsRenderComposition->RequestSurfaceResize(width, height);
}

EngineRenderRuntimeDiagnostics Engine::GetRenderRuntimeDiagnostics() const noexcept
{
    EngineRenderRuntimeDiagnostics diagnostics;
    if (!m_initialized || m_ecsRenderComposition == nullptr)
    {
        return diagnostics;
    }

    const EcsRenderRuntimeCompositionStats& stats = m_ecsRenderComposition->GetStats();
    diagnostics.available = stats.renderConfigured && !stats.shutdownComplete;
    diagnostics.temporalEpoch = stats.temporalEpoch;
    diagnostics.temporalResetCount = stats.temporalResetCount;
    if (stats.lastAcceptedFrame.has_value())
    {
        const EcsRenderAcceptedFrameEvidence& accepted = *stats.lastAcceptedFrame;
        diagnostics.requiredSceneFrameSequence = accepted.carryingFrameSequence;
        diagnostics.requiredSceneRevision = accepted.targetRenderSceneRevision;
        diagnostics.cameraIdentity = accepted.selectedCamera.GetPackedValue();
        diagnostics.cameraCutRevision = accepted.selectedCameraCutRevision;
        diagnostics.acceptedExtractionDiagnostics.acceptedPublicationCount = stats.publicationAccepted;
        diagnostics.acceptedExtractionDiagnostics.lastAcceptedSourceFrameSequence =
            accepted.carryingFrameSequence;
        diagnostics.acceptedExtractionDiagnostics.lastAcceptedSceneRevision =
            accepted.targetRenderSceneRevision;
    }
    return diagnostics;
}
} // namespace RVX
