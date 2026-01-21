#pragma once

/**
 * @file WorldSubsystem.h
 * @brief Base class for world-level subsystems
 */

#include "Core/Subsystem/ISubsystem.h"

namespace RVX
{
    class World;
    
    // Forward declare template
    template<typename TBase> class SubsystemCollection;

    /**
     * @brief Base class for world-level subsystems
     * 
     * World subsystems are created and destroyed with each world/level.
     * They are useful for per-level state that should be reset between loads.
     * 
     * Examples:
     * - SpatialSubsystem (per-world BVH)
     * - AISubsystem (per-world navigation)
     * - PhysicsSubsystem (per-world simulation)
     * 
     * Usage:
     * @code
     * class SpatialSubsystem : public WorldSubsystem
     * {
     * public:
     *     const char* GetName() const override { return "SpatialSubsystem"; }
     *     
     *     void Initialize() override {
     *         // Build spatial index for this world
     *     }
     *     
     *     void Tick(float deltaTime) override {
     *         // Update spatial index
     *     }
     *     
     *     bool ShouldTick() const override { return true; }
     * };
     * @endcode
     */
    class WorldSubsystem : public ISubsystem
    {
    public:
        SubsystemLifetime GetLifetime() const override 
        { 
            return SubsystemLifetime::World; 
        }

        /// Get the owning world (available after initialization)
        World* GetWorld() const { return m_world; }

    protected:
        friend class World;
        
        template<typename TBase>
        friend class SubsystemCollection;

        void SetWorld(World* world) { m_world = world; }

    private:
        World* m_world = nullptr;
    };

} // namespace RVX
