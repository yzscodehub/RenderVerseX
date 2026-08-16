#pragma once

/**
 * @file Engine.h
 * @brief Main engine class - coordinates all subsystems
 */

#include "Core/Event/EventBus.h"
#include "Core/Diagnostics/Trace.h"
#include "Core/Subsystem/EngineSubsystem.h"
#include "Core/Subsystem/SubsystemCollection.h"
#include "Engine/ECS/IWorldEcsRuntimeServices.h"
#include "Engine/RenderRuntimeDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace RVX
{
    // Forward declarations
    class EcsRenderRuntimeComposition;
    class WorldEcsRuntimeComposition;
    class World;
    struct WorldConfig;
    namespace Particle
    {
        class ParticleEcsRuntimeGateway;
    }
    namespace Water
    {
        class WaterEcsRuntimeGateway;
    }
    namespace Terrain
    {
        class TerrainEcsRuntimeGateway;
    }
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

    /** @brief Immutable evidence captured across the most recent shutdown. */
    struct EngineShutdownDiagnostics
    {
        bool available = false;
        bool initializationAttempted = false;
        bool initializationSucceeded = false;
        uint64 frameNumber = 0;
        size_t worldCountBeforeShutdown = 0;
        size_t worldCountAfterShutdown = 0;
        size_t subsystemCount = 0;
        bool subsystemsStopped = false;
        bool resourceSubsystemObserved = false;
        bool resourceSubsystemClean = true;
        bool activeWorldCleared = false;
        bool jobSystemStopped = false;
        RenderShutdownResult render{};
        bool clean = false;
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
         * @brief Create a World from a complete pre-initialization contract.
         * @param config World configuration applied before subsystems initialize.
         * @return Pointer to the created world
         */
        World* CreateWorld(const WorldConfig& config);

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

        /** @brief Return the Engine-owned ECS request and diagnostics service for a World. */
        [[nodiscard]] IWorldEcsRuntimeServices*
        GetWorldEcsRuntimeServices(World* world) noexcept;

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
         * @brief Process simulation without a separate legacy render call.
         *
         * The production ECS path still publishes the active immutable Scene
         * snapshot through its Engine-owned frame pipeline.  Publication and
         * completion observation are required to advance exact presentation
         * proof, including asynchronous World removal.  This method therefore
         * must not be paired with direct RenderSubsystem calls.
         */
        void TickWithoutRender();

        /**
         * @brief Process one frame without rendering, with explicit delta time
         */
        void TickWithoutRender(float deltaTime);

        /// Shutdown the engine
        void Shutdown();

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
        /**
         * @brief Return a copy of the live update-owned render observation.
         *
         * A failed initialization or a shutdown returns an unavailable value.
         */
        [[nodiscard]] EngineRenderRuntimeDiagnostics
            GetRenderRuntimeDiagnostics() const noexcept;
        [[nodiscard]] const RenderShutdownResult&
            GetLastRenderShutdownResult() const noexcept
        {
            return m_lastRenderShutdownResult;
        }
        [[nodiscard]] const EngineShutdownDiagnostics&
            GetLastShutdownDiagnostics() const noexcept
        {
            return m_lastShutdownDiagnostics;
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
        struct WorldEntry;

        bool InitializeSubsystems();
        void TickSubsystems(float deltaTime);
        void ShutdownSubsystems();
        void TickWorlds(float deltaTime);
        void ShutdownWorlds();
        void RenderAfterWorlds(float deltaTime);
        void FinalizeDestroyRequestedWorlds(float deltaTime);
        [[nodiscard]] bool RollbackFailedInitialization();
        [[nodiscard]] bool BeginWorldShutdown(WorldEntry& entry);
        [[nodiscard]] WorldEntry* FindWorldEntry(World* world) noexcept;
        [[nodiscard]] const WorldEntry* FindWorldEntry(World* world) const noexcept;
        void SelectNextWorldForShutdown();

        struct WorldEntry
        {
            std::unique_ptr<World> world;
            std::unique_ptr<WorldEcsRuntimeComposition> ecsRuntime;
            float64 fixedTimeAccumulatorSeconds = 0.0;
            // The live World that was active when this entry began draining.
            // It is restored only after this entry's removal proof completes.
            World* resumeActiveWorld = nullptr;
            bool destroyRequested = false;
        };

        EngineConfig m_config;
        SubsystemCollection<EngineSubsystem> m_subsystems;

        // World management
        // Declared before World entries so entries drain and destruct while the
        // global proof owner and its Render/Resource references remain alive.
        std::unique_ptr<EcsRenderRuntimeComposition> m_ecsRenderComposition;
        // These Engine-wide animation owners outlive every World composition.
        // Declaration order is deliberate: Worlds destruct first, then the evaluator releases
        // its immutable leases, then the request service releases Resource subscriptions.
        std::shared_ptr<AnimationSceneAdapters::EcsAnimationAssetService>
            m_ecsAnimationAssetService;
        std::shared_ptr<AnimationSceneAdapters::ResourceAnimationEcsEvaluator>
            m_resourceAnimationEvaluator;
        // These Engine-global feature gateway owners deliberately precede World entries. Every
        // World owns only a bridge consumer, so World destruction and its exact drain proof run
        // before the resource-owning Particle gateway or value-only Water/Terrain gateways reset.
        std::unique_ptr<Particle::ParticleEcsRuntimeGateway> m_particleEcsGateway;
        std::unique_ptr<Water::WaterEcsRuntimeGateway> m_waterEcsGateway;
        std::unique_ptr<Terrain::TerrainEcsRuntimeGateway> m_terrainEcsGateway;
        std::map<std::string, WorldEntry> m_worlds;
        World* m_activeWorld = nullptr;
        RenderShutdownResult m_lastRenderShutdownResult{};
        EngineShutdownDiagnostics m_lastShutdownDiagnostics{};

        bool m_initialized = false;
        bool m_initializationRollbackPending = false;
        bool m_shouldShutdown = false;
        uint64_t m_frameNumber = 0;

        static Engine* s_instance;
    };

} // namespace RVX
