# =============================================================================
# Input Module - Input state and event processing
# =============================================================================
add_library(RVX_Input STATIC)

target_sources(RVX_Input PRIVATE
    Private/Input.cpp
)

target_include_directories(RVX_Input PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_Input PUBLIC
    RVX_Core
)

add_library(RVX::Input ALIAS RVX_Input)
