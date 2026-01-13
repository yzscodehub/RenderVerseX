cmake_minimum_required(VERSION 3.21)
project(RenderVerseX VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# C++ Standard
# =============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Build Options
# =============================================================================
option(RVX_ENABLE_DX11 "Enable DirectX 11 backend" ON)
option(RVX_ENABLE_DX12 "Enable DirectX 12 backend" ON)
option(RVX_ENABLE_VULKAN "Enable Vulkan backend" ON)
option(RVX_BUILD_SAMPLES "Build sample applications" ON)
option(RVX_BUILD_TESTS "Build validation tests" ON)

# =============================================================================
# Platform Detection
# =============================================================================
if(WIN32)
    set(RVX_PLATFORM_WINDOWS ON)
    add_compile_definitions(RVX_PLATFORM_WINDOWS=1)
    add_compile_definitions(NOMINMAX)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
elseif(UNIX AND NOT APPLE)
    set(RVX_PLATFORM_LINUX ON)
    add_compile_definitions(RVX_PLATFORM_LINUX=1)
    # Disable DX backends on Linux
    set(RVX_ENABLE_DX11 OFF)
    set(RVX_ENABLE_DX12 OFF)
endif()

# =============================================================================
# Find Dependencies
# =============================================================================
find_package(spdlog CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

if(RVX_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(unofficial-spirv-reflect CONFIG REQUIRED)
endif()

if(WIN32)
    find_package(directx-dxc CONFIG REQUIRED)
    if(RVX_ENABLE_DX12)
        find_package(directx12-agility CONFIG)
        find_package(D3D12MemoryAllocator CONFIG)
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

# =============================================================================
# Subdirectories
# =============================================================================
add_subdirectory(Core)
add_subdirectory(RHI)
add_subdirectory(ShaderCompiler)

if(RVX_ENABLE_DX11)
    add_subdirectory(RHI_DX11)
endif()

if(RVX_ENABLE_DX12)
    add_subdirectory(RHI_DX12)
endif()

if(RVX_ENABLE_VULKAN)
    add_subdirectory(RHI_Vulkan)
endif()

add_subdirectory(RenderGraph)

if(RVX_BUILD_SAMPLES)
    add_subdirectory(Samples)
endif()

if(RVX_BUILD_TESTS)
    add_subdirectory(Tests)
endif()

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
message(STATUS "  Samples:      ${RVX_BUILD_SAMPLES}")
message(STATUS "  Tests:        ${RVX_BUILD_TESTS}")
message(STATUS "")
