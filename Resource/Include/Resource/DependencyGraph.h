#pragma once

/**
 * @file DependencyGraph.h
 * @brief Dependency tracking and resolution for resources
 */

#include "Resource/IResource.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

namespace RVX::Resource
{
    /**
     * @brief Dependency graph for resources
     * 
     * Tracks which resources depend on which other resources.
     * Provides:
     * - Topological sorting for load order
     * - Dependent lookup (what depends on X)
     * - Circular dependency detection
     */
    class DependencyGraph
    {
    public:
        DependencyGraph();
        ~DependencyGraph();

        // =====================================================================
        // Graph Building
        // =====================================================================

        /// Add a resource and its dependencies
        void AddResource(ResourceId id, const std::vector<ResourceId>& dependencies);

        /// Remove a resource from the graph
        void RemoveResource(ResourceId id);

        /// Update dependencies for a resource
        void UpdateDependencies(ResourceId id, const std::vector<ResourceId>& dependencies);

        /// Clear the graph
        void Clear();

        // =====================================================================
        // Queries
        // =====================================================================

        /// Get direct dependencies of a resource
        std::vector<ResourceId> GetDependencies(ResourceId id) const;

        /// Get all dependencies (recursive)
        std::vector<ResourceId> GetAllDependencies(ResourceId id) const;

        /// Get direct dependents (resources that depend on this one)
        std::vector<ResourceId> GetDependents(ResourceId id) const;

        /// Get all dependents (recursive)
        std::vector<ResourceId> GetAllDependents(ResourceId id) const;

        /// Get load order (topological sort) for a resource
        std::vector<ResourceId> GetLoadOrder(ResourceId id) const;

        /// Get load order for multiple resources
        std::vector<ResourceId> GetLoadOrder(const std::vector<ResourceId>& ids) const;

        // =====================================================================
        // Validation
        // =====================================================================

        /// Check for circular dependencies starting from a resource
        bool HasCircularDependency(ResourceId id) const;

        /// Find all circular dependencies in the graph
        std::vector<std::vector<ResourceId>> FindAllCircles() const;

        /// Check if the graph contains a resource
        bool Contains(ResourceId id) const;

        // =====================================================================
        // Statistics
        // =====================================================================

        size_t GetResourceCount() const;
        size_t GetTotalEdges() const;

    private:
        mutable std::mutex m_mutex;

        // Adjacency lists
        std::unordered_map<ResourceId, std::vector<ResourceId>> m_dependencies;
        std::unordered_map<ResourceId, std::vector<ResourceId>> m_dependents;

        // Helper for recursive operations
        void CollectAllDependencies(ResourceId id, std::unordered_set<ResourceId>& visited,
                                     std::vector<ResourceId>& result) const;
        void CollectAllDependents(ResourceId id, std::unordered_set<ResourceId>& visited,
                                   std::vector<ResourceId>& result) const;
        bool DetectCycle(ResourceId id, std::unordered_set<ResourceId>& visited,
                        std::unordered_set<ResourceId>& recursionStack) const;
    };

} // namespace RVX::Resource
