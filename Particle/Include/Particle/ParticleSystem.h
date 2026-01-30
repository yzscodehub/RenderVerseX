#pragma once

/**
 * @file ParticleSystem.h
 * @brief Particle system asset - defines particle behavior and appearance
 */

#include "Particle/ParticleTypes.h"
#include "Particle/Emitters/IEmitter.h"
#include "Particle/Modules/IParticleModule.h"
#include "Particle/Rendering/SoftParticleConfig.h"
#include "Particle/ParticleLOD.h"
#include <memory>
#include <vector>
#include <string>

namespace RVX::Particle
{
    /**
     * @brief Particle system asset definition
     * 
     * ParticleSystem defines the behavior and appearance of particles.
     * It is an asset that can be serialized and shared across instances.
     */
    class ParticleSystem
    {
    public:
        using Ptr = std::shared_ptr<ParticleSystem>;
        using ConstPtr = std::shared_ptr<const ParticleSystem>;

        // =====================================================================
        // Identity
        // =====================================================================

        /// System name
        std::string name = "ParticleSystem";

        /// Unique ID (for serialization)
        uint64 id = 0;

        // =====================================================================
        // Basic Settings
        // =====================================================================

        /// Maximum number of particles
        uint32 maxParticles = RVX_DEFAULT_MAX_PARTICLES;

        /// System duration in seconds (for non-looping systems)
        float duration = 5.0f;

        /// Whether the system loops
        bool looping = true;

        /// Pre-warm the system (simulate before first frame)
        bool prewarm = false;

        /// Pre-warm duration (seconds)
        float prewarmTime = 1.0f;

        /// Start delay (seconds before first emission)
        float startDelay = 0.0f;

        /// Playback speed multiplier
        float simulationSpeed = 1.0f;

        /// Scaling mode
        enum class ScalingMode : uint8 { Hierarchy, Local, Shape } scalingMode = ScalingMode::Hierarchy;

        // =====================================================================
        // Space Settings
        // =====================================================================

        /// Simulation space
        ParticleSpace simulationSpace = ParticleSpace::World;

        // =====================================================================
        // Emitters
        // =====================================================================

        /// Particle emitters
        std::vector<std::unique_ptr<IEmitter>> emitters;

        /// Add an emitter
        template<typename T, typename... Args>
        T* AddEmitter(Args&&... args)
        {
            auto emitter = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = emitter.get();
            emitters.push_back(std::move(emitter));
            return ptr;
        }

        /// Get emitter by index
        IEmitter* GetEmitter(size_t index) { return emitters[index].get(); }
        const IEmitter* GetEmitter(size_t index) const { return emitters[index].get(); }

        // =====================================================================
        // Modules
        // =====================================================================

        /// Behavior modules (executed in order)
        std::vector<std::unique_ptr<IParticleModule>> modules;

        /// Add a module
        template<typename T, typename... Args>
        T* AddModule(Args&&... args)
        {
            auto module = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = module.get();
            modules.push_back(std::move(module));
            return ptr;
        }

        /// Get module by type (returns first matching)
        template<typename T>
        T* GetModule()
        {
            for (auto& module : modules)
            {
                if (auto* m = dynamic_cast<T*>(module.get()))
                    return m;
            }
            return nullptr;
        }

        template<typename T>
        const T* GetModule() const
        {
            for (const auto& module : modules)
            {
                if (const auto* m = dynamic_cast<const T*>(module.get()))
                    return m;
            }
            return nullptr;
        }

        // =====================================================================
        // Rendering Settings
        // =====================================================================

        /// Render mode
        ParticleRenderMode renderMode = ParticleRenderMode::Billboard;

        /// Blend mode
        ParticleBlendMode blendMode = ParticleBlendMode::AlphaBlend;

        /// Sort mode
        ParticleSortMode sortMode = ParticleSortMode::ByDistance;

        /// Material path
        std::string materialPath;

        /// Texture path (if no material)
        std::string texturePath;

        /// Soft particle configuration
        SoftParticleConfig softParticleConfig;

        /// Stretched billboard settings
        struct StretchedBillboardSettings
        {
            float cameraVelocityScale = 0.0f;
            float speedScale = 0.0f;
            float lengthScale = 1.0f;
        } stretchedBillboard;

        /// Mesh particle settings
        struct MeshParticleSettings
        {
            std::string meshPath;
        } meshParticle;

        // =====================================================================
        // Culling & LOD
        // =====================================================================

        /// LOD configuration
        ParticleLODConfig lodConfig;

        /// Culling mode
        enum class CullingMode : uint8 
        { 
            Automatic,      ///< Cull based on bounds
            AlwaysSimulate, ///< Always simulate even when not visible
            PauseAndCatchUp ///< Pause when culled, catch up when visible
        } cullingMode = CullingMode::Automatic;

        /// Bounding box mode
        enum class BoundsMode : uint8
        {
            Automatic,      ///< Calculate from particles
            Custom          ///< Use custom bounds
        } boundsMode = BoundsMode::Automatic;

        /// Custom bounds (if boundsMode is Custom)
        Vec3 customBoundsCenter{0.0f, 0.0f, 0.0f};
        Vec3 customBoundsSize{10.0f, 10.0f, 10.0f};

        // =====================================================================
        // Factory Methods
        // =====================================================================

        /// Create an empty particle system
        static Ptr Create(const std::string& name = "ParticleSystem")
        {
            auto system = std::make_shared<ParticleSystem>();
            system->name = name;
            return system;
        }

        /// Create a simple particle system with default emitter
        static Ptr CreateSimple(const std::string& name = "SimpleParticleSystem");
    };

} // namespace RVX::Particle
