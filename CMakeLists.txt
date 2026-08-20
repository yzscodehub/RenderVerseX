cmake_minimum_required(VERSION 3.25)
project(RenderVerseX VERSION 0.1.0 LANGUAGES CXX)

include(${CMAKE_SOURCE_DIR}/cmake/RVXModuleInventory.cmake)
set(RVX_MODULE_MANIFEST
    "${CMAKE_SOURCE_DIR}/Docs/module-boundaries.json")
set(RVX_MODULE_INVENTORY
    "${CMAKE_BINARY_DIR}/Architecture/RVXModuleInventory-$<CONFIG>.json")

# =============================================================================
# C++ Standard
# =============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(RVX_Options INTERFACE)
add_library(RVX::Options ALIAS RVX_Options)
rvx_register_module_target(
    TARGET RVX_Options
    MODULE Core
    ALIASES RVX::Options
    INFRASTRUCTURE
)
target_compile_features(RVX_Options INTERFACE cxx_std_20)
target_compile_definitions(RVX_Options INTERFACE
    $<$<CONFIG:Debug>:RVX_DEBUG=1>
)

# =============================================================================
# Build Options
# =============================================================================
option(RVX_ENABLE_DX11 "Enable DirectX 11 backend" ON)
option(RVX_ENABLE_DX12 "Enable DirectX 12 backend" ON)
option(RVX_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RVX_ENABLE_METAL "Enable Metal backend (macOS/iOS)" OFF)
option(RVX_ENABLE_OPENGL "Enable OpenGL backend" OFF)
option(RVX_BUILD_DEVELOPER "Build developer-only libraries and composition" ON)
option(RVX_BUILD_PROGRAMS "Build developer command-line programs" ON)
option(RVX_BUILD_SHIPPING_PRODUCTS "Build shipping composition and consumer roots" OFF)
option(RVX_BUILD_SHIPPING_TESTS
    "Build the standalone cooked-only shipping validation lane"
    OFF)
set(RVX_SHIPPING_HOST_COOK_EXECUTABLE "" CACHE FILEPATH
    "RVXCook executable produced by the separate host-tools build")
option(RVX_BUILD_EDITOR "Build the Editor application and Editor-only validation" OFF)
option(RVX_BUILD_SAMPLES "Build sample applications" ON)
option(RVX_BUILD_TESTS "Build validation tests" ON)
option(RVX_ENABLE_TSAN "Enable ThreadSanitizer instrumentation" OFF)
option(RVX_ENABLE_GPU_VALIDATION_TESTS
    "Register bounded GPU-based validation tests (dedicated/nightly only)"
    OFF)

if(RVX_BUILD_PROGRAMS AND NOT RVX_BUILD_DEVELOPER)
    message(FATAL_ERROR
        "RVX_BUILD_PROGRAMS requires RVX_BUILD_DEVELOPER=ON")
endif()
if(RVX_BUILD_SAMPLES AND NOT RVX_BUILD_DEVELOPER)
    message(FATAL_ERROR
        "RVX_BUILD_SAMPLES requires RVX_BUILD_DEVELOPER=ON")
endif()
if(RVX_BUILD_TESTS AND NOT RVX_BUILD_DEVELOPER)
    message(FATAL_ERROR
        "RVX_BUILD_TESTS requires RVX_BUILD_DEVELOPER=ON")
endif()
if(RVX_BUILD_SHIPPING_TESTS AND NOT RVX_BUILD_SHIPPING_PRODUCTS)
    message(FATAL_ERROR
        "RVX_BUILD_SHIPPING_TESTS requires RVX_BUILD_SHIPPING_PRODUCTS=ON")
endif()
if(RVX_BUILD_SHIPPING_TESTS AND RVX_BUILD_TESTS)
    message(FATAL_ERROR
        "RVX_BUILD_SHIPPING_TESTS is a standalone composition and requires RVX_BUILD_TESTS=OFF")
endif()
if(RVX_BUILD_SHIPPING_TESTS)
    if(NOT IS_ABSOLUTE "${RVX_SHIPPING_HOST_COOK_EXECUTABLE}")
        message(FATAL_ERROR
            "RVX_SHIPPING_HOST_COOK_EXECUTABLE must be an absolute path from the host-tools build")
    endif()
    if(NOT EXISTS "${RVX_SHIPPING_HOST_COOK_EXECUTABLE}")
        message(FATAL_ERROR
            "RVX_SHIPPING_HOST_COOK_EXECUTABLE does not exist: ${RVX_SHIPPING_HOST_COOK_EXECUTABLE}")
    endif()
endif()

if(RVX_BUILD_EDITOR)
    message(FATAL_ERROR
        "RVX_BUILD_EDITOR is unavailable during the pure Runtime ECS migration; configure with RVX_BUILD_EDITOR=OFF")
endif()

# =============================================================================
# Platform Detection
# =============================================================================
if(WIN32)
    set(RVX_PLATFORM_WINDOWS ON)
    add_compile_definitions(RVX_PLATFORM_WINDOWS=1)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
elseif(APPLE)
    set(RVX_PLATFORM_APPLE ON)
    add_compile_definitions(RVX_PLATFORM_APPLE=1)
    
    if(IOS)
        set(RVX_PLATFORM_IOS ON)
        add_compile_definitions(RVX_PLATFORM_IOS=1)
    else()
        set(RVX_PLATFORM_MACOS ON)
        add_compile_definitions(RVX_PLATFORM_MACOS=1)
    endif()
    
    # Apple platform: disable DirectX backends, enable Metal
    set(RVX_ENABLE_DX11 OFF)
    set(RVX_ENABLE_DX12 OFF)
    set(RVX_ENABLE_METAL ON)
    
    # Enable Objective-C++ for Metal implementation
    enable_language(OBJCXX)
elseif(UNIX)
    set(RVX_PLATFORM_LINUX ON)
    add_compile_definitions(RVX_PLATFORM_LINUX=1)
    # DirectX backends are unavailable on Linux. Vulkan/OpenGL remain explicit
    # component choices so the shipping preset can be Vulkan-only.
    set(RVX_ENABLE_DX11 OFF)
    set(RVX_ENABLE_DX12 OFF)
endif()

# =============================================================================
# Find Dependencies
# =============================================================================
find_package(spdlog CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)

if(RVX_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
endif()

# ShaderReflection uses SPIRV-Reflect for every Windows developer build (DXC
# may emit SPIR-V even when the runtime Vulkan backend is disabled) and for
# Vulkan developer builds on other platforms.
if(RVX_BUILD_DEVELOPER AND (WIN32 OR RVX_ENABLE_VULKAN))
    find_package(unofficial-spirv-reflect CONFIG REQUIRED)
endif()

# SPIRV-Cross is needed for Metal/OpenGL translation and for portable SPIR-V
# reflection when non-Windows platforms compile Vulkan shaders with glslang.
if(RVX_BUILD_DEVELOPER AND
   (RVX_ENABLE_METAL OR RVX_ENABLE_OPENGL OR
    (RVX_ENABLE_VULKAN AND NOT WIN32)))
    # Note: Order matters - glsl must be found before msl due to dependencies
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    if(RVX_ENABLE_METAL)
        find_package(spirv_cross_hlsl CONFIG REQUIRED)
        find_package(spirv_cross_msl CONFIG REQUIRED)
    endif()
endif()

# OpenGL dependencies
if(RVX_ENABLE_OPENGL)
    find_package(glad CONFIG REQUIRED)
    find_package(OpenGL REQUIRED)
endif()

if(WIN32)
    if(RVX_BUILD_DEVELOPER)
        find_package(directx-dxc CONFIG REQUIRED)
    endif()
    if(RVX_ENABLE_DX12)
        # Detect Windows SDK path for D3D12.lib (required by directx12-agility)
        # CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION is set by Visual Studio generators
        if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            set(_WIN_SDK_VERSION ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION})
        elseif(DEFINED ENV{WindowsSDKVersion})
            string(REGEX REPLACE "\\\\$" "" _WIN_SDK_VERSION "$ENV{WindowsSDKVersion}")
        else()
            # Default to a common version
            set(_WIN_SDK_VERSION "10.0.22621.0")
        endif()
        
        # Common Windows 10/11 SDK paths
        set(_WIN_SDK_SEARCH_PATHS
            "$ENV{WindowsSdkDir}/Lib/${_WIN_SDK_VERSION}/um/x64"
            "C:/Program Files (x86)/Windows Kits/10/Lib/${_WIN_SDK_VERSION}/um/x64"
            "${CMAKE_WINDOWS_KITS_10_DIR}/Lib/${_WIN_SDK_VERSION}/um/x64"
        )
        
        find_library(D3D12_SDK_LIB d3d12 PATHS ${_WIN_SDK_SEARCH_PATHS} NO_DEFAULT_PATH)
        if(NOT D3D12_SDK_LIB)
            # Fallback: let CMake search default paths
            find_library(D3D12_SDK_LIB d3d12)
        endif()
        
        if(D3D12_SDK_LIB)
            message(STATUS "Found D3D12: ${D3D12_SDK_LIB}")
            # Set D3D12_LIB in cache so directx12-agility-config.cmake can find it
            # Use FORCE to override any previous NOTFOUND value
            set(D3D12_LIB "${D3D12_SDK_LIB}" CACHE FILEPATH "Path to D3D12.lib" FORCE)
            find_package(directx12-agility CONFIG)
            # vcpkg port name is `d3d12-memory-allocator` and it exports `unofficial::D3D12MemoryAllocator`.
            find_package(d3d12-memory-allocator CONFIG)
        else()
            message(WARNING "D3D12.lib not found in Windows SDK - disabling DX12 backend. "
                            "Searched in: ${_WIN_SDK_SEARCH_PATHS}")
            set(RVX_ENABLE_DX12 OFF)
        endif()
    endif()
endif()

# Non-Windows: Use glslang for HLSL -> SPIR-V compilation (DXC is not
# available through the supported vcpkg toolchains).
if(NOT WIN32 AND RVX_BUILD_DEVELOPER)
    find_package(glslang CONFIG REQUIRED)
endif()

if(RVX_BUILD_SHIPPING_PRODUCTS)
    if(WIN32 AND NOT RVX_ENABLE_DX12)
        message(FATAL_ERROR
            "Windows shipping products require the DX12 backend")
    elseif(APPLE AND NOT RVX_ENABLE_METAL)
        message(FATAL_ERROR
            "Apple shipping products require the Metal backend")
    elseif(UNIX AND NOT APPLE AND NOT RVX_ENABLE_VULKAN)
        message(FATAL_ERROR
            "Linux shipping products require the Vulkan backend")
    endif()
endif()

# =============================================================================
# Compiler Flags
# =============================================================================
if(MSVC)
    add_compile_options(/W4 /WX-)
    add_compile_options(/MP)  # Multi-processor compilation
    add_compile_options(/utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(RVX_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "RVX_ENABLE_TSAN requires Clang or GCC on a supported Unix platform")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "RVX_ENABLE_TSAN requires Clang or GCC")
    endif()
    target_compile_options(RVX_Options INTERFACE
        -fsanitize=thread
        -fno-omit-frame-pointer
    )
    target_link_options(RVX_Options INTERFACE
        -fsanitize=thread
    )
endif()

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(Source/Runtime/Core)
add_subdirectory(Source/Runtime/AssetContracts)
add_subdirectory(Source/Runtime/RenderContracts) # Render-only extraction data contracts
add_subdirectory(Source/Runtime/Geometry)     # Geometry module (primitives, queries, algorithms)
add_subdirectory(Source/Runtime/HAL)          # Hardware Abstraction Layer (Window, Input)
add_subdirectory(Source/Runtime/ECS)          # Runtime sparse-set entity/fragment kernel
add_subdirectory(Source/Runtime/Scene)
add_subdirectory(Source/Runtime/Animation)
add_subdirectory(Source/Runtime/RuntimeServices) # Runtime subsystems (includes Camera)
add_subdirectory(Source/Runtime/RHI)
add_subdirectory(Source/Runtime/ShaderContracts)
add_subdirectory(Source/Runtime/ShaderRuntime)
if(RVX_BUILD_DEVELOPER)
    add_subdirectory(Source/Developer/ShaderCompiler)
else()
    rvx_register_disabled_module(
        MODULE ShaderCompiler
        REASON "Developer components are disabled for this configuration")
endif()

if(RVX_ENABLE_DX11)
    add_subdirectory(Source/Runtime/RHI_DX11)
else()
    rvx_register_disabled_module(
        MODULE RHI_DX11
        REASON "DX11 backend is disabled for this configuration")
endif()

if(RVX_ENABLE_DX12)
    add_subdirectory(Source/Runtime/RHI_DX12)
else()
    rvx_register_disabled_module(
        MODULE RHI_DX12
        REASON "DX12 backend is disabled for this configuration")
endif()

if(RVX_ENABLE_VULKAN)
    add_subdirectory(Source/Runtime/RHI_Vulkan)
else()
    rvx_register_disabled_module(
        MODULE RHI_Vulkan
        REASON "Vulkan backend is disabled for this configuration")
endif()

if(RVX_ENABLE_METAL)
    add_subdirectory(Source/Runtime/RHI_Metal)
else()
    rvx_register_disabled_module(
        MODULE RHI_Metal
        REASON "Metal backend is disabled for this configuration")
endif()

if(RVX_ENABLE_OPENGL)
    add_subdirectory(Source/Runtime/RHI_OpenGL)
else()
    rvx_register_disabled_module(
        MODULE RHI_OpenGL
        REASON "OpenGL backend is disabled for this configuration")
endif()

add_subdirectory(Source/Runtime/RHI_BackendFactory) # Enabled RHI backend composition

add_subdirectory(Source/Runtime/ResourceRuntime) # Resource runtime and cooked resource management
if(RVX_BUILD_DEVELOPER)
    add_subdirectory(Source/Developer/AssetImport) # Development-only source importer adapters
    add_subdirectory(Source/Developer/ResourceSource) # Development source loading and monitoring
else()
    rvx_register_disabled_module(
        MODULE AssetImport
        REASON "Developer components are disabled for this configuration")
    rvx_register_disabled_module(
        MODULE ResourceSource
        REASON "Developer components are disabled for this configuration")
endif()
add_subdirectory(Source/Runtime/ResourceSceneAdapters) # Resource data to pure ECS scene adapters
add_subdirectory(Source/Runtime/Physics)    # Physics subsystem
add_subdirectory(Source/Runtime/PhysicsSceneAdapters) # Pure ECS to Built-in Physics composition
add_subdirectory(Source/Runtime/AnimationSceneAdapters) # Animation runtime to Scene ECS adapters
add_subdirectory(Source/Runtime/Audio) # Audio subsystem
add_subdirectory(Source/Runtime/World)      # World owns the single Scene ECS runtime
add_subdirectory(Source/Runtime/RenderExtraction) # Frozen ECS values to render contracts
add_subdirectory(Source/Runtime/Render)       # Render module (includes RenderGraph)
add_subdirectory(Source/Runtime/Engine)
if(RVX_BUILD_DEVELOPER)
    add_subdirectory(Source/Developer/EngineDevelopmentComposition)
else()
    rvx_register_disabled_module(
        MODULE EngineDevelopmentComposition
        REASON "Developer components are disabled for this configuration")
endif()
add_subdirectory(Source/Runtime/Particle) # GPU-driven particle system
add_subdirectory(Source/Runtime/AI) # AI module (Navigation, BehaviorTree, Perception)
add_subdirectory(Source/Runtime/Networking) # Network communication and replication
if(RVX_BUILD_DEVELOPER)
    add_subdirectory(Source/Developer/DebugTools) # Debug module (profiling, console, stats)
else()
    rvx_register_disabled_module(
        MODULE Debug
        REASON "Developer components are disabled for this configuration")
endif()
add_subdirectory(Source/Runtime/Scripting) # Lua scripting system
if(RVX_BUILD_DEVELOPER)
    add_subdirectory(Source/Developer/AssetPipeline) # Asset pipeline and tools
else()
    rvx_register_disabled_module(
        MODULE AssetPipeline
        REASON "Developer components are disabled for this configuration")
endif()
if(RVX_BUILD_PROGRAMS)
    add_subdirectory(Source/Programs)
else()
    rvx_register_disabled_module(
        MODULE RVXCook
        REASON "Developer command-line programs are disabled for this configuration")
    rvx_register_disabled_module(
        MODULE RVXAnimationDerive
        REASON "Developer command-line programs are disabled for this configuration")
endif()
add_subdirectory(Source/Runtime/UI) # UI framework
add_subdirectory(Source/Runtime/Terrain) # Terrain module (heightmap terrain rendering)
add_subdirectory(Source/Runtime/Water) # Water module (water surface and effects)

if(RVX_BUILD_EDITOR)
    add_subdirectory(Editor)
else()
    rvx_register_disabled_module(
        MODULE Editor
        REASON "Editor is explicitly deferred during the runtime architecture migration")
endif()

if(RVX_BUILD_SAMPLES)
    add_subdirectory(Samples)
else()
    rvx_register_disabled_module(
        MODULE Samples
        REASON "Sample products are disabled for this configuration")
endif()

if(RVX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(Tests)
endif()

if(RVX_BUILD_SHIPPING_PRODUCTS)
    add_subdirectory(Tests/ShippingConsumer)
endif()

if(RVX_BUILD_SHIPPING_TESTS)
    enable_testing()
    add_subdirectory(Tests/Shipping)
endif()

if(NOT RVX_BUILD_TESTS AND NOT RVX_BUILD_SHIPPING_PRODUCTS)
    rvx_register_disabled_module(
        MODULE Tests
        REASON "Validation targets are disabled for this configuration")
endif()

rvx_finalize_module_inventory(
    MANIFEST "${RVX_MODULE_MANIFEST}"
    OUTPUT "${RVX_MODULE_INVENTORY}"
)

# =============================================================================
# Status Summary
# =============================================================================
message(STATUS "")
message(STATUS "=== RenderVerseX Configuration ===")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  Platform:     ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Build Type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  DX11 Backend: ${RVX_ENABLE_DX11}")
message(STATUS "  DX12 Backend: ${RVX_ENABLE_DX12}")
message(STATUS "  Vulkan:       ${RVX_ENABLE_VULKAN}")
message(STATUS "  Metal:        ${RVX_ENABLE_METAL}")
message(STATUS "  OpenGL:       ${RVX_ENABLE_OPENGL}")
message(STATUS "  Developer:    ${RVX_BUILD_DEVELOPER}")
message(STATUS "  Programs:     ${RVX_BUILD_PROGRAMS}")
message(STATUS "  Shipping:     ${RVX_BUILD_SHIPPING_PRODUCTS}")
message(STATUS "  ShippingTests:${RVX_BUILD_SHIPPING_TESTS}")
message(STATUS "  Editor:       ${RVX_BUILD_EDITOR}")
message(STATUS "  Samples:      ${RVX_BUILD_SAMPLES}")
message(STATUS "  Tests:        ${RVX_BUILD_TESTS}")
message(STATUS "  TSAN:         ${RVX_ENABLE_TSAN}")
message(STATUS "")
