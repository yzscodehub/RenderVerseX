#pragma once

/**
 * @file QueryFilter.h
 * @brief Query filter for spatial queries
 */

#include <cstdint>
#include <functional>

namespace RVX::Spatial
{
    // Forward declaration
    class ISpatialEntity;

    /**
     * @brief Filter configuration for spatial queries
     * 
     * Allows filtering query results by:
     * - Layer mask (render layers, collision layers)
     * - Type mask (entity types)
     * - Custom predicate function
     */
    struct QueryFilter
    {
        /// Layer mask - bitwise AND with entity layer mask
        uint32_t layerMask = ~0u;

        /// Type mask - bitwise AND with entity type mask
        uint32_t typeMask = ~0u;

        /// Custom filter callback (optional)
        std::function<bool(const ISpatialEntity*)> customFilter = nullptr;

        /// Maximum number of results (0 = unlimited)
        size_t maxResults = 0;

        /// Whether to sort results by distance
        bool sortByDistance = false;

        // =====================================================================
        // Factory Methods
        // =====================================================================

        /// Accept all entities
        static QueryFilter All()
        {
            return QueryFilter{};
        }

        /// Filter by single layer
        static QueryFilter Layer(uint32_t layer)
        {
            return QueryFilter{1u << layer, ~0u};
        }

        /// Filter by layer mask
        static QueryFilter Layers(uint32_t mask)
        {
            return QueryFilter{mask, ~0u};
        }

        /// Filter by single type
        static QueryFilter Type(uint32_t type)
        {
            return QueryFilter{~0u, 1u << type};
        }

        /// Filter by type mask
        static QueryFilter Types(uint32_t mask)
        {
            return QueryFilter{~0u, mask};
        }

        /// Filter by layer and type
        static QueryFilter LayerAndType(uint32_t layer, uint32_t type)
        {
            return QueryFilter{1u << layer, 1u << type};
        }

        /// Create with custom filter
        static QueryFilter Custom(std::function<bool(const ISpatialEntity*)> filter)
        {
            QueryFilter qf;
            qf.customFilter = std::move(filter);
            return qf;
        }

        // =====================================================================
        // Evaluation
        // =====================================================================

        /// Check if an entity passes the filter
        bool Accepts(const ISpatialEntity* entity) const;
    };

} // namespace RVX::Spatial
