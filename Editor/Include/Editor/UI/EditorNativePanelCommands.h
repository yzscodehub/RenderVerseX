/**
 * @file EditorNativePanelCommands.h
 * @brief Native editor panel command helpers.
 */

#pragma once

#include <string>
#include <string_view>

namespace RVX::Editor
{

std::string MakeNativeUIPanelViewCommandId(std::string_view panelId);

} // namespace RVX::Editor
