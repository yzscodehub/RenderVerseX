/**
 * @file EditorServiceRegistry.cpp
 * @brief Editor service registry implementation
 */

#include "Editor/EditorServiceRegistry.h"

#include <algorithm>

namespace RVX::Editor
{

void EditorServiceRegistry::Clear()
{
    m_services.clear();
}

uint32 EditorServiceRegistry::GetServiceCount() const
{
    return static_cast<uint32>(m_services.size());
}

std::vector<std::string> EditorServiceRegistry::GetRegisteredServiceNames() const
{
    std::vector<std::string> names;
    names.reserve(m_services.size());
    for (const auto& [type, entry] : m_services)
    {
        (void)type;
        names.push_back(entry.debugName);
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace RVX::Editor
