/**
 * @file EditorUIPanel.h
 * @brief Native editor UI panel contract
 */

#pragma once

#include "Core/Types.h"
#include "Editor/UI/EditorUIBackendTypes.h"
#include "UI/UIContext.h"

#include <string>

namespace RVX::UI
{
    class Panel;
}

namespace RVX::Editor
{

class EditorUIHost;

enum class EditorUIPanelDockArea : uint8
{
    Center = 0,
    Left,
    Right,
    Bottom,
    Floating
};

struct EditorUIPanelDesc
{
    std::string id;
    std::string title;
    EditorUIPanelDockArea defaultDockArea = EditorUIPanelDockArea::Floating;
    bool visibleByDefault = true;
    bool closable = true;
};

enum class EditorUIPanelRebuildReason : uint32
{
    None = 0,
    Initial = 1u << 0u,
    Explicit = 1u << 1u,
    Visibility = 1u << 2u,
    Layout = 1u << 3u,
    Docking = 1u << 4u,
    Data = 1u << 5u
};

constexpr uint32 ToEditorUIPanelRebuildReasonMask(
    EditorUIPanelRebuildReason reason)
{
    return static_cast<uint32>(reason);
}

inline bool HasEditorUIPanelRebuildReason(
    uint32 mask,
    EditorUIPanelRebuildReason reason)
{
    return (mask & ToEditorUIPanelRebuildReasonMask(reason)) != 0u;
}

struct EditorUIPanelFrameContext
{
    UI::UIContext* ui = nullptr;
    UI::UICanvas* canvas = nullptr;
    EditorUIHost* host = nullptr;
    UI::Panel* panelContainer = nullptr;
    UI::Panel* contentContainer = nullptr;
    const UI::UIInputState* input = nullptr;
    UI::Rect panelBounds;
    UI::Rect contentBounds;
    std::string panelId;
    float deltaTime = 0.0f;
    bool isHovered = false;
    bool isActive = false;
    bool hasKeyboardFocus = false;
    bool hasMouseCapture = false;
    bool shouldRebuildContent = false;
    uint64 rebuildRevision = 0;
    uint32 rebuildReasonMask = 0;
};

class IEditorUIPanel
{
public:
    virtual ~IEditorUIPanel() = default;

    virtual const EditorUIPanelDesc& GetPanelDesc() const = 0;
    virtual void OnAttach(EditorUIHost& host) { (void)host; }
    virtual void OnDetach() {}
    virtual void OnUpdate(float deltaTime) { (void)deltaTime; }
    virtual void BuildUI(EditorUIPanelFrameContext& context) = 0;
    virtual EditorUICursorRequest GetCursorRequest() const { return {}; }
};

} // namespace RVX::Editor
