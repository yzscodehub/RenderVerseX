/**
 * @file EditorNativePanelCommands.cpp
 * @brief Native editor panel command helpers.
 */

#include "Editor/UI/EditorNativePanelCommands.h"

#include <cctype>

namespace RVX::Editor
{

std::string MakeNativeUIPanelViewCommandId(std::string_view panelId)
{
    if (panelId.empty())
    {
        return {};
    }

    std::string id = "view.nativePanel.";
    bool lastWasSeparator = false;
    for (unsigned char character : panelId)
    {
        if (std::isalnum(character) != 0)
        {
            id.push_back(static_cast<char>(std::tolower(character)));
            lastWasSeparator = false;
            continue;
        }

        if (!lastWasSeparator)
        {
            id.push_back('.');
            lastWasSeparator = true;
        }
    }

    while (!id.empty() && id.back() == '.')
    {
        id.pop_back();
    }
    return id == "view.nativePanel" ? std::string{} : id;
}

} // namespace RVX::Editor
