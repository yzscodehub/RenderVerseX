/**
 * @file EditorWindow.cpp
 * @brief EditorWindow implementation
 */

#include "Editor/EditorWindow.h"

namespace RVX::Editor
{

uint32 EditorWindow::s_nextId = 1;

EditorWindow::EditorWindow(const std::string& title)
    : m_title(title)
    , m_id(s_nextId++)
{
}

// ============================================================================
// EditorWindowManager
// ============================================================================

EditorWindowManager& EditorWindowManager::Get()
{
    static EditorWindowManager instance;
    return instance;
}

void EditorWindowManager::RegisterWindow(EditorWindow::Ptr window)
{
    if (window)
    {
        m_windows.push_back(std::move(window));
    }
}

EditorWindow* EditorWindowManager::GetWindow(const std::string& title)
{
    for (auto& window : m_windows)
    {
        if (window->GetTitle() == title)
        {
            return window.get();
        }
    }
    return nullptr;
}

void EditorWindowManager::UpdateAll(float deltaTime)
{
    for (auto& window : m_windows)
    {
        if (window->IsOpen())
        {
            window->OnUpdate(deltaTime);
        }
    }
}

void EditorWindowManager::RenderAll()
{
    for (auto& window : m_windows)
    {
        if (window->IsOpen())
        {
            // TODO: Begin ImGui window
            // ImGui::Begin(window->GetTitle().c_str(), &window->m_isOpen);
            
            if (window->HasMenuBar())
            {
                // ImGui::BeginMenuBar();
                window->OnMenuBar();
                // ImGui::EndMenuBar();
            }

            window->OnImGuiRender();

            // ImGui::End();
        }
    }
}

} // namespace RVX::Editor
