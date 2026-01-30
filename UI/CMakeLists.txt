# =============================================================================
# UI Module - User interface system
# =============================================================================
add_library(RVX_UI STATIC)

target_sources(RVX_UI PRIVATE
    Private/Widget.cpp
    Private/UICanvas.cpp
    Private/UIRenderer.cpp
    Private/Widgets/Button.cpp
    Private/Widgets/Label.cpp
    Private/Widgets/Panel.cpp
    Private/Widgets/Image.cpp
    Private/Layout/Layout.cpp
)

target_include_directories(RVX_UI PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(RVX_UI PUBLIC
    RVX::Core
    RVX::RHI
    RVX::Render
)

add_library(RVX::UI ALIAS RVX_UI)
