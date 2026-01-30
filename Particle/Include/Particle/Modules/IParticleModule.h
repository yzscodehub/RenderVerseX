#pragma once

/**
 * @file IParticleModule.h
 * @brief Base interface for particle behavior modules
 */

#include "Core/Types.h"
#include <cstddef>

namespace RVX::Particle
{
    /**
     * @brief Module execution stage
     */
    enum class ModuleStage : uint8
    {
        Spawn,      ///< Execute once when particle is spawned
        Update,     ///< Execute every frame during particle lifetime
        Render      ///< Execute during rendering (affects visual only)
    };

    /**
     * @brief Base interface for particle behavior modules
     * 
     * Modules modify particle behavior during simulation.
     * They can affect position, velocity, color, size, etc.
     */
    class IParticleModule
    {
    public:
        virtual ~IParticleModule() = default;

        // =====================================================================
        // Type Information
        // =====================================================================

        /// Get module type name (for serialization/debugging)
        virtual const char* GetTypeName() const = 0;

        /// Check if this module runs on GPU (vs CPU-only)
        virtual bool IsGPUModule() const { return true; }

        /// Get the execution stage
        virtual ModuleStage GetStage() const { return ModuleStage::Update; }

        // =====================================================================
        // GPU Data
        // =====================================================================

        /// Get size of GPU constant data for this module
        virtual size_t GetGPUDataSize() const { return 0; }

        /// Write GPU constant data to buffer
        virtual void GetGPUData(void* outData, size_t maxSize) const 
        { 
            (void)outData; 
            (void)maxSize; 
        }

        // =====================================================================
        // State
        // =====================================================================

        /// Enable/disable this module
        bool enabled = true;
    };

} // namespace RVX::Particle
