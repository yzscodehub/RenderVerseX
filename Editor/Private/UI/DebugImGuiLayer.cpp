/**
 * @file DebugImGuiLayer.cpp
 * @brief Transitional ImGui backend wrapper implementation
 */

#include "Editor/UI/DebugImGuiLayer.h"
#include "Editor/EditorTheme.h"
#include "Core/Log.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <ImGuizmo.h>

namespace RVX::Editor
{

bool DebugImGuiLayer::Initialize(GLFWwindow* window, const DebugImGuiLayerDesc& desc)
{
    if (!window)
    {
        return false;
    }

    if (m_initialized)
    {
        Shutdown();
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    m_window = window;
    m_desc = desc;

    ImGuiIO& io = ImGui::GetIO();
    if (m_desc.enableDocking)
    {
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (m_desc.automatedRun)
    {
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
    }
    else if (m_desc.enableViewports)
    {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }

    io.FontGlobalScale = 1.0f;
    EditorTheme::Get().ApplyTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    if (!ImGui_ImplGlfw_InitForOpenGL(m_window, true))
    {
        RVX_CORE_ERROR("Failed to initialize debug ImGui GLFW backend");
        ImGui::DestroyContext();
        m_window = nullptr;
        return false;
    }

    if (!ImGui_ImplOpenGL3_Init(m_desc.glslVersion))
    {
        RVX_CORE_ERROR("Failed to initialize debug ImGui OpenGL backend");
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        m_window = nullptr;
        return false;
    }

    m_initialized = true;
    RVX_CORE_INFO("Debug ImGui layer initialized with docking{}",
                  m_desc.automatedRun ? " (multi-viewports disabled for automated run)" : " and viewports");
    return true;
}

void DebugImGuiLayer::Shutdown()
{
    if (!m_initialized)
    {
        return;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_window = nullptr;
    m_initialized = false;
}

void DebugImGuiLayer::BeginFrame()
{
    if (!m_initialized)
    {
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();
}

void DebugImGuiLayer::RenderMainFramebuffer()
{
    if (!m_initialized || !m_window)
    {
        return;
    }

    ImGui::Render();

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(m_window, &displayW, &displayH);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        GLFWwindow* backupContext = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(backupContext);
    }
}

} // namespace RVX::Editor
