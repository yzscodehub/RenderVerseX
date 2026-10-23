#pragma once

/**
 * @file ISubsystem.h
 * @brief Base interface for all subsystems
 * 
 * Enhanced with:
 * - Type-safe dependency declaration
 * - Initialization state tracking
 * - Optional tick phases
 */

#include <vector>
#include <typeindex>

namespace RVX
{
    /**
     * @brief Subsystem lifetime scope
     */
    enum class SubsystemLifetime
    {
        Engine,      ///< Lives from Engine::Init to Engine::Shutdown
        World,       ///< Lives from World::Load to World::Unload
        LocalPlayer  ///< Lives per local player (for split-screen)
    };

    /**
     * @brief Tick phase for subsystem updates
     */
    enum class TickPhase
    {
        PreUpdate,   ///< Before main update (input, physics prep)
        Update,      ///< Main update
        PostUpdate,  ///< After main update (finalization)
        PreRender,   ///< Before rendering
        PostRender   ///< After rendering
    };

    /**
     * @brief Dependency declaration for subsystems
     */
    struct SubsystemDependency
    {
        std::type_index typeIndex;
        const char* name;
        bool optional;  ///< If true, missing dependency is not an error

        SubsystemDependency(std::type_index idx, const char* n, bool opt = false)
            : typeIndex(idx), name(n), optional(opt) {}
    };

    /**
     * @brief Base interface for all subsystems
     * 
     * Subsystems are singleton-like objects with managed lifetimes.
     * They provide a cleaner alternative to global singletons by:
     * - Having clear initialization/shutdown lifecycle
     * - Being scoped to Engine, World, or LocalPlayer
     * - Supporting dependency injection through the parent container
     * - Type-safe dependency declarations
     * 
     * Usage:
     * @code
     * class MySubsystem : public EngineSubsystem
     * {
     * public:
     *     // Type-safe dependencies
     *     static constexpr auto Dependencies = DependsOn<RenderSubsystem, ResourceSubsystem>();
     *     
     *     const char* GetName() const override { return "MySubsystem"; }
     *     
     *     void Initialize() override {
     *         // Setup - dependencies are guaranteed to be initialized
     *         auto* render = GetEngine()->GetSubsystem<RenderSubsystem>();
     *     }
     * };
     * @endcode
     */
    class ISubsystem
    {
    public:
        virtual ~ISubsystem() = default;

        /// Get subsystem name for debugging/logging
        virtual const char* GetName() const = 0;

        /// Get the lifetime scope of this subsystem
        virtual SubsystemLifetime GetLifetime() const = 0;

        /// Called when the subsystem is added to the collection
        virtual void Initialize() {}

        /// Called when the subsystem is about to be removed
        virtual void Deinitialize() {}

        /// Called every frame if ShouldTick() returns true
        virtual void Tick(float deltaTime) { (void)deltaTime; }

        /// Whether this subsystem needs per-frame updates
        virtual bool ShouldTick() const { return false; }

        /// Get the tick phase for this subsystem
        virtual TickPhase GetTickPhase() const { return TickPhase::Update; }

        /// Get dependencies (subsystem names that must be initialized first)
        /// @deprecated Use RVX_SUBSYSTEM_DEPENDENCIES macro with GetTypedDependencies() instead
        [[deprecated("Use RVX_SUBSYSTEM_DEPENDENCIES macro instead")]]
        virtual const char** GetDependencies(int& outCount) const 
        { 
            outCount = 0; 
            return nullptr; 
        }

        /// Get type-safe dependencies (preferred)
        virtual std::vector<SubsystemDependency> GetTypedDependencies() const
        {
            return {};
        }

        /// Check if this subsystem is initialized
        bool IsInitialized() const { return m_initialized; }

    protected:
        friend class Engine;
        friend class World;
        template<typename TBase> friend class SubsystemCollection;

        void SetInitialized(bool init) { m_initialized = init; }

    private:
        bool m_initialized = false;
    };

    // =========================================================================
    // Type-safe Dependency Declaration Helpers
    // =========================================================================

    /**
     * @brief Helper to declare type-safe dependencies
     * 
     * Usage:
     * @code
     * class MySubsystem : public EngineSubsystem
     * {
     * public:
     *     std::vector<SubsystemDependency> GetTypedDependencies() const override
     *     {
     *         return MakeDependencies<RenderSubsystem, ResourceSubsystem>();
     *     }
     * };
     * @endcode
     */
    template<typename... Deps>
    std::vector<SubsystemDependency> MakeDependencies()
    {
        return { SubsystemDependency(std::type_index(typeid(Deps)), typeid(Deps).name())... };
    }

    /**
     * @brief Helper to declare optional dependencies
     */
    template<typename T>
    SubsystemDependency OptionalDependency()
    {
        return SubsystemDependency(std::type_index(typeid(T)), typeid(T).name(), true);
    }

    /**
     * @brief Macro for declaring dependencies in class body
     */
    #define RVX_SUBSYSTEM_DEPENDENCIES(...) \
        std::vector<SubsystemDependency> GetTypedDependencies() const override \
        { \
            return MakeDependencies<__VA_ARGS__>(); \
        }

} // namespace RVX
