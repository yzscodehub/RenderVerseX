# =============================================================================
# Picking Module - Mouse picking and object selection
# =============================================================================
add_library(RVX_Picking STATIC
    Private/PickingSystem.cpp
)

target_include_directories(RVX_Picking PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Private includes for internal BVH implementation
target_include_directories(RVX_Picking PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Private
)

target_link_libraries(RVX_Picking PUBLIC
    RVX::Core
    RVX::Scene
    RVX::Camera
)

add_library(RVX::Picking ALIAS RVX_Picking)
