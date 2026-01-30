#pragma once

/**
 * @file DebugSubsystem.h
 * @brief Engine subsystem for debug and profiling features
 * 
 * Provides centralized management of:
 * - CPU/GPU profiling
 * - Memory tracking
 * - Console and CVars
 * - Statistics HUD
 * - Built-in debug CVars
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Debug/CPUProfiler.h"
#include "Debug/MemoryTracker.h"
#include "Debug/Console.h"
#include "Debug/CVarSystem.h"
#include "Debug/StatsHUD.h"

namespace RVX
{
    /**
     * @brief Debug subsystem for engine integration
     * 
     * Usage:
     * @code
     * // Add to engine subsystem collection
     * engine.GetSubsystems().Add<DebugSubsystem>();
     * 
     * // Access via subsystem
     * auto& debug = engine.GetSubsystems().Get<DebugSubsystem>();
     * debug.GetProfiler().BeginFrame();
     * @endcode
     */
    class DebugSubsystem : public EngineSubsystem
    {
    public:
        DebugSubsystem() = default;
        ~DebugSubsystem() override = default;

        // =========================================================================
        // EngineSubsystem Interface
        // =========================================================================

        const char* GetName() const override { return "DebugSubsystem"; }

        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;
        bool ShouldTick() const override { return true; }
        TickPhase GetTickPhase() const override { return TickPhase::PostRender; }

        // =========================================================================
        // Component Access
        // =========================================================================

        /**
         * @brief Get the CPU profiler
         */
        CPUProfiler& GetProfiler() { return CPUProfiler::Get(); }

        /**
         * @brief Get the memory tracker
         */
        MemoryTracker& GetMemoryTracker() { return MemoryTracker::Get(); }

        /**
         * @brief Get the console
         */
        Console& GetConsole() { return Console::Get(); }

        /**
         * @brief Get the CVar system
         */
        CVarSystem& GetCVars() { return CVarSystem::Get(); }

        /**
         * @brief Get the stats HUD
         */
        StatsHUD& GetStatsHUD() { return StatsHUD::Get(); }

        // =========================================================================
        // Frame Profiling Helpers
        // =========================================================================

        /**
         * @brief Begin frame profiling (call at start of frame)
         */
        void BeginFrame();

        /**
         * @brief End frame profiling (call at end of frame)
         */
        void EndFrame();

        // =========================================================================
        // Built-in CVars
        // =========================================================================

        /**
         * @brief Check if profiling is enabled
         */
        bool IsProfilingEnabled() const;

        /**
         * @brief Check if memory tracking is enabled
         */
        bool IsMemoryTrackingEnabled() const;

        /**
         * @brief Check if stats HUD is visible
         */
        bool IsStatsHUDVisible() const;

        /**
         * @brief Get the current stats display mode
         */
        StatsDisplayMode GetStatsDisplayMode() const;

    private:
        void RegisterBuiltinCVars();
        void RegisterDebugCommands();

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;

        // Built-in CVar references
        CVarRef m_cvarProfilingEnabled;
        CVarRef m_cvarMemoryTracking;
        CVarRef m_cvarStatsHUD;
        CVarRef m_cvarStatsMode;
        CVarRef m_cvarShowFPS;
    };

} // namespace RVX
