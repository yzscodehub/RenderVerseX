/**
 * @file EditorViewportSelectionModel.cpp
 * @brief Native viewport scene selection model implementation
 */

#include "Editor/UI/EditorViewportSelectionModel.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorSelectionService.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneManager.h"

namespace RVX::Editor
{

bool EditorViewportSelectionModel::HandleInput(
    EditorContext& context,
    const UI::UIInputState& input,
    const EditorViewportSelectionPickDesc& desc,
    bool blockedByManipulator)
{
    EditorSelectionService selectionService(context);
    return HandleInput(selectionService, input, desc, blockedByManipulator);
}

bool EditorViewportSelectionModel::HandleInput(
    EditorSelectionService& selectionService,
    const UI::UIInputState& input,
    const EditorViewportSelectionPickDesc& desc,
    bool blockedByManipulator)
{
    m_lastStats = {};
    m_lastStats.blockedByManipulator = blockedByManipulator;

    if (!input.WasMouseButtonPressed(UI::UIMouseButton::Left))
    {
        return false;
    }

    m_lastStats.receivedClick = true;
    if (blockedByManipulator)
    {
        return false;
    }

    EditorViewportSelectionPickDesc clickDesc = desc;
    clickDesc.screenPosition = input.current.mousePosition;
    return ApplyClickSelection(selectionService, clickDesc);
}

bool EditorViewportSelectionModel::ApplyClickSelection(
    EditorContext& context,
    const EditorViewportSelectionPickDesc& desc)
{
    EditorSelectionService selectionService(context);
    return ApplyClickSelection(selectionService, desc);
}

bool EditorViewportSelectionModel::ApplyClickSelection(
    EditorSelectionService& selectionService,
    const EditorViewportSelectionPickDesc& desc)
{
    const bool blockedByManipulator = m_lastStats.blockedByManipulator;
    m_lastStats = {};
    m_lastStats.blockedByManipulator = blockedByManipulator;
    m_lastStats.receivedClick = true;
    m_lastStats.cameraValid = desc.cameraFrame.valid;

    SceneManager* sceneManager = desc.sceneManager ? desc.sceneManager
                                                   : selectionService.GetActiveSceneManager();
    m_lastStats.hasScene = sceneManager != nullptr;
    if (!sceneManager || !desc.cameraFrame.valid)
    {
        return false;
    }

    const EditorViewportScreenRay screenRay =
        EditorViewportProjection::BuildScreenRay(desc.cameraFrame,
                                                 desc.screenPosition);
    m_lastStats.rayValid = screenRay.valid;
    m_lastStats.insideViewport = screenRay.insideViewport;
    if (!screenRay.valid || !screenRay.insideViewport)
    {
        return false;
    }

    RaycastHit hit;
    if (sceneManager->Raycast(screenRay.ray, hit) && hit.entity)
    {
        m_lastStats.hit = true;
        m_lastStats.hitEntity = hit.entity;
        m_lastStats.hitDistance = hit.distance;
        selectionService.SelectEntity(hit.entity);
        m_lastStats.selected =
            selectionService.GetSelectedEntity() == hit.entity;
        return true;
    }

    if (desc.clearSelectionOnMiss)
    {
        m_lastStats.clearedSelection = selectionService.ClearSelection();
        return true;
    }

    return false;
}

} // namespace RVX::Editor
