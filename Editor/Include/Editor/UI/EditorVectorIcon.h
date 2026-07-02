/**
 * @file EditorVectorIcon.h
 * @brief Semantic vector icon drawing for native editor controls
 */

#pragma once

#include "Core/Types.h"
#include "UI/UITypes.h"

#include <string>

namespace RVX::UI
{
    class UIRenderer;
}

namespace RVX::Editor
{

struct EditorVectorIconDrawDesc
{
    std::string name;
    UI::Rect bounds;
    UI::UIColor color{0.90f, 0.92f, 0.94f, 1.0f};
    std::string fallbackLabel;
    float fallbackFontSize = 14.0f;
};

struct EditorVectorIconDrawStats
{
    bool knownIcon = false;
    uint32 lineCount = 0;
    uint32 rectCount = 0;
    uint32 borderCount = 0;
    uint32 textFallbackCount = 0;
};

class EditorVectorIconLibrary
{
public:
    // =========================================================================
    // Public Methods
    // =========================================================================
    static bool IsKnownIcon(const std::string& name);
    static float GetStrokeWidth(float iconSize);
    static EditorVectorIconDrawStats Draw(UI::UIRenderer& renderer,
                                          const EditorVectorIconDrawDesc& desc);
};

} // namespace RVX::Editor
