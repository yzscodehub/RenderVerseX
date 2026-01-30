#pragma once

/**
 * @file SubEmitterModule.h
 * @brief Sub-emitter module - spawn child particles on events
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/ParticleTypes.h"
#include <string>
#include <vector>
#include <memory>

namespace RVX::Particle
{
    // Forward declaration
    class ParticleSystem;

    /**
     * @brief Sub-emitter trigger event
     */
    enum class SubEmitterTrigger : uint8
    {
        Birth,          ///< When particle is born
        Death,          ///< When particle dies
        Collision,      ///< When particle collides
        Manual          ///< Triggered manually via script
    };

    /**
     * @brief Properties to inherit from parent particle
     */
    enum class SubEmitterInherit : uint8
    {
        Nothing = 0,
        Color = 1 << 0,
        Size = 1 << 1,
        Rotation = 1 << 2,
        Lifetime = 1 << 3,
        Duration = 1 << 4,
        All = Color | Size | Rotation | Lifetime | Duration
    };

    inline SubEmitterInherit operator|(SubEmitterInherit a, SubEmitterInherit b)
    {
        return static_cast<SubEmitterInherit>(
            static_cast<uint8>(a) | static_cast<uint8>(b));
    }

    inline bool HasFlag(SubEmitterInherit flags, SubEmitterInherit flag)
    {
        return (static_cast<uint8>(flags) & static_cast<uint8>(flag)) != 0;
    }

    /**
     * @brief Sub-emitter definition
     */
    struct SubEmitter
    {
        /// Trigger event
        SubEmitterTrigger trigger = SubEmitterTrigger::Death;

        /// Path to sub-emitter particle system
        std::string systemPath;

        /// Cached reference to particle system
        std::shared_ptr<ParticleSystem> system;

        /// Properties to inherit from parent
        SubEmitterInherit inherit = SubEmitterInherit::Color;

        /// Emit probability (0-1)
        float probability = 1.0f;

        /// Number of particles to emit
        uint32 emitCount = 1;
    };

    /**
     * @brief Spawns child particle systems on particle events
     * 
     * Note: Sub-emitters are handled on CPU side and require
     * event handling from the simulation.
     */
    class SubEmitterModule : public IParticleModule
    {
    public:
        /// List of sub-emitters
        std::vector<SubEmitter> subEmitters;

        /// Add a sub-emitter
        void AddSubEmitter(SubEmitterTrigger trigger, 
                          const std::string& systemPath,
                          SubEmitterInherit inherit = SubEmitterInherit::Color)
        {
            SubEmitter sub;
            sub.trigger = trigger;
            sub.systemPath = systemPath;
            sub.inherit = inherit;
            subEmitters.push_back(sub);
        }

        const char* GetTypeName() const override { return "SubEmitterModule"; }
        ModuleStage GetStage() const override { return ModuleStage::Update; }
        
        /// Sub-emitters are managed on CPU side
        bool IsGPUModule() const override { return false; }

        size_t GetGPUDataSize() const override { return 0; }
    };

} // namespace RVX::Particle
