/**
 * @file EditorViewportToolModel.cpp
 * @brief Native viewport tool state model implementation
 */

#include "Editor/UI/EditorViewportToolModel.h"

#include <algorithm>

namespace RVX::Editor
{

EditorViewportToolState EditorViewportToolModel::CaptureState() const
{
    const EditorContext& context = EditorContext::Get();
    EditorViewportToolState state;
    state.mode = context.GetGizmoMode();
    state.space = context.GetGizmoSpace();
    state.snapEnabled = context.IsSnapEnabled();
    state.snapValue = context.GetSnapValue();

    m_lastStats.state = state;
    m_lastStats.usesLegacyGizmoAdapter = false;
    return state;
}

bool EditorViewportToolModel::IsModeActive(EditorContext::GizmoMode mode) const
{
    return CaptureState().mode == mode;
}

bool EditorViewportToolModel::IsSpaceActive(EditorContext::GizmoSpace space) const
{
    return CaptureState().space == space;
}

void EditorViewportToolModel::SetMode(EditorContext::GizmoMode mode)
{
    EditorContext::Get().SetGizmoMode(mode);
    ++m_lastStats.modeCommandCount;
    PublishStats();
}

void EditorViewportToolModel::SetSpace(EditorContext::GizmoSpace space)
{
    EditorContext::Get().SetGizmoSpace(space);
    ++m_lastStats.spaceCommandCount;
    PublishStats();
}

void EditorViewportToolModel::ToggleSnap()
{
    SetSnapEnabled(!EditorContext::Get().IsSnapEnabled());
}

void EditorViewportToolModel::SetSnapEnabled(bool enabled)
{
    EditorContext::Get().SetSnapEnabled(enabled);
    ++m_lastStats.snapCommandCount;
    PublishStats();
}

void EditorViewportToolModel::SetSnapValue(float value)
{
    EditorContext::Get().SetSnapValue(std::max(0.0f, value));
    ++m_lastStats.snapCommandCount;
    PublishStats();
}

const char* EditorViewportToolModel::GetModeLabel(EditorContext::GizmoMode mode) const
{
    switch (mode)
    {
        case EditorContext::GizmoMode::Translate:
            return "Move";
        case EditorContext::GizmoMode::Rotate:
            return "Rotate";
        case EditorContext::GizmoMode::Scale:
            return "Scale";
    }
    return "Move";
}

const char* EditorViewportToolModel::GetSpaceLabel(EditorContext::GizmoSpace space) const
{
    switch (space)
    {
        case EditorContext::GizmoSpace::World:
            return "World";
        case EditorContext::GizmoSpace::Local:
            return "Local";
    }
    return "World";
}

void EditorViewportToolModel::PublishStats()
{
    CaptureState();
}

} // namespace RVX::Editor
