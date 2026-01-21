#pragma once

/**
 * @file ISubsystem.h
 * @brief Base interface for all subsystems
 */

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
     * @brief Base interface for all subsystems
     * 
     * Subsystems are singleton-like objects with managed lifetimes.
     * They provide a cleaner alternative to global singletons by:
     * - Having clear initialization/shutdown lifecycle
     * - Being scoped to Engine, World, or LocalPlayer
     * - Supporting dependency injection through the parent container
     * 
     * Usage:
     * @code
     * class MySubsystem : public EngineSubsystem
     * {
     * public:
     *     const char* GetName() const override { return "MySubsystem"; }
     *     
     *     void Initialize() override {
     *         // Setup
     *     }
     *     
     *     void Deinitialize() override {
     *         // Cleanup
     *     }
     *     
     *     void Tick(float deltaTime) override {
     *         // Called each frame if ShouldTick() returns true
     *     }
     *     
     *     bool ShouldTick() const override { return true; }
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

        /// Get dependencies (subsystem names that must be initialized first)
        virtual const char** GetDependencies(int& outCount) const 
        { 
            outCount = 0; 
            return nullptr; 
        }
    };

} // namespace RVX
