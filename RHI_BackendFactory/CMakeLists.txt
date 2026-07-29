# =============================================================================
# RHI Backend Factory - Enabled backend composition
# =============================================================================
add_library(RVX_RHI_BackendFactory STATIC)

target_sources(RVX_RHI_BackendFactory PRIVATE
    Private/RHIBackendFactory.cpp
)

target_include_directories(RVX_RHI_BackendFactory PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_RHI_BackendFactory PUBLIC
    RVX::RHI
)

if(RVX_ENABLE_DX11)
    target_link_libraries(RVX_RHI_BackendFactory PRIVATE RVX::RHI_DX11)
endif()

if(RVX_ENABLE_DX12)
    target_link_libraries(RVX_RHI_BackendFactory PRIVATE RVX::RHI_DX12)
endif()

if(RVX_ENABLE_VULKAN)
    target_link_libraries(RVX_RHI_BackendFactory PRIVATE RVX::RHI_Vulkan)
endif()

if(RVX_ENABLE_METAL)
    target_link_libraries(RVX_RHI_BackendFactory PRIVATE RVX::RHI_Metal)
endif()

if(RVX_ENABLE_OPENGL)
    target_link_libraries(RVX_RHI_BackendFactory PRIVATE RVX::RHI_OpenGL)
endif()

add_library(RVX::RHI_BackendFactory ALIAS RVX_RHI_BackendFactory)
