#pragma once

/**
 * @file TerrainLOD.h
 * @brief Terrain Level of Detail system
 * 
 * Provides CDLOD (Continuous Distance-dependent Level of Detail) for
 * efficient terrain rendering with smooth transitions.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHIBuffer.h"

#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class Heightmap;

    /**
     * @brief LOD node representing a terrain quadtree node
     */
    struct TerrainLODNode
    {
        Vec2 position;          ///< World-space position (XZ)
        float size;             ///< Node size in world units
        uint8 level;            ///< LOD level (0 = highest detail)
        uint8 morphFactor;      ///< Morph factor for smooth transitions [0-255]
        bool isLeaf;            ///< True if this is a leaf node to render
        uint32 lodMask;         ///< Neighbor LOD levels for crack prevention
    };

    /**
     * @brief LOD selection parameters
     */
    struct TerrainLODParams
    {
        float lodDistance = 100.0f;     ///< Base distance for LOD transition
        float lodBias = 0.0f;           ///< LOD bias (negative = higher quality)
        float morphRange = 0.1f;        ///< Morph transition range [0-1]
        uint32 maxLODLevels = 8;        ///< Maximum LOD levels
        uint32 patchSize = 32;          ///< Patch vertex count
    };

    /**
     * @brief Terrain LOD selection result
     */
    struct TerrainLODSelection
    {
        std::vector<TerrainLODNode> nodes;  ///< Selected nodes to render
        uint32 totalPatches = 0;             ///< Total patch count
        uint32 totalTriangles = 0;           ///< Total triangle count
    };

    /**
     * @brief Terrain Level of Detail system
     * 
     * Implements CDLOD (Continuous Distance-dependent Level of Detail) algorithm
     * for efficient terrain rendering. Uses a quadtree structure for hierarchical
     * culling and LOD selection.
     * 
     * Features:
     * - Hierarchical quadtree-based LOD
     * - Continuous morph-based transitions
     * - Frustum culling at each LOD level
     * - Crack prevention via neighbor LOD matching
     * - GPU-friendly patch generation
     * 
     * Usage:
     * @code
     * TerrainLODParams params;
     * params.lodDistance = 200.0f;
     * params.maxLODLevels = 6;
     * 
     * auto lod = std::make_unique<TerrainLOD>();
     * lod->Initialize(heightmap, terrainSize, params);
     * 
     * // Per frame
     * TerrainLODSelection selection;
     * lod->SelectLOD(cameraPosition, frustum, selection);
     * 
     * for (const auto& node : selection.nodes) {
     *     // Render patch at node.position with node.level
     * }
     * @endcode
     */
    class TerrainLOD
    {
    public:
        TerrainLOD() = default;
        ~TerrainLOD() = default;

        // Non-copyable
        TerrainLOD(const TerrainLOD&) = delete;
        TerrainLOD& operator=(const TerrainLOD&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the LOD system
         * @param heightmap Terrain heightmap
         * @param terrainSize Terrain size (width, height, depth)
         * @param params LOD parameters
         * @return true if initialization succeeded
         */
        bool Initialize(const Heightmap* heightmap, const Vec3& terrainSize, 
                        const TerrainLODParams& params);

        /**
         * @brief Update LOD parameters
         * @param params New LOD parameters
         */
        void SetParams(const TerrainLODParams& params);

        /**
         * @brief Get current LOD parameters
         */
        const TerrainLODParams& GetParams() const { return m_params; }

        // =====================================================================
        // LOD Selection
        // =====================================================================

        /**
         * @brief Select LOD levels based on camera position
         * @param cameraPosition Camera world position
         * @param frustumPlanes Frustum planes for culling (can be null)
         * @param outSelection Output selection result
         */
        void SelectLOD(const Vec3& cameraPosition, const Vec4* frustumPlanes,
                       TerrainLODSelection& outSelection);

        /**
         * @brief Get the LOD level for a given distance
         * @param distance Distance from camera
         * @return LOD level (0 = highest detail)
         */
        uint8 GetLODLevel(float distance) const;

        /**
         * @brief Get the morph factor for smooth LOD transition
         * @param distance Distance from camera
         * @param lodLevel Current LOD level
         * @return Morph factor [0, 1]
         */
        float GetMorphFactor(float distance, uint8 lodLevel) const;

        // =====================================================================
        // GPU Resources
        // =====================================================================

        /**
         * @brief Create GPU buffers for terrain patches
         * @param device RHI device
         * @return true if creation succeeded
         */
        bool CreateGPUResources(IRHIDevice* device);

        /**
         * @brief Get the patch vertex buffer
         */
        RHIBuffer* GetPatchVertexBuffer() const { return m_patchVertexBuffer.Get(); }

        /**
         * @brief Get the patch index buffer
         */
        RHIBuffer* GetPatchIndexBuffer() const { return m_patchIndexBuffer.Get(); }

        /**
         * @brief Get index count per patch
         */
        uint32 GetPatchIndexCount() const { return m_patchIndexCount; }

        // =====================================================================
        // Statistics
        // =====================================================================

        struct Statistics
        {
            uint32 nodesTraversed = 0;
            uint32 nodesCulled = 0;
            uint32 patchesRendered = 0;
            uint32 trianglesRendered = 0;
        };

        const Statistics& GetStatistics() const { return m_stats; }

    private:
        struct QuadTreeNode
        {
            Vec2 min;
            Vec2 max;
            float minHeight;
            float maxHeight;
            uint32 children[4];     // Child node indices (0 if none)
            uint8 level;
        };

        void BuildQuadTree(const Heightmap* heightmap, const Vec3& terrainSize);
        void SelectLODRecursive(uint32 nodeIndex, const Vec3& cameraPos,
                                 const Vec4* frustumPlanes, TerrainLODSelection& selection);
        bool IsNodeInFrustum(const QuadTreeNode& node, const Vec4* frustumPlanes) const;
        void CreatePatchMesh(uint32 patchSize);

        std::vector<QuadTreeNode> m_quadTree;
        TerrainLODParams m_params;
        Vec3 m_terrainSize{1.0f};
        
        // GPU resources for patch rendering
        RHIBufferRef m_patchVertexBuffer;
        RHIBufferRef m_patchIndexBuffer;
        uint32 m_patchIndexCount = 0;

        std::vector<Vec2> m_patchVertices;
        std::vector<uint32> m_patchIndices;

        Statistics m_stats;
    };

} // namespace RVX
