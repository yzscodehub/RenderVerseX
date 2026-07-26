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

#include "Core/Log.h"
#include "Core/Subsystem/ISubsystem.h"
#include "Core/Types.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

    /** @brief Result code for registering a composition-owned dependency edge */
    enum class SubsystemDependencyRegistrationCode : uint8
    {
        Added = 0,
        AlreadyRegistered,
        MissingDependent,
        MissingPrerequisite,
        SelfDependency,
        LifecycleActive
    };

    /** @brief Diagnostic result for composition dependency registration */
    struct SubsystemDependencyRegistrationResult
    {
        SubsystemDependencyRegistrationCode code =
            SubsystemDependencyRegistrationCode::MissingDependent;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == SubsystemDependencyRegistrationCode::Added ||
                   code ==
                       SubsystemDependencyRegistrationCode::AlreadyRegistered;
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
        using InitializeHook = std::function<void(TBase&)>;

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
         * @brief Add a dependency owned by the current subsystem composition
         *
         * The dependent and prerequisite must already be registered. The
         * edge affects initialization order only for this collection and does
         * not change either subsystem's intrinsic dependency declaration.
         */
        template<typename TDependent, typename TPrerequisite>
        [[nodiscard]] SubsystemDependencyRegistrationResult
        AddInitializationDependency()
        {
            static_assert(
                std::is_base_of_v<TBase, TDependent>,
                "TDependent must derive from the base subsystem type");
            static_assert(
                std::is_base_of_v<TBase, TPrerequisite>,
                "TPrerequisite must derive from the base subsystem type");

            const bool lifecycleActive =
                m_initialized ||
                std::any_of(
                    m_subsystems.begin(),
                    m_subsystems.end(),
                    [](const std::unique_ptr<TBase>& subsystem) {
                        return subsystem->IsInitialized();
                    });
            if (lifecycleActive)
            {
                return {
                    SubsystemDependencyRegistrationCode::LifecycleActive};
            }

            const std::type_index dependentType(typeid(TDependent));
            const std::type_index prerequisiteType(typeid(TPrerequisite));
            if (dependentType == prerequisiteType)
            {
                return {
                    SubsystemDependencyRegistrationCode::SelfDependency};
            }
            if (m_lookup.find(dependentType) == m_lookup.end())
            {
                return {
                    SubsystemDependencyRegistrationCode::MissingDependent};
            }
            if (m_lookup.find(prerequisiteType) == m_lookup.end())
            {
                return {
                    SubsystemDependencyRegistrationCode::MissingPrerequisite};
            }

            auto& prerequisites =
                m_compositionDependencies[dependentType];
            const auto [iterator, inserted] =
                prerequisites.insert(prerequisiteType);
            (void)iterator;
            if (!inserted)
            {
                return {
                    SubsystemDependencyRegistrationCode::AlreadyRegistered};
            }

            m_orderDirty = true;
            return {SubsystemDependencyRegistrationCode::Added};
        }

        /**
         * @brief Validate dependencies before initialization
         */
        DependencyValidationResult ValidateDependencies() const
        {
            return ValidateDependencyGraph(BuildDependencyGraph());
        }

        /**
         * @brief Initialize all subsystems in dependency order
         */
        bool InitializeAll(const InitializeHook& beforeInitialize = {},
                           const InitializeHook& afterInitialize = {})
        {
            if (m_initialized)
            {
                return true;
            }

            // Build and validate one graph snapshot before any lifecycle work.
            const DependencyGraph graph = BuildDependencyGraph();
            auto validation = ValidateDependencyGraph(graph);
            if (!validation.valid)
            {
                RVX_CORE_ERROR("Subsystem dependency validation failed: {}", 
                               validation.GetErrorMessage());
                return false;
            }

            if (!BuildOrder(graph))
            {
                return false;
            }

            std::vector<TBase*> initializedThisCall;
            initializedThisCall.reserve(m_ordered.size());
            for (auto* subsystem : m_ordered)
            {
                try
                {
                    RVX_CORE_DEBUG("Initializing subsystem: {}", subsystem->GetName());
                    if (beforeInitialize)
                    {
                        beforeInitialize(*subsystem);
                    }
                    subsystem->Initialize();
                    subsystem->SetInitialized(true);
                    initializedThisCall.push_back(subsystem);
                    if (afterInitialize)
                    {
                        afterInitialize(*subsystem);
                    }
                }
                catch (const std::exception& e)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed to initialize: {}",
                                   subsystem->GetName(),
                                   e.what());
                    UnwindInitialized(initializedThisCall);
                    return false;
                }
                catch (...)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed to initialize with an unknown exception",
                                   subsystem->GetName());
                    UnwindInitialized(initializedThisCall);
                    return false;
                }
            }

            m_initialized = true;
            return true;
        }

        /**
         * @brief Deinitialize a subsystem by type
         * @return False if the subsystem is missing or deinitialization fails
         */
        template<typename T>
        bool DeinitializeSubsystem()
        {
            T* subsystem = GetSubsystem<T>();
            if (!subsystem)
            {
                return false;
            }

            if (!subsystem->IsInitialized())
            {
                return true;
            }

            bool succeeded = true;
            RVX_CORE_DEBUG("Deinitializing subsystem: {}", subsystem->GetName());
            try
            {
                subsystem->Deinitialize();
            }
            catch (const std::exception& e)
            {
                RVX_CORE_ERROR("Subsystem '{}' failed during deinitialize: {}",
                               subsystem->GetName(),
                               e.what());
                succeeded = false;
            }
            catch (...)
            {
                RVX_CORE_ERROR("Subsystem '{}' failed during deinitialize with an unknown exception",
                               subsystem->GetName());
                succeeded = false;
            }

            subsystem->SetInitialized(false);
            m_initialized = std::any_of(
                m_subsystems.begin(),
                m_subsystems.end(),
                [](const std::unique_ptr<TBase>& entry) { return entry->IsInitialized(); });
            return succeeded;
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
                if (!(*it)->IsInitialized())
                {
                    continue;
                }

                RVX_CORE_DEBUG("Deinitializing subsystem: {}", (*it)->GetName());
                try
                {
                    (*it)->Deinitialize();
                }
                catch (const std::exception& e)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed during deinitialize: {}",
                                   (*it)->GetName(),
                                   e.what());
                }
                catch (...)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed during deinitialize with an unknown exception",
                                   (*it)->GetName());
                }
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
                if (subsystem->IsInitialized() && subsystem->ShouldTick())
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
                if (subsystem->IsInitialized() &&
                    subsystem->ShouldTick() &&
                    subsystem->GetTickPhase() == phase)
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
            m_compositionDependencies.clear();
            m_subsystems.clear();
            m_orderDirty = true;
        }

        /**
         * @brief Check if collection is initialized
         */
        bool IsInitialized() const { return m_initialized; }

    private:
        struct DependencyGraph
        {
            std::vector<TBase*> nodes;
            std::vector<std::vector<size_t>> prerequisites;
            std::vector<std::vector<size_t>> dependents;
            std::vector<size_t> indegree;
            std::vector<std::string> missingDependencies;
        };

        DependencyGraph BuildDependencyGraph() const
        {
            DependencyGraph graph;
            graph.nodes.reserve(m_subsystems.size());
            graph.prerequisites.resize(m_subsystems.size());
            graph.dependents.resize(m_subsystems.size());
            graph.indegree.resize(m_subsystems.size(), 0);

            std::unordered_map<TBase*, size_t> nodeIndices;
            nodeIndices.reserve(m_subsystems.size());
            for (size_t index = 0; index < m_subsystems.size(); ++index)
            {
                TBase* subsystem = m_subsystems[index].get();
                graph.nodes.push_back(subsystem);
                nodeIndices.insert_or_assign(subsystem, index);
            }

            std::vector<std::unordered_set<size_t>> prerequisiteSets(
                m_subsystems.size());
            auto addEdge =
                [&](size_t dependentIndex, size_t prerequisiteIndex) {
                    if (prerequisiteSets[dependentIndex]
                            .insert(prerequisiteIndex)
                            .second)
                    {
                        graph.dependents[prerequisiteIndex].push_back(
                            dependentIndex);
                    }
                };

            std::unordered_set<std::string> missingSet;
            auto addMissing = [&](std::string diagnostic) {
                if (missingSet.insert(diagnostic).second)
                {
                    graph.missingDependencies.push_back(
                        std::move(diagnostic));
                }
            };

            for (size_t dependentIndex = 0;
                 dependentIndex < m_subsystems.size();
                 ++dependentIndex)
            {
                TBase* subsystem = graph.nodes[dependentIndex];

                const auto typedDependencies =
                    subsystem->GetTypedDependencies();
                for (const auto& dependency : typedDependencies)
                {
                    const auto dependencyIt =
                        m_lookup.find(dependency.typeIndex);
                    if (dependencyIt == m_lookup.end())
                    {
                        if (!dependency.optional)
                        {
                            addMissing(
                                std::string(subsystem->GetName()) +
                                " requires " +
                                (dependency.name
                                     ? dependency.name
                                     : dependency.typeIndex.name()));
                        }
                        continue;
                    }

                    addEdge(
                        dependentIndex,
                        nodeIndices.at(dependencyIt->second));
                }

                int dependencyCount = 0;
                const char** legacyDependencies =
                    subsystem->GetDependencies(dependencyCount);
                for (int index = 0; index < dependencyCount; ++index)
                {
                    const char* dependencyName =
                        legacyDependencies[index];
                    if (!dependencyName)
                    {
                        continue;
                    }

                    const auto dependencyIt =
                        m_nameLookup.find(dependencyName);
                    if (dependencyIt == m_nameLookup.end())
                    {
                        addMissing(
                            std::string(subsystem->GetName()) +
                            " requires " + dependencyName);
                        continue;
                    }

                    addEdge(
                        dependentIndex,
                        nodeIndices.at(dependencyIt->second));
                }
            }

            for (const auto& [dependentType, prerequisiteTypes] :
                 m_compositionDependencies)
            {
                const auto dependentIt = m_lookup.find(dependentType);
                if (dependentIt == m_lookup.end())
                {
                    addMissing(
                        std::string(dependentType.name()) +
                        " composition dependent is not registered");
                    continue;
                }

                const size_t dependentIndex =
                    nodeIndices.at(dependentIt->second);
                for (const std::type_index prerequisiteType :
                     prerequisiteTypes)
                {
                    const auto prerequisiteIt =
                        m_lookup.find(prerequisiteType);
                    if (prerequisiteIt == m_lookup.end())
                    {
                        addMissing(
                            std::string(dependentIt->second->GetName()) +
                            " requires composition prerequisite " +
                            prerequisiteType.name());
                        continue;
                    }

                    addEdge(
                        dependentIndex,
                        nodeIndices.at(prerequisiteIt->second));
                }
            }

            for (size_t index = 0; index < m_subsystems.size(); ++index)
            {
                graph.prerequisites[index].assign(
                    prerequisiteSets[index].begin(),
                    prerequisiteSets[index].end());
                std::sort(
                    graph.prerequisites[index].begin(),
                    graph.prerequisites[index].end());
                std::sort(
                    graph.dependents[index].begin(),
                    graph.dependents[index].end());
                graph.indegree[index] =
                    graph.prerequisites[index].size();
            }

            return graph;
        }

        DependencyValidationResult ValidateDependencyGraph(
            const DependencyGraph& graph) const
        {
            DependencyValidationResult result;
            result.missingDependencies = graph.missingDependencies;
            if (!result.missingDependencies.empty())
            {
                result.valid = false;
            }

            result.cyclePath = DetectCycles(graph);
            if (!result.cyclePath.empty())
            {
                result.valid = false;
            }
            return result;
        }

        std::vector<std::string> DetectCycles(
            const DependencyGraph& graph) const
        {
            // 0 = white, 1 = gray, 2 = black.
            std::vector<uint8> color(graph.nodes.size(), 0);
            std::vector<size_t> path;
            std::vector<std::string> cyclePath;

            std::function<bool(size_t)> visit;
            visit = [&](size_t nodeIndex) {
                color[nodeIndex] = 1;
                path.push_back(nodeIndex);

                for (const size_t prerequisiteIndex :
                     graph.prerequisites[nodeIndex])
                {
                    if (color[prerequisiteIndex] == 1)
                    {
                        const auto cycleStart = std::find(
                            path.begin(),
                            path.end(),
                            prerequisiteIndex);
                        for (auto iterator = cycleStart;
                             iterator != path.end();
                             ++iterator)
                        {
                            cyclePath.emplace_back(
                                graph.nodes[*iterator]->GetName());
                        }
                        cyclePath.emplace_back(
                            graph.nodes[prerequisiteIndex]->GetName());
                        return true;
                    }
                    if (color[prerequisiteIndex] == 0 &&
                        visit(prerequisiteIndex))
                    {
                        return true;
                    }
                }

                path.pop_back();
                color[nodeIndex] = 2;
                return false;
            };

            for (size_t index = 0; index < graph.nodes.size(); ++index)
            {
                if (color[index] == 0 && visit(index))
                {
                    return cyclePath;
                }
            }
            return {};
        }

        bool BuildOrder(const DependencyGraph& graph)
        {
            if (!m_orderDirty)
            {
                return true;
            }

            std::vector<size_t> indegree = graph.indegree;
            std::priority_queue<
                size_t,
                std::vector<size_t>,
                std::greater<size_t>>
                ready;
            for (size_t index = 0; index < indegree.size(); ++index)
            {
                if (indegree[index] == 0)
                {
                    ready.push(index);
                }
            }

            std::vector<TBase*> ordered;
            ordered.reserve(graph.nodes.size());
            while (!ready.empty())
            {
                const size_t nodeIndex = ready.top();
                ready.pop();
                ordered.push_back(graph.nodes[nodeIndex]);

                for (const size_t dependentIndex :
                     graph.dependents[nodeIndex])
                {
                    if (--indegree[dependentIndex] == 0)
                    {
                        ready.push(dependentIndex);
                    }
                }
            }

            if (ordered.size() != graph.nodes.size())
            {
                const auto cyclePath = DetectCycles(graph);
                std::stringstream cycleStr;
                for (size_t i = 0; i < cyclePath.size(); ++i)
                {
                    if (i > 0)
                    {
                        cycleStr << " -> ";
                    }
                    cycleStr << cyclePath[i];
                }
                RVX_CORE_ERROR("Subsystem dependency cycle detected: {}", cycleStr.str());
                return false;
            }

            m_ordered = std::move(ordered);
            m_orderDirty = false;
            return true;
        }

        void UnwindInitialized(std::vector<TBase*>& initialized)
        {
            for (auto it = initialized.rbegin(); it != initialized.rend(); ++it)
            {
                TBase* subsystem = *it;
                if (!subsystem || !subsystem->IsInitialized())
                {
                    continue;
                }

                RVX_CORE_DEBUG("Unwinding subsystem: {}", subsystem->GetName());
                try
                {
                    subsystem->Deinitialize();
                }
                catch (const std::exception& e)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed during initialization unwind: {}",
                                   subsystem->GetName(),
                                   e.what());
                }
                catch (...)
                {
                    RVX_CORE_ERROR("Subsystem '{}' failed during initialization unwind with an unknown exception",
                                   subsystem->GetName());
                }
                subsystem->SetInitialized(false);
            }

            initialized.clear();
            m_initialized = false;
        }

        std::vector<std::unique_ptr<TBase>> m_subsystems;
        std::unordered_map<std::type_index, TBase*> m_lookup;
        std::unordered_map<TBase*, std::type_index> m_typeLookup;
        std::unordered_map<std::string, TBase*> m_nameLookup;
        std::unordered_map<
            std::type_index,
            std::unordered_set<std::type_index>>
            m_compositionDependencies;
        std::vector<TBase*> m_ordered;
        bool m_orderDirty = true;
        bool m_initialized = false;
    };

} // namespace RVX
