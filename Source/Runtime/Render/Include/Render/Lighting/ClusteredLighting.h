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

#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class LightManager;

    /** Matches the fixed local-light loop bound in DefaultLit/DeferredLighting. */
    inline constexpr uint32 RVX_CLUSTERED_LIGHTING_SHADER_MAX_LIGHTS_PER_PIXEL = 100U;

    /**
     * @brief Device-NDC depth interval represented by the frame projection.
     *
     * RenderVerseX's normal desktop projections use ZeroToOne for DX12,
     * Vulkan, Metal, and the canonical OpenGL projection path.  The explicit
     * value remains part of the clustering input so tests and future imported
     * views cannot silently reinterpret a [-1, 1] projection as [0, 1].
     */
    enum class ClusterNdcDepthRange : uint8
    {
        ZeroToOne = 0,
        MinusOneToOne,
    };

    /**
     * @brief Immutable camera data used to construct one frame's clusters.
     *
     * `nearPlane`/`farPlane` deliberately come from the accepted ViewData,
     * rather than the allocation-time ClusteringConfig.  The matrix pair and
     * device-depth convention describe how those linear view distances map to
     * the active projection, including reverse-Z views.
     */
    struct ClusteredLightingView
    {
        Mat4 viewMatrix = Mat4Identity();
        Mat4 projectionMatrix = Mat4Identity();
        Mat4 inverseProjectionMatrix = Mat4Identity();
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        uint32 screenWidth = 0;
        uint32 screenHeight = 0;
        ClusterNdcDepthRange ndcDepthRange =
            ClusterNdcDepthRange::ZeroToOne;
        bool reverseZ = false;
    };

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
        
        /**
         * @brief CPU-side cluster cap. It may never exceed the shader's
         * RVX_MAX_CLUSTERED_LIGHTS_PER_PIXEL loop bound.
         */
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

    static_assert(sizeof(LightIndex) == sizeof(uint32),
                  "Clustered light indices are read as packed uint values in shaders");

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
     * @brief Clustered lighting constants uploaded to the GPU
     */
    struct GPUClusterConstants
    {
        Vec4 clusterSize;  ///< x/y/z cluster counts, w total cluster count
        Vec4 screenParams; ///< x/y viewport size, z/w near/far planes
        Mat4 invProj;
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
        bool Initialize(IRHIDevice* device, const ClusteringConfig& config = {});

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        /**
         * @brief Get the last visible failure reason
         */
        const std::string& GetLastError() const { return m_lastError; }

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
        bool Reconfigure(const ClusteringConfig& config);

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
        bool BeginFrame(const ClusteredLightingView& view);

        /**
         * @brief Assign lights to clusters
         * @param lightManager Light manager with current frame's lights
         */
        bool AssignLights(const LightManager& lightManager);

        /**
         * @brief Upload cluster data to GPU
         * @param ctx Command context for buffer updates
         */
        bool UpdateGPUBuffers(RHICommandContext& ctx);

        /**
         * @brief Upload current frame cluster data to GPU-visible buffers
         */
        bool UploadFrameData();

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
            uint32 clusterCount = 0;
            uint32 lightIndexCount = 0;
            uint32 activeClusters = 0;
            uint32 totalLightAssignments = 0;
            uint32 maxLightsInCluster = 0;
            /** Clusters whose visible-light set exceeded the declared cap. */
            uint32 overflowedClusterCount = 0;
            /** Intersecting light assignments rejected by the cap. */
            uint32 overflowLightAssignmentCount = 0;
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

        bool BuildClusterAABBs();
        void ClearClusters();
        bool IntersectsCluster(const ClusterAABB& cluster, const Vec3& lightPos, float range);
        void ReleaseResources();
        void SetLastError(std::string message);
        bool ValidateConfig(const ClusteringConfig& config,
                            uint64& outTotalClusters,
                            uint64& outLightIndexCapacity);
        bool CreateBuffers(uint64 totalClusters, uint64 lightIndexCapacity);
        bool UploadBuffer(RHIBuffer* buffer, const void* data, uint64 size, const char* label);
        bool UnprojectDeviceDepth(float ndcX, float ndcY, float deviceDepth,
                                  Vec3& outViewPosition) const;
        bool BuildViewSpacePointAtDepth(const Vec3& nearViewPosition,
                                        const Vec3& farViewPosition,
                                        float viewDepth,
                                        Vec3& outViewPosition) const;

        IRHIDevice* m_device = nullptr;
        bool m_initialized = false;
        bool m_frameBegun = false;
        ClusteringConfig m_config;
        std::string m_lastError;

        // View data
        Mat4 m_viewMatrix;
        Mat4 m_projMatrix;
        Mat4 m_invProjMatrix;
        uint32 m_screenWidth = 0;
        uint32 m_screenHeight = 0;
        float m_frameNearPlane = 0.1f;
        float m_frameFarPlane = 1000.0f;
        ClusterNdcDepthRange m_ndcDepthRange =
            ClusterNdcDepthRange::ZeroToOne;
        bool m_reverseZ = false;

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
