/**
 * @file EditorViewportToolService.cpp
 * @brief Shared native viewport tool state service implementation.
 */

#include "Editor/EditorViewportToolService.h"

namespace RVX::Editor
{

EditorViewportToolState EditorViewportToolService::CaptureState() const
{
    return m_model.CaptureState();
}

const EditorViewportToolModelStats& EditorViewportToolService::GetLastStats() const
{
    return m_model.GetLastStats();
}

bool EditorViewportToolService::IsModeActive(EditorContext::GizmoMode mode) const
{
    return m_model.IsModeActive(mode);
}

bool EditorViewportToolService::IsSpaceActive(EditorContext::GizmoSpace space) const
{
    return m_model.IsSpaceActive(space);
}

void EditorViewportToolService::SetMode(EditorContext::GizmoMode mode)
{
    m_model.SetMode(mode);
}

void EditorViewportToolService::SetSpace(EditorContext::GizmoSpace space)
{
    m_model.SetSpace(space);
}

void EditorViewportToolService::ToggleSnap()
{
    m_model.ToggleSnap();
}

void EditorViewportToolService::SetSnapEnabled(bool enabled)
{
    m_model.SetSnapEnabled(enabled);
}

void EditorViewportToolService::SetSnapValue(float value)
{
    m_model.SetSnapValue(value);
}

const char* EditorViewportToolService::GetModeLabel(EditorContext::GizmoMode mode) const
{
    return m_model.GetModeLabel(mode);
}

const char* EditorViewportToolService::GetSpaceLabel(EditorContext::GizmoSpace space) const
{
    return m_model.GetSpaceLabel(space);
}

} // namespace RVX::Editor
