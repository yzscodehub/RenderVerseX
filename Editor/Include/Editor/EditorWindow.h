/**
 * @file EditorWindow.h
 * @brief Base editor window/panel class
 */

#pragma once

#include "Core/Types.h"
#include <string>
#include <memory>

namespace RVX::Editor
{

class EditorContext;

/**
 * @brief Base class for editor windows/panels
 */
class EditorWindow
{
public:
    using Ptr = std::shared_ptr<EditorWindow>;

    EditorWindow(const std::string& title);
    virtual ~EditorWindow() = default;

    // =========================================================================
    // Identity
    // =========================================================================

    const std::string& GetTitle() const { return m_title; }
    void SetTitle(const std::string& title) { m_title = title; }

    uint32 GetId() const { return m_id; }

    // =========================================================================
    // Visibility
    // =========================================================================

    bool IsOpen() const { return m_isOpen; }
    void SetOpen(bool open) { m_isOpen = open; }

    void Open() { m_isOpen = true; }
    void Close() { m_isOpen = false; }

    bool IsFocused() const { return m_isFocused; }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Called when window is created
     */
    virtual void OnCreate(EditorContext& context) {}

    /**
     * @brief Called when window is destroyed
     */
    virtual void OnDestroy() {}

    /**
     * @brief Update logic
     */
    virtual void OnUpdate(float deltaTime) {}

    /**
     * @brief Render the window using ImGui
     */
    virtual void OnImGuiRender() = 0;

    /**
     * @brief Handle menu bar (if has one)
     */
    virtual void OnMenuBar() {}

    // =========================================================================
    // Window Flags
    // =========================================================================

    bool HasMenuBar() const { return m_hasMenuBar; }
    void SetHasMenuBar(bool has) { m_hasMenuBar = has; }

    bool IsResizable() const { return m_isResizable; }
    void SetResizable(bool resizable) { m_isResizable = resizable; }

    bool IsDockable() const { return m_isDockable; }
    void SetDockable(bool dockable) { m_isDockable = dockable; }

protected:
    std::string m_title;
    uint32 m_id = 0;
    bool m_isOpen = true;
    bool m_isFocused = false;
    bool m_hasMenuBar = false;
    bool m_isResizable = true;
    bool m_isDockable = true;

    static uint32 s_nextId;
};

/**
 * @brief Editor window manager
 */
class EditorWindowManager
{
public:
    static EditorWindowManager& Get();

    /**
     * @brief Register a window
     */
    void RegisterWindow(EditorWindow::Ptr window);

    /**
     * @brief Get window by title
     */
    EditorWindow* GetWindow(const std::string& title);

    /**
     * @brief Get all windows
     */
    const std::vector<EditorWindow::Ptr>& GetWindows() const { return m_windows; }

    /**
     * @brief Update all windows
     */
    void UpdateAll(float deltaTime);

    /**
     * @brief Render all windows
     */
    void RenderAll();

private:
    std::vector<EditorWindow::Ptr> m_windows;
};

} // namespace RVX::Editor
