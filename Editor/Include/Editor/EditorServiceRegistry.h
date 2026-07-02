/**
 * @file EditorServiceRegistry.h
 * @brief Non-owning typed registry for editor infrastructure services.
 */

#pragma once

#include "Core/Types.h"

#include <string>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <vector>

namespace RVX::Editor
{

/**
 * @brief Non-owning typed registry for editor service boundaries.
 *
 * Services keep their existing owners. The registry gives editor systems a
 * single typed lookup path without forcing every service to inherit a common
 * interface up front.
 */
class EditorServiceRegistry final
{
public:
    // =========================================================================
    // Registration
    // =========================================================================

    template <typename ServiceT>
    void Register(ServiceT& service, std::string debugName = {})
    {
        ServiceEntry entry;
        entry.instance = &service;
        entry.debugName = debugName.empty() ? typeid(ServiceT).name()
                                           : std::move(debugName);
        m_services[std::type_index(typeid(ServiceT))] = std::move(entry);
    }

    template <typename ServiceT>
    bool Unregister()
    {
        return m_services.erase(std::type_index(typeid(ServiceT))) > 0;
    }

    void Clear();

    // =========================================================================
    // Lookup
    // =========================================================================

    template <typename ServiceT>
    ServiceT* Get()
    {
        auto it = m_services.find(std::type_index(typeid(ServiceT)));
        if (it == m_services.end())
        {
            return nullptr;
        }
        return static_cast<ServiceT*>(it->second.instance);
    }

    template <typename ServiceT>
    const ServiceT* Get() const
    {
        auto it = m_services.find(std::type_index(typeid(ServiceT)));
        if (it == m_services.end())
        {
            return nullptr;
        }
        return static_cast<const ServiceT*>(it->second.instance);
    }

    template <typename ServiceT>
    bool Contains() const
    {
        return Get<ServiceT>() != nullptr;
    }

    uint32 GetServiceCount() const;
    std::vector<std::string> GetRegisteredServiceNames() const;

private:
    struct ServiceEntry
    {
        void* instance = nullptr;
        std::string debugName;
    };

    std::unordered_map<std::type_index, ServiceEntry> m_services;
};

} // namespace RVX::Editor
