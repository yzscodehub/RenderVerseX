#pragma once

/**
 * @file WaterSurface.h
 * @brief CPU water surface mesh and properties
 * 
 * Defines the water surface mesh generation and visual properties exported to
 * Render-owned water passes.
 */

#include "Water/WaterTypes.h"

#include <memory>
#include <vector>

namespace RVX
{
    /**
     * @brief Water surface mesh and rendering data source
     * 
     * Manages CPU water surface data. GPU resources are owned by Render.
     * 
     * Features:
     * - Tessellated grid mesh with LOD
     * - Surface visual configuration
     * - Render snapshot source data
     * 
     * Usage:
     * @code
     * WaterSurfaceDesc desc;
     * desc.size = Vec2(500.0f, 500.0f);
     * desc.resolution = 256;
     * desc.type = WaterSurfaceType::Ocean;
     * 
     * auto surface = std::make_shared<WaterSurface>();
     * surface->Create(desc);
     * @endcode
     */
    class WaterSurface
    {
    public:
        using Ptr = std::shared_ptr<WaterSurface>;

        WaterSurface() = default;
        ~WaterSurface() = default;

        // Non-copyable
        WaterSurface(const WaterSurface&) = delete;
        WaterSurface& operator=(const WaterSurface&) = delete;

        // =====================================================================
        // Creation
        // =====================================================================

        /**
         * @brief Create the water surface
         * @param desc Surface descriptor
         * @return true if creation succeeded
         */
        bool Create(const WaterSurfaceDesc& desc);

        // =====================================================================
        // Properties
        // =====================================================================

        const Vec2& GetSize() const { return m_size; }
        uint32 GetResolution() const { return m_resolution; }
        WaterSurfaceType GetType() const { return m_type; }

        WaterVisualProperties& GetVisualProperties() { return m_visual; }
        const WaterVisualProperties& GetVisualProperties() const { return m_visual; }
        void SetVisualProperties(const WaterVisualProperties& props);

        /**
         * @brief Get index count
         */
        uint32 GetIndexCount() const { return m_indexCount; }

    private:
        void GenerateMesh();

        Vec2 m_size{100.0f, 100.0f};
        uint32 m_resolution = 128;
        WaterSurfaceType m_type = WaterSurfaceType::Ocean;
        WaterVisualProperties m_visual;

        // Mesh data
        std::vector<Vec3> m_vertices;
        std::vector<Vec2> m_uvs;
        std::vector<uint32> m_indices;

        uint32 m_indexCount = 0;
    };

} // namespace RVX
