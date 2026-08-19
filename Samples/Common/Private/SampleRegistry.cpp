/** @file SampleRegistry.cpp @brief SampleRegistry implementation. */

#include "Samples/SampleRegistry.h"

#include <utility>

namespace RVX
{
    bool SampleRegistry::Register(SampleInfo info,
                                  SampleFactory factory,
                                  std::string* outError)
    {
        if (info.id.empty())
        {
            if (outError)
            {
                *outError = "Sample id must not be empty";
            }
            return false;
        }
        if (!factory)
        {
            if (outError)
            {
                *outError = "Sample factory must not be empty: " + info.id;
            }
            return false;
        }
        if (m_entries.contains(info.id))
        {
            if (outError)
            {
                *outError = "Duplicate sample id: " + info.id;
            }
            return false;
        }

        const std::string id = info.id;
        m_entries.emplace(id, Entry{std::move(info), std::move(factory)});
        return true;
    }

    std::unique_ptr<ISample> SampleRegistry::Create(std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : it->second.factory();
    }

    const SampleInfo* SampleRegistry::Find(std::string_view id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second.info;
    }

    std::vector<SampleInfo> SampleRegistry::List() const
    {
        std::vector<SampleInfo> result;
        result.reserve(m_entries.size());
        for (const auto& [id, entry] : m_entries)
        {
            static_cast<void>(id);
            result.push_back(entry.info);
        }
        return result;
    }
} // namespace RVX
