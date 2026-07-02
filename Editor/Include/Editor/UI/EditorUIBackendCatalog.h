/**
 * @file EditorUIBackendCatalog.h
 * @brief Catalog of selectable editor UI backend types.
 */

#pragma once

#include "Core/Types.h"

#include <span>
#include <string_view>

namespace RVX::Editor
{

enum class EditorUIBackendType : uint8
{
    Native = 0,
};

struct EditorUIBackendDescriptor
{
    EditorUIBackendType type = EditorUIBackendType::Native;
    std::string_view displayName;
    std::string_view configName;
    std::string_view description;
    bool canCreate = false;
};

std::span<const EditorUIBackendDescriptor> GetEditorUIBackendCatalog();
const EditorUIBackendDescriptor* FindEditorUIBackendDescriptor(
    EditorUIBackendType type);
const EditorUIBackendDescriptor* FindEditorUIBackendDescriptor(
    std::string_view configName);
EditorUIBackendType GetDefaultEditorUIBackendType();
const char* ToString(EditorUIBackendType type);
const char* ToConfigString(EditorUIBackendType type);
bool TryParseEditorUIBackendType(std::string_view value,
                                 EditorUIBackendType& outType);

} // namespace RVX::Editor
