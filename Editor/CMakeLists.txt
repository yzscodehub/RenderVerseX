# =============================================================================
# Editor Module - RenderVerseX Editor Application
# =============================================================================
add_library(RVX_Editor STATIC)

target_sources(RVX_Editor PRIVATE
    Private/EditorWindow.cpp
    Private/EditorContext.cpp
    Private/EditorApplication.cpp
    Private/EditorTheme.cpp
    Private/Panels/SceneHierarchy.cpp
    Private/Panels/Inspector.cpp
    Private/Panels/AssetBrowser.cpp
    Private/Panels/Viewport.cpp
    Private/Panels/Console.cpp
    Private/Panels/AnimationEditor.cpp
    Private/Panels/MaterialEditor.cpp
    Private/Panels/ParticleEditor.cpp
)

target_include_directories(RVX_Editor PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
    $<INSTALL_INTERFACE:include>
)

# Find ImGui and ImGuizmo
find_package(imgui CONFIG REQUIRED)
find_package(imguizmo CONFIG REQUIRED)

# Find OpenGL for direct GL calls in ImGui backend
find_package(OpenGL REQUIRED)

target_link_libraries(RVX_Editor PUBLIC
    RVX::Core
    RVX::RHI
    RVX::Render
    RVX::Scene
    RVX::Tools
    RVX::UI
    RVX::Animation
    RVX::HAL
    OpenGL::GL
)

# Link ImGui for editor UI
target_link_libraries(RVX_Editor PRIVATE 
    imgui::imgui
    imguizmo::imguizmo
)
add_library(RVX::Editor ALIAS RVX_Editor)

# =============================================================================
# Editor Executable
# =============================================================================
add_executable(RVXEditor
    Private/Main.cpp
)

target_link_libraries(RVXEditor PRIVATE 
    RVX::Editor
    RVX::Engine
)
