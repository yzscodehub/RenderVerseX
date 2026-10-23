/**
 * @file DebugSubsystem.cpp
 * @brief Debug subsystem implementation
 */

#include "Debug/DebugSubsystem.h"
#include "Core/Log.h"

namespace RVX
{
    // =========================================================================
    // EngineSubsystem Interface
    // =========================================================================

    void DebugSubsystem::Initialize()
    {
        if (m_initialized)
        {
            return;
        }

        RVX_CORE_INFO("Initializing DebugSubsystem...");

        // Initialize components in order
        Console::Get().Initialize();
        CVarSystem::Get().Initialize();
        CPUProfiler::Get().Initialize();
        MemoryTracker::Get().Initialize();
        StatsHUD::Get().Initialize();

        // Register built-in CVars and commands
        RegisterBuiltinCVars();
        RegisterDebugCommands();

        // Register CVar console commands
        CVarSystem::Get().RegisterConsoleCommands();

        m_initialized = true;
        RVX_CORE_INFO("DebugSubsystem initialized successfully");
    }

    void DebugSubsystem::Deinitialize()
    {
        if (!m_initialized)
        {
            return;
        }

        RVX_CORE_INFO("Shutting down DebugSubsystem...");

        // Shutdown in reverse order
        StatsHUD::Get().Shutdown();
        MemoryTracker::Get().Shutdown();
        CPUProfiler::Get().Shutdown();
        CVarSystem::Get().Shutdown();
        Console::Get().Shutdown();

        m_initialized = false;
        RVX_CORE_INFO("DebugSubsystem shutdown complete");
    }

    void DebugSubsystem::Tick(float deltaTime)
    {
        if (!m_initialized)
        {
            return;
        }

        // Update stats HUD
        StatsHUD::Get().Update(deltaTime);

        // Update memory tracker frame index
        MemoryTracker::Get().SetFrameIndex(CPUProfiler::Get().GetLastFrame().frameIndex);
    }

    // =========================================================================
    // Frame Profiling Helpers
    // =========================================================================

    void DebugSubsystem::BeginFrame()
    {
        if (IsProfilingEnabled())
        {
            CPUProfiler::Get().BeginFrame();
        }
    }

    void DebugSubsystem::EndFrame()
    {
        if (IsProfilingEnabled())
        {
            CPUProfiler::Get().EndFrame();

            // Record frame time to stats HUD
            StatsHUD::Get().RecordFrameTime(CPUProfiler::Get().GetLastFrame().frameTimeMs);
        }
    }

    // =========================================================================
    // Built-in CVars Access
    // =========================================================================

    bool DebugSubsystem::IsProfilingEnabled() const
    {
        return m_cvarProfilingEnabled.IsValid() ? m_cvarProfilingEnabled.GetBool() : true;
    }

    bool DebugSubsystem::IsMemoryTrackingEnabled() const
    {
        return m_cvarMemoryTracking.IsValid() ? m_cvarMemoryTracking.GetBool() : true;
    }

    bool DebugSubsystem::IsStatsHUDVisible() const
    {
        return m_cvarStatsHUD.IsValid() ? m_cvarStatsHUD.GetBool() : true;
    }

    StatsDisplayMode DebugSubsystem::GetStatsDisplayMode() const
    {
        if (m_cvarStatsMode.IsValid())
        {
            return static_cast<StatsDisplayMode>(m_cvarStatsMode.GetInt());
        }
        return StatsDisplayMode::Basic;
    }

    // =========================================================================
    // Registration
    // =========================================================================

    void DebugSubsystem::RegisterBuiltinCVars()
    {
        auto& cvars = CVarSystem::Get();

        // Profiling
        m_cvarProfilingEnabled = cvars.RegisterBool(
            "debug.profiling",
            true,
            "Enable CPU/GPU profiling",
            CVarFlags::Archive
        );

        m_cvarMemoryTracking = cvars.RegisterBool(
            "debug.memoryTracking",
            true,
            "Enable memory allocation tracking",
            CVarFlags::Archive
        );

        // Stats HUD
        m_cvarStatsHUD = cvars.RegisterBool(
            "debug.statsHUD",
            true,
            "Show statistics HUD",
            CVarFlags::Archive
        );

        m_cvarStatsMode = cvars.RegisterInt(
            "debug.statsMode",
            static_cast<int32>(StatsDisplayMode::Basic),
            "Stats display mode (0=Hidden, 1=Minimal, 2=Basic, 3=Extended, 4=Full)",
            0, 4,
            CVarFlags::Archive
        );

        m_cvarShowFPS = cvars.RegisterBool(
            "debug.showFPS",
            true,
            "Show FPS counter",
            CVarFlags::Archive
        );

        // Register callbacks
        cvars.RegisterCallback(m_cvarProfilingEnabled, [](const CVarValue&, const CVarValue& newVal) {
            CPUProfiler::Get().SetEnabled(std::get<bool>(newVal));
        });

        cvars.RegisterCallback(m_cvarMemoryTracking, [](const CVarValue&, const CVarValue& newVal) {
            MemoryTracker::Get().SetEnabled(std::get<bool>(newVal));
        });

        cvars.RegisterCallback(m_cvarStatsHUD, [](const CVarValue&, const CVarValue& newVal) {
            StatsHUD::Get().SetVisible(std::get<bool>(newVal));
        });

        cvars.RegisterCallback(m_cvarStatsMode, [](const CVarValue&, const CVarValue& newVal) {
            StatsHUD::Get().SetDisplayMode(static_cast<StatsDisplayMode>(std::get<int32>(newVal)));
        });

        RVX_CORE_DEBUG("Registered {} debug CVars", 5);
    }

    void DebugSubsystem::RegisterDebugCommands()
    {
        auto& console = Console::Get();

        // Memory commands
        console.RegisterCommand({
            .name = "mem_stats",
            .description = "Print memory statistics",
            .usage = "mem_stats",
            .handler = [](const CommandArgs&) -> CommandResult {
                MemoryTracker::Get().PrintSummary();
                return CommandResult::Success("Memory stats printed to log");
            }
        });

        console.RegisterCommand({
            .name = "mem_leaks",
            .description = "Check for memory leaks",
            .usage = "mem_leaks",
            .handler = [](const CommandArgs&) -> CommandResult {
                if (MemoryTracker::Get().HasLeaks())
                {
                    MemoryTracker::Get().PrintLeaks();
                    return CommandResult::Error("Memory leaks detected!");
                }
                return CommandResult::Success("No memory leaks detected");
            }
        });

        console.RegisterCommand({
            .name = "mem_snapshot",
            .description = "Take a memory snapshot",
            .usage = "mem_snapshot",
            .handler = [](const CommandArgs&) -> CommandResult {
                uint32 id = MemoryTracker::Get().TakeSnapshot();
                return CommandResult::Success("Snapshot created with ID: " + std::to_string(id));
            }
        });

        // Profiler commands
        console.RegisterCommand({
            .name = "profile_dump",
            .description = "Dump profiling data to log",
            .usage = "profile_dump",
            .handler = [](const CommandArgs&) -> CommandResult {
                const auto& frame = CPUProfiler::Get().GetLastFrame();

                std::vector<std::string> lines;
                lines.push_back("=== CPU Profile Dump ===");
                lines.push_back("Frame: " + std::to_string(frame.frameIndex));
                lines.push_back("Frame Time: " + std::to_string(frame.frameTimeMs) + " ms");
                lines.push_back("Avg Frame Time: " + std::to_string(frame.avgFrameTimeMs) + " ms");
                lines.push_back("");

                for (const auto& scope : frame.scopes)
                {
                    std::string indent(scope.depth * 2, ' ');
                    lines.push_back(indent + scope.name + ": " + std::to_string(scope.timeMs) + " ms");
                }

                return CommandResult::Output(lines);
            }
        });

        // Stats commands
        console.RegisterCommand({
            .name = "stat",
            .description = "Toggle or set stats display",
            .usage = "stat [mode]",
            .handler = [this](const CommandArgs& args) -> CommandResult {
                if (args.Count() == 0)
                {
                    // Toggle visibility
                    bool visible = !StatsHUD::Get().IsVisible();
                    StatsHUD::Get().SetVisible(visible);
                    return CommandResult::Success(visible ? "Stats enabled" : "Stats disabled");
                }

                const std::string& mode = args.GetString(0);

                if (mode == "fps")
                {
                    StatsHUD::Get().SetDisplayMode(StatsDisplayMode::Minimal);
                }
                else if (mode == "basic")
                {
                    StatsHUD::Get().SetDisplayMode(StatsDisplayMode::Basic);
                }
                else if (mode == "extended" || mode == "ext")
                {
                    StatsHUD::Get().SetDisplayMode(StatsDisplayMode::Extended);
                }
                else if (mode == "full" || mode == "all")
                {
                    StatsHUD::Get().SetDisplayMode(StatsDisplayMode::Full);
                }
                else if (mode == "off" || mode == "none")
                {
                    StatsHUD::Get().SetDisplayMode(StatsDisplayMode::Hidden);
                }
                else
                {
                    return CommandResult::Error("Unknown mode: " + mode +
                        ". Use: fps, basic, extended, full, or off");
                }

                return CommandResult::Success("Stats mode set to: " + mode);
            },
            .aliases = {"stats"}
        });

        // Version/info command
        console.RegisterCommand({
            .name = "version",
            .description = "Show engine version",
            .usage = "version",
            .handler = [](const CommandArgs&) -> CommandResult {
                std::vector<std::string> lines;
                lines.push_back("RenderVerseX Engine");
                lines.push_back("Version: 0.1.0");
                lines.push_back("Build: " + std::string(__DATE__) + " " + __TIME__);
#ifdef RVX_DEBUG
                lines.push_back("Configuration: Debug");
#else
                lines.push_back("Configuration: Release");
#endif
                return CommandResult::Output(lines);
            }
        });

        // Quit command
        console.RegisterCommand({
            .name = "quit",
            .description = "Request application exit",
            .usage = "quit",
            .handler = [](const CommandArgs&) -> CommandResult {
                RVX_CORE_INFO("Quit requested via console");
                // Note: Actual quit logic would be handled by the application
                return CommandResult::Success("Quit requested");
            },
            .aliases = {"exit", "q"}
        });

        RVX_CORE_DEBUG("Registered debug console commands");
    }

} // namespace RVX
