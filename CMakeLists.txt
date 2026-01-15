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
add_subdirectory(Platform)
add_subdirectory(Input)
add_subdirectory(Camera)
add_subdirectory(Engine)
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
add_subdirectory(Render)

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
