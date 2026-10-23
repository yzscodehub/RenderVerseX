#pragma once

/** @file ResourceChangeMonitor.h @brief Source-change observation contract */

#include "AssetContracts/ResourceTypes.h"

#include <string>
#include <vector>

namespace RVX::Resource
{
    struct ResourceSourceChange
    {
        ResourceId resourceId = InvalidResourceId;
        std::string path;
    };

    /**
     * @brief Reports source changes without performing resource reload work.
     *
     * Implementations own only observation state. ResourceManager remains the
     * sole owner of cache mutation, reload transactions, and lifecycle events.
     */
    class IResourceChangeMonitor
    {
    public:
        virtual ~IResourceChangeMonitor() = default;

        virtual void Watch(ResourceId resourceId, std::string path) = 0;
        virtual void Unwatch(ResourceId resourceId) = 0;
        virtual std::vector<ResourceSourceChange> Poll() = 0;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourceChangeMonitor;
    using Resource::ResourceSourceChange;
} // namespace RVX
