/**
 * @file EditorCommandIconResolver.h
 * @brief Central command id to native editor icon mapping
 */

#pragma once

#include <string>

namespace RVX::Editor
{

class EditorCommandIconResolver
{
public:
    static std::string ResolveIconName(const std::string& commandId,
                                       const std::string& explicitIconName = {});
    static std::string ResolveCommandIconName(const std::string& commandId);
};

} // namespace RVX::Editor
