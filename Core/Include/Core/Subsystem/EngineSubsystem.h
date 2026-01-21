#pragma once

/**
 * @file EngineSubsystem.h
 * @brief Base class for engine-level subsystems
 */

#include "Core/Subsystem/ISubsystem.h"

namespace RVX
{
    class Engine;
    
    // Forward declare template
    template<typename TBase> class SubsystemCollection;

    /**
     * @brief Base class for engine-level subsystems
     * 
     * Engine subsystems live for the entire duration of the engine,
     * from Engine::Initialize() to Engine::Shutdown().
     * 
     * Examples:
     * - RenderSubsystem
     * - ResourceSubsystem
     * - InputSubsystem
     * - TimeSubsystem
     * 
     * Usage:
     * @code
     * class RenderSubsystem : public EngineSubsystem
     * {
     * public:
     *     const char* GetName() const override { return "RenderSubsystem"; }
     *     
     *     void Initialize() override {
     *         // Create RHI device, swap chain, etc.
     *     }
     * };
     * @endcode
     */
    class EngineSubsystem : public ISubsystem
    {
    public:
        SubsystemLifetime GetLifetime() const override 
        { 
            return SubsystemLifetime::Engine; 
        }

        /// Get the owning engine (available after initialization)
        Engine* GetEngine() const { return m_engine; }

    protected:
        friend class Engine;
        
        template<typename TBase>
        friend class SubsystemCollection;

        void SetEngine(Engine* engine) { m_engine = engine; }

    private:
        Engine* m_engine = nullptr;
    };

} // namespace RVX
