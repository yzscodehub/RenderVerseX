/**
 * @file EditorViewportToolService.h
 * @brief Shared native viewport tool state service.
 */

#pragma once

#include "Editor/UI/EditorViewportToolModel.h"

namespace RVX::Editor
{

/**
 * @brief Service boundary for editor viewport tool commands and toolbar state.
 */
class EditorViewportToolService
{
public:
    // =========================================================================
    // State
    // =========================================================================

    EditorViewportToolState CaptureState() const;
    const EditorViewportToolModelStats& GetLastStats() const;

    bool IsModeActive(EditorContext::GizmoMode mode) const;
    bool IsSpaceActive(EditorContext::GizmoSpace space) const;

    // =========================================================================
    // Commands
    // =========================================================================

    void SetMode(EditorContext::GizmoMode mode);
    void SetSpace(EditorContext::GizmoSpace space);
    void ToggleSnap();
    void SetSnapEnabled(bool enabled);
    void SetSnapValue(float value);

    const char* GetModeLabel(EditorContext::GizmoMode mode) const;
    const char* GetSpaceLabel(EditorContext::GizmoSpace space) const;

private:
    EditorViewportToolModel m_model;
};

} // namespace RVX::Editor
