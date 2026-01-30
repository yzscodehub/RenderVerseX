#pragma once

/**
 * @file SubsystemCollection.h
 * @brief Template container for managing subsystems
 * 
 * Enhanced with:
 * - Type-safe dependency resolution
 * - Cycle detection with detailed reporting
 * - Tick phase ordering
 * - Service auto-registration
 */

#include "Core/Subsystem/ISubsystem.h"
#include "Core/Log.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <typeindex>
#include <algorithm>
#include <string>
#include <queue>
#include <sstream>

namespace RVX
{
    /**
     * @brief Result of dependency validation
     */
    struct DependencyValidationResult
    {
        bool valid = true;
        std::vector<std::string> missingDependencies;
        std::vector<std::string> cyclePath;  // If cycle detected
        
        std::string GetErrorMessage() const
        {
            std::stringstream ss;
            if (!missingDependencies.empty())
            {
                ss << "Missing dependencies: ";
                for (size_t i = 0; i < missingDependencies.size(); ++i)
                {
                    if (i > 0) ss << ", ";
                    ss << missingDependencies[i];
                }
            }
            if (!cyclePath.empty())
            {
                if (!missingDependencies.empty()) ss << "; ";
                ss << "Dependency cycle: ";
                for (size_t i = 0; i < cyclePath.size(); ++i)
                {
                    if (i > 0) ss << " -> ";
                    ss << cyclePath[i];
                }
            }
            return ss.str();
        }
    };

    /**
     * @brief Template container for managing subsystems
     * 
     * This collection manages the lifecycle of subsystems:
     * - Adding/removing subsystems
     * - Initialization order based on dependencies
     * - Per-frame ticking with phase support
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

            // Check if already exists
            auto typeIndex = std::type_index(typeid(T));
            if (m_lookup.find(typeIndex) != m_lookup.end())
            {
                RVX_CORE_WARN("Subsystem {} already exists, returning existing instance", 
                              typeid(T).name());
                return static_cast<T*>(m_lookup[typeIndex]);
            }

            auto subsystem = std::make_unique<T>(std::forward<Args>(args)...);
            T* ptr = subsystem.get();

            m_lookup[typeIndex] = ptr;
            m_typeLookup.insert_or_assign(ptr, typeIndex);
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
         * @brief Validate dependencies before initialization
         */
        DependencyValidationResult ValidateDependencies() const
        {
            DependencyValidationResult result;
            
            for (const auto& subsystem : m_subsystems)
            {
                // Check typed dependencies
                auto deps = subsystem->GetTypedDependencies();
                for (const auto& dep : deps)
                {
                    if (m_lookup.find(dep.typeIndex) == m_lookup.end())
                    {
                        if (!dep.optional)
                        {
                            result.valid = false;
                            result.missingDependencies.push_back(
                                std::string(subsystem->GetName()) + " requires " + dep.name);
                        }
                    }
                }

                // Check legacy string dependencies
                int depCount = 0;
                const char** deps2 = subsystem->GetDependencies(depCount);
                for (int i = 0; i < depCount; ++i)
                {
                    if (deps2[i] && m_nameLookup.find(deps2[i]) == m_nameLookup.end())
                    {
                        result.valid = false;
                        result.missingDependencies.push_back(
                            std::string(subsystem->GetName()) + " requires " + deps2[i]);
                    }
                }
            }

            // Check for cycles
            auto cycleResult = DetectCycles();
            if (!cycleResult.empty())
            {
                result.valid = false;
                result.cyclePath = cycleResult;
            }

            return result;
        }

        /**
         * @brief Initialize all subsystems in dependency order
         */
        void InitializeAll()
        {
            // Validate first
            auto validation = ValidateDependencies();
            if (!validation.valid)
            {
                RVX_CORE_ERROR("Subsystem dependency validation failed: {}", 
                               validation.GetErrorMessage());
            }

            BuildOrder();

            for (auto* subsystem : m_ordered)
            {
                RVX_CORE_DEBUG("Initializing subsystem: {}", subsystem->GetName());
                subsystem->Initialize();
                subsystem->SetInitialized(true);
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
                (*it)->SetInitialized(false);
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
         * @brief Tick subsystems in a specific phase
         */
        void TickPhase(TickPhase phase, float deltaTime)
        {
            for (auto* subsystem : m_ordered)
            {
                if (subsystem->ShouldTick() && subsystem->GetTickPhase() == phase)
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
         * @brief Get ordered subsystems (after BuildOrder)
         */
        const std::vector<TBase*>& GetOrdered() const
        {
            return m_ordered;
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
            m_typeLookup.clear();
            m_nameLookup.clear();
            m_subsystems.clear();
        }

        /**
         * @brief Check if collection is initialized
         */
        bool IsInitialized() const { return m_initialized; }

    private:
        std::vector<std::string> DetectCycles() const
        {
            std::unordered_map<std::string, int> color; // 0=white, 1=gray, 2=black
            std::vector<std::string> cyclePath;
            
            for (const auto& subsystem : m_subsystems)
            {
                color[subsystem->GetName()] = 0;
            }

            std::function<bool(const std::string&, std::vector<std::string>&)> dfs;
            dfs = [&](const std::string& name, std::vector<std::string>& path) -> bool {
                color[name] = 1; // Gray
                path.push_back(name);

                auto subsystem = GetSubsystem(name);
                if (subsystem)
                {
                    // Check typed dependencies
                    auto deps = subsystem->GetTypedDependencies();
                    for (const auto& dep : deps)
                    {
                        auto it = m_lookup.find(dep.typeIndex);
                        if (it != m_lookup.end())
                        {
                            const std::string& depName = it->second->GetName();
                            if (color[depName] == 1) // Gray = cycle
                            {
                                path.push_back(depName);
                                return true;
                            }
                            if (color[depName] == 0 && dfs(depName, path))
                            {
                                return true;
                            }
                        }
                    }

                    // Check legacy dependencies
                    int depCount = 0;
                    const char** deps2 = subsystem->GetDependencies(depCount);
                    for (int i = 0; i < depCount; ++i)
                    {
                        if (deps2[i])
                        {
                            if (color[deps2[i]] == 1)
                            {
                                path.push_back(deps2[i]);
                                return true;
                            }
                            if (color[deps2[i]] == 0 && dfs(deps2[i], path))
                            {
                                return true;
                            }
                        }
                    }
                }

                path.pop_back();
                color[name] = 2; // Black
                return false;
            };

            for (const auto& subsystem : m_subsystems)
            {
                if (color[subsystem->GetName()] == 0)
                {
                    std::vector<std::string> path;
                    if (dfs(subsystem->GetName(), path))
                    {
                        return path;
                    }
                }
            }

            return {};
        }

        void BuildOrder()
        {
            if (!m_orderDirty)
                return;

            m_ordered.clear();

            // Build dependency graph using both typed and string dependencies
            std::unordered_map<std::string, int> indegree;
            std::unordered_map<std::string, std::vector<std::string>> dependents;

            for (const auto& subsystem : m_subsystems)
            {
                indegree[subsystem->GetName()] = 0;
            }

            for (const auto& subsystem : m_subsystems)
            {
                // Process typed dependencies
                auto deps = subsystem->GetTypedDependencies();
                for (const auto& dep : deps)
                {
                    auto it = m_lookup.find(dep.typeIndex);
                    if (it != m_lookup.end())
                    {
                        dependents[it->second->GetName()].push_back(subsystem->GetName());
                        indegree[subsystem->GetName()]++;
                    }
                }

                // Process legacy string dependencies
                int depCount = 0;
                const char** deps2 = subsystem->GetDependencies(depCount);
                for (int i = 0; i < depCount; ++i)
                {
                    if (deps2[i] && m_nameLookup.find(deps2[i]) != m_nameLookup.end())
                    {
                        dependents[deps2[i]].push_back(subsystem->GetName());
                        indegree[subsystem->GetName()]++;
                    }
                }
            }

            // Topological sort (Kahn's algorithm)
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
                auto cyclePath = DetectCycles();
                std::stringstream cycleStr;
                for (size_t i = 0; i < cyclePath.size(); ++i)
                {
                    if (i > 0) cycleStr << " -> ";
                    cycleStr << cyclePath[i];
                }
                RVX_CORE_ERROR("Subsystem dependency cycle detected: {}", cycleStr.str());
                RVX_CORE_WARN("Falling back to registration order");
                
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
        std::unordered_map<TBase*, std::type_index> m_typeLookup;
        std::unordered_map<std::string, TBase*> m_nameLookup;
        std::vector<TBase*> m_ordered;
        bool m_orderDirty = true;
        bool m_initialized = false;
    };

} // namespace RVX
