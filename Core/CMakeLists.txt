# =============================================================================
# Core Module - Foundation types and utilities
# =============================================================================
add_library(RVX_Core STATIC)

target_sources(RVX_Core PRIVATE
    # Core
    Private/Memory.cpp
    Private/Log.cpp
    Private/PathUtils.cpp
    Private/SystemManager.cpp

    # Content identity
    Private/Hash/SHA256.cpp

    # Diagnostics
    Private/Diagnostics/ArtifactMetadata.cpp
    Private/Diagnostics/ContentHash.cpp
    Private/Diagnostics/JsonWriter.cpp
    Private/Diagnostics/PortablePath.cpp
    Private/Diagnostics/Trace.cpp

    # Camera contracts
    Private/Camera/Camera.cpp
    
    # Event System
    Private/Event/EventBus.cpp
    
    # Job System
    Private/Job/ThreadPool.cpp
    Private/Job/JobSystem.cpp
    Private/Job/JobGraph.cpp
    
    # Memory Allocators
    Private/Memory/Allocators.cpp
    
    # Serialization
    Private/Serialization/Serialization.cpp
    Private/Serialization/PropertyReflection.cpp
)

target_include_directories(RVX_Core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Core PUBLIC
    RVX::Options
    spdlog::spdlog
    glm::glm
)

# Alias for cleaner target names
add_library(RVX::Core ALIAS RVX_Core)
