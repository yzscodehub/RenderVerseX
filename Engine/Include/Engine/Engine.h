#pragma once

/**
 * @file Engine.h
 * @brief Main engine class - coordinates all subsystems
 */

#include "Core/Event/EventBus.h"
#include "Core/Diagnostics/Trace.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Subsystem/SubsystemCollection.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    class RenderRuntimeComposition;
    class World;
    /**
     * @brief Engine configuration
     */
    struct EngineConfig
    {
        const char* appName = "RenderVerseX";
        uint32_t windowWidth = 1280;
        uint32_t windowHeight = 720;
        bool vsync = true;
        bool enableJobSystem = true;
        size_t jobWorkerCount = 0;  // 0 = auto (hardware concurrency)
        /** @brief Optional correlated diagnostics context; disabled by default. */
        Diagnostics::TraceContext startupTraceContext{};
        RenderRuntimeConfig renderRuntime{};
        RenderFrameSettings initialRenderFrameSettings{};
    };

    /**
     * @brief Main engine class
     * 
     * The Engine coordinates all subsystems and provides the main game loop.
     * Uses the EngineSubsystem pattern for modular, dependency-aware initialization.
     * 
     * Built-in subsystems (from Runtime module):
     * - WindowSubsystem: Window creation and management
     * - InputSubsystem: Input polling and events
     * - TimeSubsystem: Frame timing
     * 
     * Render subsystems (from Render module):
     * - RenderSubsystem: Rendering coordination
     * 
     * Usage:
     * @code
     * Engine engine;
     * engine.SetConfig(config);
     * 
     * // Add subsystems (order doesn't matter - dependencies are resolved automatically)
     * engine.AddSubsystem<WindowSubsystem>();
     * engine.AddSubsystem<InputSubsystem>();
     * engine.AddSubsystem<RenderSubsystem>();
     * 
     * engine.Initialize();
     * 
     * while (!engine.ShouldShutdown()) {
     *     engine.Tick();
     * }
     * 
     * engine.Shutdown();
     * @endcode
     */
    class Engine
    {
    public:
        Engine();
        ~Engine();

        // Non-copyable
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;

        /// Get the global engine instance (if using singleton pattern)
        static Engine* Get();

        // =====================================================================
        // Configuration
        // =====================================================================

        /// Set engine configuration (call before Initialize)
        void SetConfig(const EngineConfig& config) { m_config = config; }
        const EngineConfig& GetConfig() const { return m_config; }

        // =====================================================================
        // Subsystem Management
        // =====================================================================

        /**
         * @brief Add a subsystem
         * @tparam T Subsystem type (must derive from EngineSubsystem)
         * @return Pointer to the added subsystem
         */
        template<typename T, typename... Args>
        T* AddSubsystem(Args&&... args)
        {
            auto* subsystem = m_subsystems.AddSubsystem<T>(std::forward<Args>(args)...);
            subsystem->SetEngine(this);
            return subsystem;
        }

        /**
         * @brief Get a subsystem by type
         * @return Pointer to subsystem or nullptr if not found
         */
        template<typename T>
        T* GetSubsystem() const
        {
            return m_subsystems.GetSubsystem<T>();
        }

        /**
         * @brief Check if a subsystem exists
         */
        template<typename T>
        bool HasSubsystem() const
        {
            return m_subsystems.HasSubsystem<T>();
        }

        // =====================================================================
        // World Management
        // =====================================================================

        /**
         * @brief Create a new world
         * @param name Name of the world (default: "Main")
         * @return Pointer to the created world
         */
        World* CreateWorld(const std::string& name = "Main");

        /**
         * @brief Get a world by name
         * @param name Name of the world
         * @return Pointer to the world or nullptr if not found
         */
        World* GetWorld(const std::string& name = "Main") const;

        /**
         * @brief Destroy a world by name
         * @param name Name of the world to destroy
         */
        void DestroyWorld(const std::string& name);

        /**
         * @brief Set the active world for rendering
         * @param world The world to set as active (must be owned by this engine)
         */
        void SetActiveWorld(World* world);

        /**
         * @brief Get the currently active world
         * @return Pointer to the active world or nullptr
         */
        World* GetActiveWorld() const { return m_activeWorld; }

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize the engine and all subsystems
        bool Initialize();

        /// Process one frame (uses internal time tracking)
        void Tick();

        /// Process one frame with explicit delta time
        void Tick(float deltaTime);

        /**
         * @brief Process one frame without rendering
         * 
         * Use this when you need manual control over rendering.
         * Call RenderSubsystem methods directly for rendering.
         */
        void TickWithoutRender();

        /**
         * @brief Process one frame without rendering, with explicit delta time
         */
        void TickWithoutRender(float deltaTime);

        /// Shutdown the engine
        void Shutdown();

        /**
         * @brief Stop and drain rendering while Worlds and resources stay alive.
         *
         * Host layers use this boundary before releasing external scene or
         * resource owners. Shutdown() calls it automatically and idempotently.
         */
        void ShutdownRenderRuntime();

        /// Request engine shutdown (sets shutdown flag)
        void RequestShutdown() { m_shouldShutdown = true; }

        /// Check if shutdown was requested
        bool ShouldShutdown() const { return m_shouldShutdown; }

        /// Check if engine is initialized
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Update-Owned Render Controls
        // =====================================================================

        /** @brief Replace settings copied into subsequently extracted frames. */
        [[nodiscard]] bool SetRenderFrameSettings(
            const RenderFrameSettings& settings) noexcept;
        [[nodiscard]] const RenderFrameSettings&
            GetRenderFrameSettings() const noexcept;
        /** @brief Advance the temporal epoch and reset the next accepted frame. */
        [[nodiscard]] uint64 RequestRenderTemporalReset() noexcept;
        /** @brief Queue one owned capture request for the next accepted frame. */
        [[nodiscard]] bool RequestRenderFrameCapture(
            const RenderFrameCaptureRequest& request) noexcept;
        /** @brief Queue an explicit render-surface extent generation. */
        [[nodiscard]] bool RequestRenderSurfaceResize(uint32 width,
                                                      uint32 height);
        [[nodiscard]] const RenderShutdownResult&
            GetLastRenderShutdownResult() const noexcept
        {
            return m_lastRenderShutdownResult;
        }

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the event bus
        EventBus& GetEventBus() { return EventBus::Get(); }

        /// Get current frame number
        uint64_t GetFrameNumber() const { return m_frameNumber; }

        /// Compatibility alias
        bool Init() { return Initialize(); }

    private:
        bool InitializeSubsystems();
        void TickSubsystems(float deltaTime);
        void ShutdownSubsystems();
        void TickWorlds(float deltaTime);
        void ShutdownWorlds();
        void RenderAfterWorlds(float deltaTime);

        EngineConfig m_config;
        SubsystemCollection<EngineSubsystem> m_subsystems;

        // World management
        std::unordered_map<std::string, std::unique_ptr<World>> m_worlds;
        World* m_activeWorld = nullptr;
        std::unique_ptr<RenderRuntimeComposition> m_renderComposition;
        RenderShutdownResult m_lastRenderShutdownResult{};

        bool m_initialized = false;
        bool m_shouldShutdown = false;
        uint64_t m_frameNumber = 0;

        static Engine* s_instance;
    };

} // namespace RVX
