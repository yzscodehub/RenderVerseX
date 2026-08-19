# =============================================================================
# UI Module - User interface system
# =============================================================================
add_library(RVX_UI STATIC)

target_sources(RVX_UI PRIVATE
    Private/Widget.cpp
    Private/UICanvas.cpp
    Private/UIClipboard.cpp
    Private/UIContext.cpp
    Private/UIRenderer.cpp
    Private/UITextOverflow.cpp
    Private/Widgets/Button.cpp
    Private/Widgets/Label.cpp
    Private/Widgets/Panel.cpp
    Private/Widgets/Image.cpp
    Private/Widgets/ScrollView.cpp
    Private/Layout/Layout.cpp
)

target_include_directories(RVX_UI PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

find_path(STB_INCLUDE_DIR stb_truetype.h)
if(NOT STB_INCLUDE_DIR)
    message(FATAL_ERROR "RVX_UI: stb_truetype.h not found")
endif()

target_include_directories(RVX_UI PRIVATE
    ${STB_INCLUDE_DIR}
)

target_link_libraries(RVX_UI PUBLIC
    RVX::Core
    RVX::RHI
)

add_library(RVX::UI ALIAS RVX_UI)
