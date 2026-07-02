/**
 * @file EditorViewportSelectionModel.h
 * @brief Native viewport scene selection model
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorViewportProjection.h"
#include "UI/UIContext.h"

namespace RVX
{
    class SceneEntity;
    class SceneManager;
}

namespace RVX::Editor
{

class EditorContext;
class EditorSelectionService;

struct EditorViewportSelectionPickDesc
{
    SceneManager* sceneManager = nullptr;
    EditorViewportCameraFrame cameraFrame;
    Vec2 screenPosition{0.0f};
    bool clearSelectionOnMiss = true;
};

struct EditorViewportSelectionStats
{
    bool receivedClick = false;
    bool blockedByManipulator = false;
    bool hasScene = false;
    bool cameraValid = false;
    bool rayValid = false;
    bool insideViewport = false;
    bool hit = false;
    bool selected = false;
    bool clearedSelection = false;
    SceneEntity* hitEntity = nullptr;
    float hitDistance = 0.0f;
};

class EditorViewportSelectionModel
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================

    bool HandleInput(EditorContext& context,
                     const UI::UIInputState& input,
                     const EditorViewportSelectionPickDesc& desc,
                     bool blockedByManipulator);
    bool HandleInput(EditorSelectionService& selectionService,
                     const UI::UIInputState& input,
                     const EditorViewportSelectionPickDesc& desc,
                     bool blockedByManipulator);
    bool ApplyClickSelection(EditorContext& context,
                             const EditorViewportSelectionPickDesc& desc);
    bool ApplyClickSelection(EditorSelectionService& selectionService,
                             const EditorViewportSelectionPickDesc& desc);

    const EditorViewportSelectionStats& GetLastStats() const { return m_lastStats; }

private:
    EditorViewportSelectionStats m_lastStats;
};

} // namespace RVX::Editor
