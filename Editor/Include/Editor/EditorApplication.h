/**
 * @file EditorApplication.h
 * @brief Main editor application class
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorWindow.h"
#include "Editor/EditorContext.h"
#include <memory>
#include <vector>
#include <string>

struct GLFWwindow;

namespace RVX
{
    class IRHIDevice;
    class RHISwapChain;
}

namespace RVX::Editor
{

class IEditorPanel;

/**
 * @brief Main editor application
 * 
 * Manages the editor window, ImGui context, and all editor panels.
 */
class EditorApplication
{
public:
    EditorApplication();
    ~EditorApplication();

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Initialize the editor
     * @return true if initialization succeeded
     */
    bool Initialize();

    /**
     * @brief Run the main editor loop
     * @return Exit code
     */
    int Run();

    /**
     * @brief Shutdown the editor
     */
    void Shutdown();

    // =========================================================================
    // Window
    // =========================================================================

    /**
     * @brief Get the main window
     */
    GLFWwindow* GetWindow() const { return m_window; }

    /**
     * @brief Get window dimensions
     */
    void GetWindowSize(int& width, int& height) const;

    // =========================================================================
    // Panels
    // =========================================================================

    /**
     * @brief Register a panel
     */
    void RegisterPanel(std::shared_ptr<IEditorPanel> panel);

    /**
     * @brief Get a panel by name
     */
    IEditorPanel* GetPanel(const std::string& name);

    // =========================================================================
    // Frame
    // =========================================================================

    /**
     * @brief Get delta time for current frame
     */
    float GetDeltaTime() const { return m_deltaTime; }

    /**
     * @brief Get total elapsed time
     */
    float GetTime() const { return m_totalTime; }

private:
    bool InitializeWindow();
    bool InitializeRHI();
    bool InitializeImGui();
    bool InitializePanels();

    void BeginFrame();
    void Update(float deltaTime);
    void Render();
    void EndFrame();

    void DrawMainMenuBar();
    void DrawDockSpace();
    void DrawStatusBar();
    void DrawPanels();

    void HandleShortcuts();

    // Window
    GLFWwindow* m_window = nullptr;
    int m_windowWidth = 1920;
    int m_windowHeight = 1080;
    std::string m_windowTitle = "RenderVerseX Editor";

    // RHI
    std::shared_ptr<IRHIDevice> m_device;
    std::shared_ptr<RHISwapChain> m_swapChain;

    // Panels
    std::vector<std::shared_ptr<IEditorPanel>> m_panels;

    // Timing
    float m_deltaTime = 0.0f;
    float m_totalTime = 0.0f;
    double m_lastFrameTime = 0.0;

    // State
    bool m_running = false;
    bool m_showDemoWindow = false;
    bool m_showMetricsWindow = false;
};

} // namespace RVX::Editor
