/**
 * @file IEditorPanel.h
 * @brief Base interface for editor panels
 */

#pragma once

#include "Core/Types.h"
#include "UI/UIContext.h"

#include <string>

namespace RVX::Editor
{

/**
 * @brief Interface for editor panels
 *
 * All editor panels inherit from this interface to provide
 * a consistent API for the editor window manager.
 */
class IEditorPanel
{
public:
    virtual ~IEditorPanel() = default;

    // =========================================================================
    // Identity
    // =========================================================================

    /**
     * @brief Get the panel name
     */
    virtual const char* GetName() const = 0;

    /**
     * @brief Get the panel icon identifier
     */
    virtual const char* GetIcon() const { return ""; }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    /**
     * @brief Called when panel is created/initialized
     */
    virtual void OnInit() {}

    /**
     * @brief Called when panel is destroyed
     */
    virtual void OnShutdown() {}

    /**
     * @brief Called every frame for logic updates
     */
    virtual void OnUpdate(float deltaTime) { (void)deltaTime; }

    /**
     * @brief Called to render the legacy panel UI through the debug ImGui bridge
     */
    virtual void OnGUI() = 0;

    /**
     * @brief Called after legacy UI has updated panel-local hover/focus state.
     */
    virtual void OnNativeInput(const UI::UIInputState& input) { (void)input; }

    // =========================================================================
    // Visibility
    // =========================================================================

    bool IsVisible() const { return m_visible; }
    void SetVisible(bool visible) { m_visible = visible; }
    void Show() { m_visible = true; }
    void Hide() { m_visible = false; }
    void Toggle() { m_visible = !m_visible; }

    bool IsFocused() const { return m_focused; }
    void SetFocused(bool focused) { m_focused = focused; }

protected:
    bool m_visible = true;
    bool m_focused = false;
};

} // namespace RVX::Editor
