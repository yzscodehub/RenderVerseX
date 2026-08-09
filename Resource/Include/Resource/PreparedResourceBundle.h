#pragma once

/**
 * @file PreparedResourceBundle.h
 * @brief Worker-produced, owner-thread-published resource transaction input.
 */

#include "Resource/IResource.h"
#include "Resource/ResourceHandle.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace RVX::Resource
{
    /** @brief One resource prepared without cache, registry, scene or render access. */
    struct PreparedResourceEntry
    {
        ResourceHandle<IResource> resource;
        bool isRoot = false;
    };

    /**
     * @brief Immutable-at-publication bundle produced entirely by a loader worker.
     *
     * Entries are stored dependency-first.  The ResourceManager validates the
     * complete bundle before it changes any owner-thread registry or cache state;
     * only the marked root is published after all dependencies.
     */
    class PreparedResourceBundle
    {
    public:
        bool AddDependency(ResourceHandle<IResource> resource);
        bool SetRoot(ResourceHandle<IResource> resource);
        bool IsValid() const;
        bool IsEmpty() const { return m_entries.empty(); }
        bool Contains(ResourceId resourceId) const
        {
            return m_resourceIds.contains(resourceId);
        }

        [[nodiscard]] const std::vector<PreparedResourceEntry>& GetEntries() const
        {
            return m_entries;
        }

        [[nodiscard]] ResourceHandle<IResource> GetRoot() const
        {
            return m_root;
        }

        [[nodiscard]] std::string GetValidationError() const;

    private:
        bool Add(ResourceHandle<IResource> resource, bool isRoot);

        std::vector<PreparedResourceEntry> m_entries;
        ResourceHandle<IResource> m_root;
        std::unordered_set<ResourceId> m_resourceIds;
    };
} // namespace RVX::Resource
