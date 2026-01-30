# =============================================================================
# Debug Module - Profiling, debugging, and development tools
# =============================================================================
add_library(RVX_Debug STATIC)

target_sources(RVX_Debug PRIVATE
    # Core profiling
    Private/CPUProfiler.cpp
    Private/MemoryTracker.cpp
    
    # Console and CVars
    Private/Console.cpp
    Private/CVarSystem.cpp
    
    # Statistics display
    Private/StatsHUD.cpp
    
    # Engine integration
    Private/DebugSubsystem.cpp
)

target_include_directories(RVX_Debug PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Debug PUBLIC
    RVX::Core
)

# Debug-only features
target_compile_definitions(RVX_Debug PRIVATE
    $<$<CONFIG:Debug>:RVX_DEBUG=1>
    $<$<CONFIG:Debug>:RVX_ENABLE_PROFILING=1>
)

# Alias for cleaner target names
add_library(RVX::Debug ALIAS RVX_Debug)
