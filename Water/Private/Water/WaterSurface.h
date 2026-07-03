#pragma once

/**
 * @file WaterSurface.h
 * @brief Water surface mesh and properties
 * 
 * Defines the water surface mesh generation and visual properties.
 */

#include "Water/WaterTypes.h"
#include "RHI/RHITexture.h"
#include "RHI/RHIBuffer.h"

#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;

    /**
     * @brief Water surface mesh and rendering data
     * 
     * Manages the water surface mesh, textures, and rendering properties.
     * 
     * Features:
     * - Tessellated grid mesh with LOD
     * - Normal map generation from displacement
     * - Foam texture support
     * - Flow map support for rivers
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
     * surface->InitializeGPU(device);
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

        // =====================================================================
        // Textures
        // =====================================================================

        /**
         * @brief Set the normal map texture
         */
        void SetNormalMap(RHITextureRef normalMap);
        RHITexture* GetNormalMap() const { return m_normalMap.Get(); }

        /**
         * @brief Set the foam texture
         */
        void SetFoamTexture(RHITextureRef foamTexture);
        RHITexture* GetFoamTexture() const { return m_foamTexture.Get(); }

        /**
         * @brief Set the flow map (for rivers)
         */
        void SetFlowMap(RHITextureRef flowMap);
        RHITexture* GetFlowMap() const { return m_flowMap.Get(); }

        /**
         * @brief Set environment cubemap for reflections
         */
        void SetEnvironmentMap(RHITextureRef envMap);
        RHITexture* GetEnvironmentMap() const { return m_environmentMap.Get(); }

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Initialize GPU resources
         * @param device RHI device
         * @return true if initialization succeeded
         */
        bool InitializeGPU(IRHIDevice* device);

        /**
         * @brief Get the vertex buffer
         */
        RHIBuffer* GetVertexBuffer() const { return m_vertexBuffer.Get(); }

        /**
         * @brief Get the index buffer
         */
        RHIBuffer* GetIndexBuffer() const { return m_indexBuffer.Get(); }

        /**
         * @brief Get index count
         */
        uint32 GetIndexCount() const { return m_indexCount; }

        /**
         * @brief Check if GPU initialized
         */
        bool IsGPUInitialized() const { return m_gpuInitialized; }

    private:
        void GenerateMesh();

        Vec2 m_size{100.0f, 100.0f};
        uint32 m_resolution = 128;
        WaterSurfaceType m_type = WaterSurfaceType::Ocean;
        WaterVisualProperties m_visual;

        // Textures
        RHITextureRef m_normalMap;
        RHITextureRef m_foamTexture;
        RHITextureRef m_flowMap;
        RHITextureRef m_environmentMap;

        // Mesh data
        std::vector<Vec3> m_vertices;
        std::vector<Vec2> m_uvs;
        std::vector<uint32> m_indices;

        // GPU resources
        RHIBufferRef m_vertexBuffer;
        RHIBufferRef m_indexBuffer;
        uint32 m_indexCount = 0;
        bool m_gpuInitialized = false;
    };

} // namespace RVX
