#pragma once

/**
 * @file Particle.h
 * @brief Main header for the Particle module
 * 
 * Include this header to access all particle system functionality.
 */

// Core types
#include "Particle/ParticleTypes.h"
#include "Particle/ParticleSystem.h"
#include "Particle/ParticleSystemInstance.h"
#include "Particle/ParticleSubsystem.h"
#include "Particle/ParticleComponent.h"
#include "Particle/ParticleLOD.h"
#include "Particle/ParticlePool.h"

// Curves
#include "Particle/Curves/AnimationCurve.h"
#include "Particle/Curves/GradientCurve.h"

// Emitters
#include "Particle/Emitters/IEmitter.h"
#include "Particle/Emitters/PointEmitter.h"
#include "Particle/Emitters/BoxEmitter.h"
#include "Particle/Emitters/SphereEmitter.h"
#include "Particle/Emitters/ConeEmitter.h"
#include "Particle/Emitters/CircleEmitter.h"
#include "Particle/Emitters/EdgeEmitter.h"
#include "Particle/Emitters/MeshEmitter.h"

// Modules
#include "Particle/Modules/IParticleModule.h"
#include "Particle/Modules/ForceModule.h"
#include "Particle/Modules/ColorOverLifetimeModule.h"
#include "Particle/Modules/SizeOverLifetimeModule.h"
#include "Particle/Modules/VelocityOverLifetimeModule.h"
#include "Particle/Modules/RotationOverLifetimeModule.h"
#include "Particle/Modules/NoiseModule.h"
#include "Particle/Modules/CollisionModule.h"
#include "Particle/Modules/TextureSheetModule.h"
#include "Particle/Modules/TrailModule.h"
#include "Particle/Modules/LightsModule.h"
#include "Particle/Modules/SubEmitterModule.h"

// Events
#include "Particle/Events/ParticleEvent.h"
#include "Particle/Events/ParticleEventHandler.h"

// GPU
#include "Particle/GPU/IParticleSimulator.h"
#include "Particle/GPU/GPUParticleSimulator.h"
#include "Particle/GPU/CPUParticleSimulator.h"
#include "Particle/GPU/ParticleSorter.h"

// Rendering
#include "Particle/Rendering/SoftParticleConfig.h"
#include "Particle/Rendering/ParticleRenderer.h"
#include "Particle/Rendering/TrailRenderer.h"
#include "Particle/Rendering/ParticlePass.h"

// Resource
#include "Particle/ParticleSystemLoader.h"

namespace RVX::Particle
{
    /**
     * @brief Initialize the particle module
     * 
     * Call this during engine initialization to register
     * serialization types and resource loaders.
     */
    inline void InitializeModule()
    {
        RegisterParticleTypes();
    }

} // namespace RVX::Particle
