#pragma once

/**
 * @file Engine.h
 * @brief Main engine class - coordinates all subsystems
 */

#include "Core/SystemManager.h"
#include "Core/Subsystem/SubsystemCollection.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Event/EventBus.h"
#include <functional>
#include <memory>

namespace RVX
{
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
    };

    /**
     * @brief Main engine class
     * 
     * The Engine coordinates all subsystems and provides the main game loop.
     * It supports both the legacy SystemManager pattern and the new
     * EngineSubsystem pattern.
     * 
     * Usage (new style):
     * @code
     * Engine engine;
     * engine.AddSubsystem<MySubsystem>();
     * engine.Initialize();
     * 
     * while (!engine.ShouldShutdown()) {
     *     engine.Tick();
     * }
     * 
     * engine.Shutdown();
     * @endcode
     * 
     * Usage (legacy style, still supported):
     * @code
     * Engine engine;
     * engine.GetSystemManager().RegisterSystem<MySystem>(...);
     * engine.Run(shouldExit, getDeltaTime);
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
        // Subsystem Management (New API)
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
        // Legacy System Manager (for backward compatibility)
        // =====================================================================

        /// Get the legacy system manager
        [[deprecated("Use AddSubsystem/GetSubsystem instead")]]
        SystemManager& GetSystemManager() { return m_legacySystems; }

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /// Initialize the engine and all subsystems
        void Initialize();

        /// Process one frame
        void Tick();

        /// Tick with explicit delta time (legacy compatibility)
        void Tick(float deltaTime);

        /// Shutdown the engine
        void Shutdown();

        /// Request engine shutdown (sets shutdown flag)
        void RequestShutdown() { m_shouldShutdown = true; }

        /// Check if shutdown was requested
        bool ShouldShutdown() const { return m_shouldShutdown; }

        /// Check if engine is initialized
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Legacy Run Loop (for backward compatibility)
        // =====================================================================

        /// Legacy run method (deprecated, use Initialize/Tick/Shutdown instead)
        [[deprecated("Use Initialize/Tick/Shutdown loop instead")]]
        void Run(const std::function<bool()>& shouldExit, 
                 const std::function<float()>& getDeltaTime);

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the event bus
        EventBus& GetEventBus() { return EventBus::Get(); }

        /// Get current frame number
        uint64_t GetFrameNumber() const { return m_frameNumber; }

        /// Compatibility aliases
        void Init() { Initialize(); }

    private:
        void InitializeSubsystems();
        void TickSubsystems(float deltaTime);
        void ShutdownSubsystems();

        EngineConfig m_config;
        SubsystemCollection<EngineSubsystem> m_subsystems;
        SystemManager m_legacySystems;  // For backward compatibility

        bool m_initialized = false;
        bool m_shouldShutdown = false;
        uint64_t m_frameNumber = 0;

        static Engine* s_instance;
    };

} // namespace RVX
