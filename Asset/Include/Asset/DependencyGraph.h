#pragma once

/**
 * @file DependencyGraph.h
 * @brief Dependency tracking and resolution for assets
 */

#include "Asset/Asset.h"
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

namespace RVX::Asset
{
    /**
     * @brief Dependency graph for assets
     * 
     * Tracks which assets depend on which other assets.
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

        /// Add an asset and its dependencies
        void AddAsset(AssetId id, const std::vector<AssetId>& dependencies);

        /// Remove an asset from the graph
        void RemoveAsset(AssetId id);

        /// Update dependencies for an asset
        void UpdateDependencies(AssetId id, const std::vector<AssetId>& dependencies);

        /// Clear the graph
        void Clear();

        // =====================================================================
        // Queries
        // =====================================================================

        /// Get direct dependencies of an asset
        std::vector<AssetId> GetDependencies(AssetId id) const;

        /// Get all dependencies (recursive)
        std::vector<AssetId> GetAllDependencies(AssetId id) const;

        /// Get direct dependents (assets that depend on this one)
        std::vector<AssetId> GetDependents(AssetId id) const;

        /// Get all dependents (recursive)
        std::vector<AssetId> GetAllDependents(AssetId id) const;

        /// Get load order (topological sort) for an asset
        std::vector<AssetId> GetLoadOrder(AssetId id) const;

        /// Get load order for multiple assets
        std::vector<AssetId> GetLoadOrder(const std::vector<AssetId>& ids) const;

        // =====================================================================
        // Validation
        // =====================================================================

        /// Check for circular dependencies starting from an asset
        bool HasCircularDependency(AssetId id) const;

        /// Find all circular dependencies in the graph
        std::vector<std::vector<AssetId>> FindAllCircles() const;

        /// Check if the graph contains an asset
        bool Contains(AssetId id) const;

        // =====================================================================
        // Statistics
        // =====================================================================

        size_t GetAssetCount() const;
        size_t GetTotalEdges() const;

    private:
        mutable std::mutex m_mutex;

        // Adjacency lists
        std::unordered_map<AssetId, std::vector<AssetId>> m_dependencies;
        std::unordered_map<AssetId, std::vector<AssetId>> m_dependents;

        // Helper for recursive operations
        void CollectAllDependencies(AssetId id, std::unordered_set<AssetId>& visited,
                                     std::vector<AssetId>& result) const;
        void CollectAllDependents(AssetId id, std::unordered_set<AssetId>& visited,
                                   std::vector<AssetId>& result) const;
        bool DetectCycle(AssetId id, std::unordered_set<AssetId>& visited,
                        std::unordered_set<AssetId>& recursionStack) const;
    };

} // namespace RVX::Asset
