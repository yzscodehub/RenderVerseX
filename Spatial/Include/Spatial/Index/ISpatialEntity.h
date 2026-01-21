#pragma once

/**
 * @file ISpatialEntity.h
 * @brief Interface for entities that can be indexed spatially
 */

#include "Core/Math/AABB.h"
#include <cstdint>

namespace RVX::Spatial
{
    /// Entity handle type
    using EntityHandle = uint32_t;
    static constexpr EntityHandle InvalidHandle = ~0u;

    /**
     * @brief Interface for entities that can be spatially indexed
     * 
     * Any object that needs to participate in spatial queries
     * (frustum culling, ray picking, range queries) should implement
     * this interface.
     */
    class ISpatialEntity
    {
    public:
        virtual ~ISpatialEntity() = default;

        // =====================================================================
        // Identity
        // =====================================================================

        /// Get unique handle for this entity
        virtual EntityHandle GetHandle() const = 0;

        // =====================================================================
        // Bounds
        // =====================================================================

        /// Get world-space bounding box
        virtual AABB GetWorldBounds() const = 0;

        // =====================================================================
        // Filtering
        // =====================================================================

        /// Get layer mask for filtering (default: all layers)
        virtual uint32_t GetLayerMask() const { return ~0u; }

        /// Get type mask for filtering (default: all types)
        virtual uint32_t GetTypeMask() const { return ~0u; }

        // =====================================================================
        // Dirty Tracking
        // =====================================================================

        /// Check if spatial data needs updating
        virtual bool IsSpatialDirty() const = 0;

        /// Clear dirty flag after spatial index update
        virtual void ClearSpatialDirty() = 0;

        // =====================================================================
        // Optional
        // =====================================================================

        /// Get user data pointer (optional)
        virtual void* GetUserData() const { return nullptr; }
    };

} // namespace RVX::Spatial
