# =============================================================================
# RHI Module - Rendering Hardware Interface abstractions
# =============================================================================
add_library(RVX_RHI STATIC)

target_sources(RVX_RHI PRIVATE
    Private/RHIModule.cpp
    Private/RHIValidation.cpp
    Private/DescriptorCache.cpp
)

target_include_directories(RVX_RHI PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI PUBLIC
    RVX::Core
)

# Backend compile definitions
if(RVX_ENABLE_DX11)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_DX11=1)
endif()

if(RVX_ENABLE_DX12)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_DX12=1)
endif()

if(RVX_ENABLE_VULKAN)
    target_compile_definitions(RVX_RHI PUBLIC RVX_ENABLE_VULKAN=1)
endif()

add_library(RVX::RHI ALIAS RVX_RHI)
