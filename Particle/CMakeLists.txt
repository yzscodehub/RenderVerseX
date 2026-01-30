# Particle Module CMakeLists.txt
# GPU-driven particle system with CPU fallback

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
    Include/Particle/ParticleComponent.h
    Include/Particle/ParticleLOD.h
    Include/Particle/ParticlePool.h
    Include/Particle/ParticleSystemLoader.h
    
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
    
    # GPU
    Include/Particle/GPU/IParticleSimulator.h
    Include/Particle/GPU/GPUParticleSimulator.h
    Include/Particle/GPU/CPUParticleSimulator.h
    Include/Particle/GPU/ParticleSorter.h
    
    # Rendering
    Include/Particle/Rendering/SoftParticleConfig.h
    Include/Particle/Rendering/ParticleRenderer.h
    Include/Particle/Rendering/TrailRenderer.h
    Include/Particle/Rendering/ParticlePass.h
)

set(PARTICLE_SOURCES
    Private/ParticleSystem.cpp
    Private/ParticleSystemInstance.cpp
    Private/ParticleSubsystem.cpp
    Private/ParticleComponent.cpp
    Private/ParticlePool.cpp
    Private/ParticleSystemLoader.cpp
    
    # GPU
    Private/GPU/GPUParticleSimulator.cpp
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
    Private/Rendering/ParticleRenderer.cpp
    Private/Rendering/TrailRenderer.cpp
    Private/Rendering/ParticlePass.cpp
)

set(PARTICLE_SHADERS
    Shaders/Include/ParticleCommon.hlsli
    Shaders/Include/SoftParticle.hlsli
    Shaders/Include/TextureSheet.hlsli
    Shaders/Include/NoiseUtils.hlsli
    Shaders/ParticleEmit.hlsl
    Shaders/ParticleSimulate.hlsl
    Shaders/ParticleBillboard.hlsl
    Shaders/ParticleStretchedBillboard.hlsl
    Shaders/ParticleBitonicSort.hlsl
)

# ==============================================================================
# Library Definition
# ==============================================================================

add_library(${MODULE_NAME} STATIC
    ${PARTICLE_HEADERS}
    ${PARTICLE_SOURCES}
)

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
        RVX_RHI
        RVX_Render
        RVX_Scene
        RVX_Resource
)

# ==============================================================================
# Compile Definitions
# ==============================================================================

target_compile_definitions(${MODULE_NAME}
    PRIVATE
        RVX_PARTICLE_EXPORTS
)

# ==============================================================================
# Shader Installation
# ==============================================================================

# Copy shaders to output directory
set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Shaders/Particle)

add_custom_command(TARGET ${MODULE_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}/Include
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/ParticleCommon.hlsli
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/SoftParticle.hlsli
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/TextureSheet.hlsli
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Include/NoiseUtils.hlsli
        ${SHADER_OUTPUT_DIR}/Include/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleEmit.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleSimulate.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleBillboard.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleStretchedBillboard.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/ParticleBitonicSort.hlsl
        ${SHADER_OUTPUT_DIR}/
    COMMENT "Copying Particle shaders to output directory"
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
    Include/Particle/ParticleComponent.h
    Include/Particle/ParticleLOD.h
    Include/Particle/ParticlePool.h
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

source_group("Include\\Particle\\GPU" FILES
    Include/Particle/GPU/IParticleSimulator.h
    Include/Particle/GPU/GPUParticleSimulator.h
    Include/Particle/GPU/CPUParticleSimulator.h
    Include/Particle/GPU/ParticleSorter.h
)

source_group("Include\\Particle\\Rendering" FILES
    Include/Particle/Rendering/SoftParticleConfig.h
    Include/Particle/Rendering/ParticleRenderer.h
    Include/Particle/Rendering/TrailRenderer.h
    Include/Particle/Rendering/ParticlePass.h
)

source_group("Private" FILES 
    Private/ParticleSystem.cpp
    Private/ParticleSystemInstance.cpp
    Private/ParticleSubsystem.cpp
    Private/ParticleComponent.cpp
    Private/ParticlePool.cpp
    Private/ParticleSystemLoader.cpp
)

source_group("Private\\GPU" FILES 
    Private/GPU/GPUParticleSimulator.cpp
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
    Private/Rendering/ParticleRenderer.cpp
    Private/Rendering/TrailRenderer.cpp
    Private/Rendering/ParticlePass.cpp
)

source_group("Shaders" FILES ${PARTICLE_SHADERS})

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
