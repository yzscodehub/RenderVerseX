# Particle Module CMakeLists.txt
# CPU particle runtime contracts. Renderer shader programs are owned and cooked
# by the Render module; this module must not stage raw shader source.

cmake_minimum_required(VERSION 3.21)

# ==============================================================================
# Module Definition
# ==============================================================================

set(MODULE_NAME Particle)

# ==============================================================================
# Source Files
# ==============================================================================

set(PARTICLE_HEADERS
    # Main
    Include/Particle/Particle.h
    Include/Particle/ParticleTypes.h
    Include/Particle/ParticleSystem.h
    Include/Particle/ParticleSystemInstance.h
    Include/Particle/ParticleSubsystem.h
    Include/Particle/ParticleLOD.h
    Include/Particle/ParticlePool.h
    Include/Particle/ParticleRenderStats.h
    Include/Particle/ParticleSystemLoader.h
    Include/Particle/ECS/ParticleEcsBridge.h
    Include/Particle/ECS/ParticleEcsRuntimeGateway.h
    
    # Curves
    Include/Particle/Curves/AnimationCurve.h
    Include/Particle/Curves/GradientCurve.h
    Include/Particle/Curves/CurveSerialization.h
    
    # Emitters
    Include/Particle/Emitters/IEmitter.h
    Include/Particle/Emitters/PointEmitter.h
    Include/Particle/Emitters/BoxEmitter.h
    Include/Particle/Emitters/SphereEmitter.h
    Include/Particle/Emitters/ConeEmitter.h
    Include/Particle/Emitters/CircleEmitter.h
    Include/Particle/Emitters/EdgeEmitter.h
    Include/Particle/Emitters/MeshEmitter.h
    
    # Modules
    Include/Particle/Modules/IParticleModule.h
    Include/Particle/Modules/ForceModule.h
    Include/Particle/Modules/ColorOverLifetimeModule.h
    Include/Particle/Modules/SizeOverLifetimeModule.h
    Include/Particle/Modules/VelocityOverLifetimeModule.h
    Include/Particle/Modules/RotationOverLifetimeModule.h
    Include/Particle/Modules/NoiseModule.h
    Include/Particle/Modules/CollisionModule.h
    Include/Particle/Modules/TextureSheetModule.h
    Include/Particle/Modules/TrailModule.h
    Include/Particle/Modules/LightsModule.h
    Include/Particle/Modules/SubEmitterModule.h
    
    # Events
    Include/Particle/Events/ParticleEvent.h
    Include/Particle/Events/ParticleEventHandler.h
    
    # Rendering
    Include/Particle/Rendering/SoftParticleConfig.h
)

set(PARTICLE_PRIVATE_HEADERS
    # GPU
    Private/Particle/GPU/IParticleSimulator.h
    Private/Particle/GPU/CPUParticleSimulator.h
    Private/Particle/GPU/ParticleSorter.h

    # Rendering
    Private/Particle/Rendering/TrailRenderer.h
)

set(PARTICLE_SOURCES
    Private/ParticleSystem.cpp
    Private/ParticleSystemInstance.cpp
    Private/ParticleSubsystem.cpp
    Private/ParticlePool.cpp
    Private/ParticleSystemLoader.cpp
    Private/ECS/ParticleEcsBridge.cpp
    Private/ECS/ParticleEcsRuntimeGateway.cpp

    # GPU
    Private/GPU/CPUParticleSimulator.cpp
    Private/GPU/ParticleSorter.cpp

    # Curves
    Private/Curves/CurveUtils.cpp
    Private/Curves/CurveSerialization.cpp
    
    # Events
    Private/Events/ParticleEventSystem.cpp
    
    # Modules
    Private/Modules/TrailModuleProcessor.cpp
    Private/Modules/LightsModuleProcessor.cpp
    Private/Modules/SubEmitterModuleProcessor.cpp
    
    # Rendering
    Private/Rendering/TrailRenderer.cpp
)

# ==============================================================================
# Library Definition
# ==============================================================================

add_library(${MODULE_NAME} STATIC
    ${PARTICLE_HEADERS}
    ${PARTICLE_PRIVATE_HEADERS}
    ${PARTICLE_SOURCES}
)
add_library(RVX::Particle ALIAS ${MODULE_NAME})

# ==============================================================================
# Include Directories
# ==============================================================================

target_include_directories(${MODULE_NAME}
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

# ==============================================================================
# Dependencies
# ==============================================================================

target_link_libraries(${MODULE_NAME}
    PUBLIC
        RVX_Core
        RVX::RenderContracts
        RVX_Scene
        RVX::ResourceRuntime
)

# ==============================================================================
# Compile Definitions
# ==============================================================================

target_compile_definitions(${MODULE_NAME}
    PRIVATE
        RVX_PARTICLE_EXPORTS
)

# ==============================================================================
# IDE Organization
# ==============================================================================

source_group("Include\\Particle" FILES
    Include/Particle/Particle.h
    Include/Particle/ParticleTypes.h
    Include/Particle/ParticleSystem.h
    Include/Particle/ParticleSystemInstance.h
    Include/Particle/ParticleSubsystem.h
    Include/Particle/ParticleLOD.h
    Include/Particle/ParticlePool.h
    Include/Particle/ParticleRenderStats.h
    Include/Particle/ParticleSystemLoader.h
)

source_group("Include\\Particle\\Curves" FILES
    Include/Particle/Curves/AnimationCurve.h
    Include/Particle/Curves/GradientCurve.h
)

source_group("Include\\Particle\\Emitters" FILES
    ${PARTICLE_HEADERS}
)

source_group("Include\\Particle\\Modules" FILES
    ${PARTICLE_HEADERS}
)

source_group("Include\\Particle\\Events" FILES
    Include/Particle/Events/ParticleEvent.h
    Include/Particle/Events/ParticleEventHandler.h
)

source_group("Include\\Particle\\Rendering" FILES
    Include/Particle/Rendering/SoftParticleConfig.h
)

source_group("Private" FILES 
    Private/ParticleSystem.cpp
    Private/ParticleSystemInstance.cpp
    Private/ParticleSubsystem.cpp
    Private/ParticlePool.cpp
    Private/ParticleSystemLoader.cpp
)

source_group("Private\\GPU" FILES
    Private/Particle/GPU/IParticleSimulator.h
    Private/Particle/GPU/CPUParticleSimulator.h
    Private/Particle/GPU/ParticleSorter.h
    Private/GPU/CPUParticleSimulator.cpp
    Private/GPU/ParticleSorter.cpp
)

source_group("Private\\Curves" FILES
    Private/Curves/CurveUtils.cpp
    Private/Curves/CurveSerialization.cpp
)

source_group("Private\\Events" FILES
    Private/Events/ParticleEventSystem.cpp
)

source_group("Private\\Modules" FILES
    Private/Modules/TrailModuleProcessor.cpp
    Private/Modules/LightsModuleProcessor.cpp
    Private/Modules/SubEmitterModuleProcessor.cpp
)

source_group("Private\\Rendering" FILES
    Private/Particle/Rendering/TrailRenderer.h
    Private/Rendering/TrailRenderer.cpp
)

# ==============================================================================
# Precompiled Headers (optional)
# ==============================================================================

if(RVX_USE_PCH)
    target_precompile_headers(${MODULE_NAME} PRIVATE
        <vector>
        <memory>
        <string>
        <unordered_map>
        <functional>
        <cmath>
    )
endif()

message(STATUS "Configured module: ${MODULE_NAME}")

rvx_register_module_target(
    TARGET Particle
    MODULE Particle
    ALIASES RVX::Particle
)
