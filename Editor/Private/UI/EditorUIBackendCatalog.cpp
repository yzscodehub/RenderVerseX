/**
 * @file EditorUIBackendCatalog.cpp
 * @brief Catalog of selectable editor UI backend types.
 */

#include "Editor/UI/EditorUIBackendCatalog.h"

#include <array>
#include <cctype>

namespace RVX::Editor
{
namespace
{
    constexpr std::array<EditorUIBackendDescriptor, 1> RVX_EDITOR_UI_BACKENDS = {{
        {
            EditorUIBackendType::Native,
            "Native",
            "native",
            "RenderVerseX native retained-mode editor UI",
            true,
        },
    }};

    std::string_view TrimAsciiWhitespace(std::string_view value)
    {
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front())))
        {
            value.remove_prefix(1);
        }
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back())))
        {
            value.remove_suffix(1);
        }
        return value;
    }

    bool EqualsCaseInsensitive(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t index = 0; index < lhs.size(); ++index)
        {
            const int lhsChar =
                std::tolower(static_cast<unsigned char>(lhs[index]));
            const int rhsChar =
                std::tolower(static_cast<unsigned char>(rhs[index]));
            if (lhsChar != rhsChar)
            {
                return false;
            }
        }

        return true;
    }
} // namespace

std::span<const EditorUIBackendDescriptor> GetEditorUIBackendCatalog()
{
    return RVX_EDITOR_UI_BACKENDS;
}

const EditorUIBackendDescriptor* FindEditorUIBackendDescriptor(
    EditorUIBackendType type)
{
    for (const EditorUIBackendDescriptor& descriptor :
         GetEditorUIBackendCatalog())
    {
        if (descriptor.type == type)
        {
            return &descriptor;
        }
    }

    return nullptr;
}

const EditorUIBackendDescriptor* FindEditorUIBackendDescriptor(
    std::string_view configName)
{
    configName = TrimAsciiWhitespace(configName);
    for (const EditorUIBackendDescriptor& descriptor :
         GetEditorUIBackendCatalog())
    {
        if (EqualsCaseInsensitive(configName, descriptor.configName))
        {
            return &descriptor;
        }
    }

    return nullptr;
}

EditorUIBackendType GetDefaultEditorUIBackendType()
{
    return EditorUIBackendType::Native;
}

const char* ToString(EditorUIBackendType type)
{
    if (const EditorUIBackendDescriptor* descriptor =
            FindEditorUIBackendDescriptor(type))
    {
        return descriptor->displayName.data();
    }

    return "Unknown";
}

const char* ToConfigString(EditorUIBackendType type)
{
    if (const EditorUIBackendDescriptor* descriptor =
            FindEditorUIBackendDescriptor(type))
    {
        return descriptor->configName.data();
    }

    return "unknown";
}

bool TryParseEditorUIBackendType(std::string_view value,
                                 EditorUIBackendType& outType)
{
    if (const EditorUIBackendDescriptor* descriptor =
            FindEditorUIBackendDescriptor(value))
    {
        outType = descriptor->type;
        return true;
    }

    return false;
}

} // namespace RVX::Editor
