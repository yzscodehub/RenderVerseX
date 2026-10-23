# =============================================================================
# DebugTools Module - Profiling, debugging, and development tools
# =============================================================================
add_library(RVX_DebugTools STATIC)

target_sources(RVX_DebugTools PRIVATE
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

target_include_directories(RVX_DebugTools PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_DebugTools PUBLIC
    RVX::Core
)

# Debug-only features
target_compile_definitions(RVX_DebugTools PRIVATE
    $<$<CONFIG:Debug>:RVX_ENABLE_PROFILING=1>
)

# Alias for cleaner target names
add_library(RVX::DebugTools ALIAS RVX_DebugTools)

rvx_register_module_target(
    TARGET RVX_DebugTools
    MODULE DebugTools
    ALIASES RVX::DebugTools
)
