# =============================================================================
# Platform Module - Windowing and platform abstraction
# =============================================================================
add_library(RVX_Platform STATIC)

target_sources(RVX_Platform PRIVATE
    Private/Window.cpp
    Private/InputBackend_GLFW.cpp
)

target_include_directories(RVX_Platform PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/Input/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Platform PUBLIC
    RVX_Core
    RVX_Input
    glfw
)

add_library(RVX::Platform ALIAS RVX_Platform)
