/**
 * @file Editor.h
 * @brief Editor application main header
 */

#pragma once

#include "Editor/EditorWindow.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorTheme.h"
#include "Editor/EditorApplication.h"
#include "Editor/Panels/IEditorPanel.h"
#include "Editor/Panels/SceneHierarchy.h"
#include "Editor/Panels/Inspector.h"
#include "Editor/Panels/AssetBrowser.h"
#include "Editor/Panels/Viewport.h"
#include "Editor/Panels/Console.h"
#include "Editor/Panels/AnimationEditor.h"
#include "Editor/Panels/MaterialEditor.h"

namespace RVX::Editor
{

/**
 * @brief Editor version info
 */
struct EditorInfo
{
    static constexpr const char* kName = "RenderVerseX Editor";
    static constexpr int kMajorVersion = 0;
    static constexpr int kMinorVersion = 1;
};

} // namespace RVX::Editor
