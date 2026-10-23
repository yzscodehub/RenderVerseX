#include "Resource/PreparedResourceBundle.h"

#include <sstream>

namespace RVX::Resource
{
bool PreparedResourceBundle::AddDependency(ResourceHandle<IResource> resource)
{
    return Add(std::move(resource), false);
}

bool PreparedResourceBundle::SetRoot(ResourceHandle<IResource> resource)
{
    return Add(std::move(resource), true);
}

bool PreparedResourceBundle::SetObservedContentIdentity(ResourceContentIdentity identity)
{
    if (m_observedContentIdentity.IsValid() || !identity.IsValid())
    {
        return false;
    }

    m_observedContentIdentity = std::move(identity);
    return true;
}

bool PreparedResourceBundle::IsValid() const
{
    if (!m_root || m_entries.empty())
    {
        return false;
    }

    uint32 rootCount = 0;
    for (const PreparedResourceEntry& entry : m_entries)
    {
        if (!entry.resource || entry.resource->GetId() == InvalidResourceId ||
            entry.resource->GetType() == ResourceType::Unknown)
        {
            return false;
        }
        rootCount += entry.isRoot ? 1u : 0u;
    }

    return rootCount == 1 && m_root.GetId() != InvalidResourceId;
}

std::string PreparedResourceBundle::GetValidationError() const
{
    if (!m_root)
    {
        return "Prepared resource bundle has no root resource.";
    }
    if (m_entries.empty())
    {
        return "Prepared resource bundle has no entries.";
    }

    uint32 rootCount = 0;
    for (const PreparedResourceEntry& entry : m_entries)
    {
        if (!entry.resource)
        {
            return "Prepared resource bundle contains a null resource.";
        }
        if (entry.resource->GetId() == InvalidResourceId)
        {
            return "Prepared resource bundle contains a resource without an id.";
        }
        if (entry.resource->GetType() == ResourceType::Unknown)
        {
            return "Prepared resource bundle contains a resource with an unknown type.";
        }
        rootCount += entry.isRoot ? 1u : 0u;
    }
    if (rootCount != 1)
    {
        return "Prepared resource bundle must contain exactly one root.";
    }
    if (m_root.GetId() == InvalidResourceId)
    {
        return "Prepared resource bundle root has an invalid id.";
    }
    return {};
}

bool PreparedResourceBundle::Add(ResourceHandle<IResource> resource, bool isRoot)
{
    if (!resource || resource->GetId() == InvalidResourceId ||
        resource->GetType() == ResourceType::Unknown)
    {
        return false;
    }
    if (isRoot && m_root)
    {
        return false;
    }
    if (!m_resourceIds.insert(resource.GetId()).second)
    {
        return false;
    }

    if (isRoot)
    {
        m_root = resource;
    }
    m_entries.push_back({std::move(resource), isRoot});
    return true;
}
} // namespace RVX::Resource
