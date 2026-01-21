# =============================================================================
# Geometry Module - Geometric primitives, queries, and algorithms
# =============================================================================
# Provides:
# - Primitives: OBB, Capsule, Triangle, Cylinder, Cone, Line/Segment
# - Query functions: Intersection tests, distance calculations
# - (Future) SIMD batch processing, collision detection, mesh operations
#
# Dependencies: Core (MathTypes, existing geometry)
# =============================================================================

add_library(RVX_Geometry STATIC)

# Source files
target_sources(RVX_Geometry PRIVATE
    # Placeholder source file (headers are mostly inline)
    Private/Geometry.cpp
)

target_include_directories(RVX_Geometry PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Geometry PUBLIC
    RVX::Core
)

target_compile_features(RVX_Geometry PUBLIC cxx_std_20)

# SIMD compile options (for future batch processing)
if(MSVC)
    # Enable AVX2 on MSVC (optional, can be made configurable)
    # target_compile_options(RVX_Geometry PRIVATE /arch:AVX2)
else()
    # GCC/Clang options
    # Check for ARM NEON vs x86 SSE/AVX
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        # ARM - NEON is typically enabled by default
        message(STATUS "Geometry: ARM architecture detected, NEON will be used if available")
    else()
        # x86 - Can enable SSE4.1/AVX2 as needed
        # target_compile_options(RVX_Geometry PRIVATE -msse4.1)
        message(STATUS "Geometry: x86 architecture detected")
    endif()
endif()

# Create alias for cleaner target names
add_library(RVX::Geometry ALIAS RVX_Geometry)
