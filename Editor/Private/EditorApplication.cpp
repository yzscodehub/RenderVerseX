/**
 * @file EditorApplication.cpp
 * @brief Editor application implementation
 */

#include "Editor/EditorApplication.h"
#include "Editor/EditorTheme.h"
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/Panels/Viewport.h"
#include "Editor/Panels/Inspector.h"
#include "Editor/Panels/SceneHierarchy.h"
#include "Editor/Panels/AssetBrowser.h"
#include "Editor/Panels/Console.h"
#include "Editor/Panels/AnimationEditor.h"
#include "Editor/Panels/MaterialEditor.h"
#include "Core/Log.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>

namespace RVX::Editor
{

EditorApplication::EditorApplication() = default;

EditorApplication::~EditorApplication()
{
    Shutdown();
}

bool EditorApplication::Initialize()
{
    RVX_CORE_INFO("Initializing EditorApplication...");

    if (!InitializeWindow())
    {
        RVX_CORE_ERROR("Failed to initialize window");
        return false;
    }

    if (!InitializeImGui())
    {
        RVX_CORE_ERROR("Failed to initialize ImGui");
        return false;
    }

    if (!InitializePanels())
    {
        RVX_CORE_ERROR("Failed to initialize panels");
        return false;
    }

    m_running = true;
    m_lastFrameTime = glfwGetTime();

    RVX_CORE_INFO("EditorApplication initialized successfully");
    return true;
}

bool EditorApplication::InitializeWindow()
{
    if (!glfwInit())
    {
        RVX_CORE_ERROR("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    m_window = glfwCreateWindow(m_windowWidth, m_windowHeight, 
                                 m_windowTitle.c_str(), nullptr, nullptr);
    if (!m_window)
    {
        RVX_CORE_ERROR("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);  // Enable VSync

    // Update window size
    glfwGetWindowSize(m_window, &m_windowWidth, &m_windowHeight);

    RVX_CORE_INFO("Window created: {}x{}", m_windowWidth, m_windowHeight);
    return true;
}

bool EditorApplication::InitializeRHI()
{
    // TODO: Initialize RHI device for rendering
    // For now, using OpenGL via ImGui backends
    return true;
}

bool EditorApplication::InitializeImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    // Set default font size
    io.FontGlobalScale = 1.0f;

    // Apply theme
    EditorTheme::Get().ApplyTheme();

    // When viewports are enabled, adjust style
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    RVX_CORE_INFO("ImGui initialized with docking and viewports");
    return true;
}

bool EditorApplication::InitializePanels()
{
    // Create and register standard panels
    RegisterPanel(std::make_shared<ViewportPanel>());
    RegisterPanel(std::make_shared<InspectorPanel>());
    RegisterPanel(std::make_shared<SceneHierarchyPanel>());
    RegisterPanel(std::make_shared<AssetBrowserPanel>());
    RegisterPanel(std::make_shared<ConsolePanel>());
    RegisterPanel(std::make_shared<AnimationEditorPanel>());
    RegisterPanel(std::make_shared<MaterialEditorPanel>());

    // Initialize all panels
    for (auto& panel : m_panels)
    {
        panel->OnInit();
    }

    RVX_CORE_INFO("Registered {} editor panels", m_panels.size());
    return true;
}

int EditorApplication::Run()
{
    RVX_CORE_INFO("Entering main loop");

    while (m_running && !glfwWindowShouldClose(m_window))
    {
        BeginFrame();
        Update(m_deltaTime);
        Render();
        EndFrame();
    }

    return 0;
}

void EditorApplication::Shutdown()
{
    if (!m_window)
        return;

    RVX_CORE_INFO("Shutting down editor...");

    // Shutdown panels
    for (auto& panel : m_panels)
    {
        panel->OnShutdown();
    }
    m_panels.clear();

    // Cleanup ImGui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // Cleanup window
    glfwDestroyWindow(m_window);
    glfwTerminate();
    m_window = nullptr;
}

void EditorApplication::BeginFrame()
{
    glfwPollEvents();

    // Calculate delta time
    double currentTime = glfwGetTime();
    m_deltaTime = static_cast<float>(currentTime - m_lastFrameTime);
    m_lastFrameTime = currentTime;
    m_totalTime += m_deltaTime;

    // Start ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void EditorApplication::Update(float deltaTime)
{
    HandleShortcuts();

    // Update all panels
    for (auto& panel : m_panels)
    {
        if (panel->IsVisible())
        {
            panel->OnUpdate(deltaTime);
        }
    }

    // Update editor context
    // EditorContext::Get().Update(deltaTime);
}

void EditorApplication::Render()
{
    // Draw main UI
    DrawDockSpace();
    DrawMainMenuBar();
    DrawPanels();
    DrawStatusBar();

    // Optional debug windows
    if (m_showDemoWindow)
    {
        ImGui::ShowDemoWindow(&m_showDemoWindow);
    }
    if (m_showMetricsWindow)
    {
        ImGui::ShowMetricsWindow(&m_showMetricsWindow);
    }
}

void EditorApplication::EndFrame()
{
    // Render ImGui
    ImGui::Render();

    int displayW, displayH;
    glfwGetFramebufferSize(m_window, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Handle viewports
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }

    glfwSwapBuffers(m_window);
}

void EditorApplication::DrawMainMenuBar()
{
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("New Scene", "Ctrl+N"))
            {
                EditorContext::Get().NewScene();
            }
            if (ImGui::MenuItem("Open Scene", "Ctrl+O"))
            {
                // TODO: Open file dialog
            }
            if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
            {
                // TODO: Save scene
            }
            if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
            {
                // TODO: Save as dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open Project..."))
            {
                // TODO: Open project dialog
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                m_running = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit"))
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, EditorContext::Get().CanUndo()))
            {
                EditorContext::Get().Undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, EditorContext::Get().CanRedo()))
            {
                EditorContext::Get().Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X"))
            {
                // TODO: Cut
            }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))
            {
                // TODO: Copy
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V"))
            {
                // TODO: Paste
            }
            if (ImGui::MenuItem("Delete", "Delete"))
            {
                // TODO: Delete selected
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Preferences..."))
            {
                // TODO: Show preferences
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            for (auto& panel : m_panels)
            {
                bool visible = panel->IsVisible();
                if (ImGui::MenuItem(panel->GetName(), nullptr, &visible))
                {
                    panel->SetVisible(visible);
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &m_showDemoWindow);
            ImGui::MenuItem("ImGui Metrics", nullptr, &m_showMetricsWindow);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("GameObject"))
        {
            if (ImGui::MenuItem("Create Empty", "Ctrl+Shift+N"))
            {
                // TODO: Create empty entity
            }
            if (ImGui::BeginMenu("3D Object"))
            {
                if (ImGui::MenuItem("Cube")) {}
                if (ImGui::MenuItem("Sphere")) {}
                if (ImGui::MenuItem("Plane")) {}
                if (ImGui::MenuItem("Cylinder")) {}
                if (ImGui::MenuItem("Capsule")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Light"))
            {
                if (ImGui::MenuItem("Directional Light")) {}
                if (ImGui::MenuItem("Point Light")) {}
                if (ImGui::MenuItem("Spot Light")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Audio"))
            {
                if (ImGui::MenuItem("Audio Source")) {}
                if (ImGui::MenuItem("Audio Listener")) {}
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Effects"))
            {
                if (ImGui::MenuItem("Particle System")) {}
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Camera")) {}
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Window"))
        {
            if (ImGui::MenuItem("Reset Layout"))
            {
                // TODO: Reset docking layout
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("Documentation"))
            {
                // TODO: Open documentation
            }
            if (ImGui::MenuItem("About RenderVerseX"))
            {
                // TODO: Show about dialog
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void EditorApplication::DrawDockSpace()
{
    static ImGuiDockNodeFlags dockspaceFlags = ImGuiDockNodeFlags_None;

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    
    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    windowFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::Begin("DockSpace", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable)
    {
        ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), dockspaceFlags);
    }

    ImGui::End();
}

void EditorApplication::DrawStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    float statusBarHeight = 24.0f;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, 
                                    viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    if (ImGui::Begin("StatusBar", nullptr, flags))
    {
        // Left side: Status message
        if (EditorContext::Get().IsPlaying())
        {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Playing");
        }
        else
        {
            ImGui::Text("Ready");
        }

        // Right side: FPS and memory
        ImGui::SameLine(ImGui::GetWindowWidth() - 200.0f);
        ImGui::Text("FPS: %.1f (%.2f ms)", 1.0f / m_deltaTime, m_deltaTime * 1000.0f);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void EditorApplication::DrawPanels()
{
    for (auto& panel : m_panels)
    {
        if (panel->IsVisible())
        {
            panel->OnGUI();
        }
    }
}

void EditorApplication::HandleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();

    // Don't process shortcuts when typing in text fields
    if (io.WantTextInput)
        return;

    bool ctrl = io.KeyCtrl;
    bool shift = io.KeyShift;

    // File shortcuts
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_N))
    {
        if (shift)
        {
            // Create empty entity
        }
        else
        {
            EditorContext::Get().NewScene();
        }
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S))
    {
        // Save scene
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O))
    {
        // Open scene
    }

    // Edit shortcuts
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z))
    {
        if (shift)
            EditorContext::Get().Redo();
        else
            EditorContext::Get().Undo();
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y))
    {
        EditorContext::Get().Redo();
    }

    // Gizmo shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_W))
    {
        EditorContext::Get().SetGizmoMode(EditorContext::GizmoMode::Translate);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_E))
    {
        EditorContext::Get().SetGizmoMode(EditorContext::GizmoMode::Rotate);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R))
    {
        EditorContext::Get().SetGizmoMode(EditorContext::GizmoMode::Scale);
    }

    // Focus shortcuts
    if (ImGui::IsKeyPressed(ImGuiKey_F))
    {
        // Focus on selected object
    }
}

void EditorApplication::GetWindowSize(int& width, int& height) const
{
    width = m_windowWidth;
    height = m_windowHeight;
}

void EditorApplication::RegisterPanel(std::shared_ptr<IEditorPanel> panel)
{
    if (panel)
    {
        m_panels.push_back(std::move(panel));
    }
}

IEditorPanel* EditorApplication::GetPanel(const std::string& name)
{
    for (auto& panel : m_panels)
    {
        if (panel->GetName() == name)
        {
            return panel.get();
        }
    }
    return nullptr;
}

} // namespace RVX::Editor
