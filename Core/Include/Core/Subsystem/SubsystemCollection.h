#pragma once

/**
 * @file SubsystemCollection.h
 * @brief Template container for managing subsystems
 */

#include "Core/Subsystem/ISubsystem.h"
#include "Core/Log.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <algorithm>
#include <string>
#include <queue>

namespace RVX
{
    /**
     * @brief Template container for managing subsystems
     * 
     * This collection manages the lifecycle of subsystems:
     * - Adding/removing subsystems
     * - Initialization order based on dependencies
     * - Per-frame ticking
     * - Shutdown in reverse order
     * 
     * @tparam TBase Base subsystem type (EngineSubsystem or WorldSubsystem)
     */
    template<typename TBase>
    class SubsystemCollection
    {
    public:
        SubsystemCollection() = default;
        ~SubsystemCollection() { DeinitializeAll(); }

        // Non-copyable
        SubsystemCollection(const SubsystemCollection&) = delete;
        SubsystemCollection& operator=(const SubsystemCollection&) = delete;

        // Movable
        SubsystemCollection(SubsystemCollection&&) = default;
        SubsystemCollection& operator=(SubsystemCollection&&) = default;

        /**
         * @brief Add a subsystem to the collection
         * @return Pointer to the added subsystem
         */
        template<typename T, typename... Args>
        T* AddSubsystem(Args&&... args)
        {
            static_assert(std::is_base_of_v<TBase, T>, 
                "T must derive from the base subsystem type");

            auto subsystem = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = subsystem.get();

            auto typeIndex = std::type_index(typeid(T));
            m_lookup[typeIndex] = ptr;
            m_nameLookup[ptr->GetName()] = ptr;
            m_subsystems.push_back(std::move(subsystem));
            m_orderDirty = true;

            return ptr;
        }

        /**
         * @brief Get a subsystem by type
         * @return Pointer to subsystem or nullptr if not found
         */
        template<typename T>
        T* GetSubsystem() const
        {
            auto typeIndex = std::type_index(typeid(T));
            auto it = m_lookup.find(typeIndex);
            if (it != m_lookup.end())
            {
                return static_cast<T*>(it->second);
            }
            return nullptr;
        }

        /**
         * @brief Get a subsystem by name
         * @return Pointer to subsystem or nullptr if not found
         */
        TBase* GetSubsystem(const std::string& name) const
        {
            auto it = m_nameLookup.find(name);
            if (it != m_nameLookup.end())
            {
                return it->second;
            }
            return nullptr;
        }

        /**
         * @brief Check if a subsystem exists
         */
        template<typename T>
        bool HasSubsystem() const
        {
            return GetSubsystem<T>() != nullptr;
        }

        /**
         * @brief Initialize all subsystems in dependency order
         */
        void InitializeAll()
        {
            BuildOrder();

            for (auto* subsystem : m_ordered)
            {
                RVX_CORE_DEBUG("Initializing subsystem: {}", subsystem->GetName());
                subsystem->Initialize();
            }

            m_initialized = true;
        }

        /**
         * @brief Deinitialize all subsystems in reverse order
         */
        void DeinitializeAll()
        {
            if (!m_initialized)
                return;

            // Shutdown in reverse order
            for (auto it = m_ordered.rbegin(); it != m_ordered.rend(); ++it)
            {
                RVX_CORE_DEBUG("Deinitializing subsystem: {}", (*it)->GetName());
                (*it)->Deinitialize();
            }

            m_initialized = false;
        }

        /**
         * @brief Tick all subsystems that need updating
         */
        void TickAll(float deltaTime)
        {
            for (auto* subsystem : m_ordered)
            {
                if (subsystem->ShouldTick())
                {
                    subsystem->Tick(deltaTime);
                }
            }
        }

        /**
         * @brief Get all subsystems
         */
        const std::vector<std::unique_ptr<TBase>>& GetAll() const
        {
            return m_subsystems;
        }

        /**
         * @brief Get subsystem count
         */
        size_t GetCount() const
        {
            return m_subsystems.size();
        }

        /**
         * @brief Clear all subsystems
         */
        void Clear()
        {
            DeinitializeAll();
            m_ordered.clear();
            m_lookup.clear();
            m_nameLookup.clear();
            m_subsystems.clear();
        }

    private:
        void BuildOrder()
        {
            if (!m_orderDirty)
                return;

            m_ordered.clear();

            // Build dependency graph
            std::unordered_map<std::string, int> indegree;
            std::unordered_map<std::string, std::vector<std::string>> dependents;

            for (const auto& subsystem : m_subsystems)
            {
                indegree[subsystem->GetName()] = 0;
            }

            for (const auto& subsystem : m_subsystems)
            {
                int depCount = 0;
                const char** deps = subsystem->GetDependencies(depCount);
                
                for (int i = 0; i < depCount; ++i)
                {
                    if (deps[i])
                    {
                        dependents[deps[i]].push_back(subsystem->GetName());
                        indegree[subsystem->GetName()]++;
                    }
                }
            }

            // Topological sort
            std::queue<std::string> queue;
            for (const auto& [name, degree] : indegree)
            {
                if (degree == 0)
                {
                    queue.push(name);
                }
            }

            std::vector<std::string> sortedNames;
            while (!queue.empty())
            {
                auto name = queue.front();
                queue.pop();
                sortedNames.push_back(name);

                for (const auto& dependent : dependents[name])
                {
                    if (--indegree[dependent] == 0)
                    {
                        queue.push(dependent);
                    }
                }
            }

            // Check for cycles
            if (sortedNames.size() != m_subsystems.size())
            {
                RVX_CORE_WARN("Subsystem dependency cycle detected, using registration order");
                for (const auto& subsystem : m_subsystems)
                {
                    m_ordered.push_back(subsystem.get());
                }
            }
            else
            {
                for (const auto& name : sortedNames)
                {
                    auto it = m_nameLookup.find(name);
                    if (it != m_nameLookup.end())
                    {
                        m_ordered.push_back(it->second);
                    }
                }
            }

            m_orderDirty = false;
        }

        std::vector<std::unique_ptr<TBase>> m_subsystems;
        std::unordered_map<std::type_index, TBase*> m_lookup;
        std::unordered_map<std::string, TBase*> m_nameLookup;
        std::vector<TBase*> m_ordered;
        bool m_orderDirty = true;
        bool m_initialized = false;
    };

} // namespace RVX
