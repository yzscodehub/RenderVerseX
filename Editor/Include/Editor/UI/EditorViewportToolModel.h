/**
 * @file EditorViewportToolModel.h
 * @brief Native viewport tool state model
 */

#pragma once

#include "Core/Types.h"
#include "Editor/EditorContext.h"

#include <string>

namespace RVX::Editor
{

struct EditorViewportToolState
{
    EditorContext::GizmoMode mode = EditorContext::GizmoMode::Translate;
    EditorContext::GizmoSpace space = EditorContext::GizmoSpace::World;
    bool snapEnabled = false;
    float snapValue = 1.0f;
};

struct EditorViewportToolModelStats
{
    EditorViewportToolState state;
    uint32 modeCommandCount = 0;
    uint32 spaceCommandCount = 0;
    uint32 snapCommandCount = 0;
    bool usesLegacyGizmoAdapter = false;
};

class EditorViewportToolModel
{
public:
    EditorViewportToolModel() = default;

    EditorViewportToolState CaptureState() const;
    const EditorViewportToolModelStats& GetLastStats() const { return m_lastStats; }

    bool IsModeActive(EditorContext::GizmoMode mode) const;
    bool IsSpaceActive(EditorContext::GizmoSpace space) const;

    void SetMode(EditorContext::GizmoMode mode);
    void SetSpace(EditorContext::GizmoSpace space);
    void ToggleSnap();
    void SetSnapEnabled(bool enabled);
    void SetSnapValue(float value);

    const char* GetModeLabel(EditorContext::GizmoMode mode) const;
    const char* GetSpaceLabel(EditorContext::GizmoSpace space) const;

private:
    void PublishStats();

    mutable EditorViewportToolModelStats m_lastStats;
};

} // namespace RVX::Editor
