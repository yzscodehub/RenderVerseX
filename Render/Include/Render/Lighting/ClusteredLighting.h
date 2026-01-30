/**
 * @file ClusteredLighting.h
 * @brief Clustered forward/deferred lighting system
 * 
 * Implements clustered shading for efficient multi-light rendering.
 * Divides the view frustum into 3D clusters and assigns lights to clusters.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <vector>
#include <memory>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class LightManager;

    /**
     * @brief Configuration for clustered lighting
     */
    struct ClusteringConfig
    {
        uint32 clusterCountX = 16;     ///< Clusters in X (screen space)
        uint32 clusterCountY = 9;      ///< Clusters in Y (screen space)
        uint32 clusterCountZ = 24;     ///< Clusters in Z (depth)
        
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        
        uint32 maxLightsPerCluster = 100;
        
        /// Total cluster count
        uint32 GetTotalClusterCount() const
        {
            return clusterCountX * clusterCountY * clusterCountZ;
        }
    };

    /**
     * @brief Light index for cluster
     */
    struct LightIndex
    {
        uint16 lightIndex;
        uint16 lightType;  // 0 = point, 1 = spot
    };

    /**
     * @brief Cluster data for GPU
     */
    struct GPUCluster
    {
        uint32 offset;      ///< Offset into light index list
        uint32 count;       ///< Number of lights in this cluster
        uint32 pointCount;  ///< Number of point lights
        uint32 spotCount;   ///< Number of spot lights
    };

    /**
     * @brief Clustered lighting manager
     * 
     * Implements clustered forward shading:
     * 1. Divide view frustum into 3D grid of clusters
     * 2. Compute cluster AABBs
     * 3. Cull lights against clusters
     * 4. Create light index lists
     * 5. Shade pixels using their cluster's light list
     * 
     * Benefits:
     * - O(1) light lookup per pixel
     * - Scales well with many lights
     * - Works with forward rendering
     */
    class ClusteredLighting
    {
    public:
        ClusteredLighting() = default;
        ~ClusteredLighting();

        // Non-copyable
        ClusteredLighting(const ClusteredLighting&) = delete;
        ClusteredLighting& operator=(const ClusteredLighting&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the clustered lighting system
         */
        void Initialize(IRHIDevice* device, const ClusteringConfig& config = {});

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Get current configuration
         */
        const ClusteringConfig& GetConfig() const { return m_config; }

        /**
         * @brief Reconfigure clusters (rebuilds cluster AABBs)
         */
        void Reconfigure(const ClusteringConfig& config);

        // =========================================================================
        // Per-Frame Update
        // =========================================================================

        /**
         * @brief Begin a new frame
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         * @param screenWidth Screen width in pixels
         * @param screenHeight Screen height in pixels
         */
        void BeginFrame(const Mat4& viewMatrix, const Mat4& projMatrix,
                        uint32 screenWidth, uint32 screenHeight);

        /**
         * @brief Assign lights to clusters
         * @param lightManager Light manager with current frame's lights
         */
        void AssignLights(const LightManager& lightManager);

        /**
         * @brief Upload cluster data to GPU
         * @param ctx Command context for buffer updates
         */
        void UpdateGPUBuffers(RHICommandContext& ctx);

        // =========================================================================
        // GPU Resources
        // =========================================================================

        /**
         * @brief Get cluster AABB buffer (for debug visualization)
         */
        RHIBuffer* GetClusterAABBBuffer() const { return m_clusterAABBBuffer.Get(); }

        /**
         * @brief Get cluster data buffer (offset/count per cluster)
         */
        RHIBuffer* GetClusterBuffer() const { return m_clusterBuffer.Get(); }

        /**
         * @brief Get light index buffer
         */
        RHIBuffer* GetLightIndexBuffer() const { return m_lightIndexBuffer.Get(); }

        /**
         * @brief Get clustering constants buffer
         */
        RHIBuffer* GetClusterConstantsBuffer() const { return m_clusterConstantsBuffer.Get(); }

        // =========================================================================
        // Debug
        // =========================================================================

        /**
         * @brief Get statistics for current frame
         */
        struct Statistics
        {
            uint32 activeClusters = 0;
            uint32 totalLightAssignments = 0;
            uint32 maxLightsInCluster = 0;
            float avgLightsPerCluster = 0.0f;
        };

        Statistics GetStatistics() const { return m_stats; }

        /**
         * @brief Enable/disable cluster visualization
         */
        void SetDebugVisualization(bool enable) { m_debugVisualize = enable; }
        bool IsDebugVisualizationEnabled() const { return m_debugVisualize; }

    private:
        struct ClusterAABB
        {
            Vec4 minPoint;  // .w unused
            Vec4 maxPoint;  // .w unused
        };

        void BuildClusterAABBs();
        void ClearClusters();
        bool IntersectsCluster(const ClusterAABB& cluster, const Vec3& lightPos, float range);

        IRHIDevice* m_device = nullptr;
        ClusteringConfig m_config;

        // View data
        Mat4 m_viewMatrix;
        Mat4 m_projMatrix;
        Mat4 m_invProjMatrix;
        uint32 m_screenWidth = 0;
        uint32 m_screenHeight = 0;

        // CPU-side cluster data
        std::vector<ClusterAABB> m_clusterAABBs;
        std::vector<GPUCluster> m_clusters;
        std::vector<LightIndex> m_lightIndices;

        // GPU buffers
        RHIBufferRef m_clusterAABBBuffer;
        RHIBufferRef m_clusterBuffer;
        RHIBufferRef m_lightIndexBuffer;
        RHIBufferRef m_clusterConstantsBuffer;

        // Statistics
        Statistics m_stats;
        bool m_debugVisualize = false;
    };

} // namespace RVX
