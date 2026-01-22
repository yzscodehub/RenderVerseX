#pragma once

/**
 * @file DebugRenderer.h
 * @brief Debug visualization renderer for lines, boxes, and gizmos
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <vector>

namespace RVX
{
    class PipelineCache;
    struct ViewData;

    /**
     * @brief Debug line vertex
     */
    struct DebugVertex
    {
        Vec3 position;
        Vec4 color;
    };

    /**
     * @brief Debug rendering capabilities
     * 
     * Provides immediate-mode debug drawing for:
     * - Lines and polylines
     * - Bounding boxes (AABB, OBB)
     * - Spheres and circles
     * - Coordinate axes and gizmos
     * - Grid planes
     */
    class DebugRenderer
    {
    public:
        DebugRenderer() = default;
        ~DebugRenderer();

        // Non-copyable
        DebugRenderer(const DebugRenderer&) = delete;
        DebugRenderer& operator=(const DebugRenderer&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        void Initialize(IRHIDevice* device, PipelineCache* pipelineCache);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Drawing Commands (immediate mode)
        // =========================================================================

        /**
         * @brief Draw a line between two points
         */
        void DrawLine(const Vec3& start, const Vec3& end, const Vec4& color);

        /**
         * @brief Draw an axis-aligned bounding box
         */
        void DrawAABB(const Vec3& min, const Vec3& max, const Vec4& color);

        /**
         * @brief Draw an oriented bounding box
         */
        void DrawOBB(const Vec3& center, const Vec3& halfExtents, const Mat4& rotation, const Vec4& color);

        /**
         * @brief Draw a sphere (wireframe)
         */
        void DrawSphere(const Vec3& center, float radius, const Vec4& color, int segments = 16);

        /**
         * @brief Draw a circle on a plane
         */
        void DrawCircle(const Vec3& center, const Vec3& normal, float radius, const Vec4& color, int segments = 32);

        /**
         * @brief Draw coordinate axes
         */
        void DrawAxes(const Vec3& origin, const Mat4& orientation, float size = 1.0f);

        /**
         * @brief Draw a grid on the XZ plane
         */
        void DrawGrid(const Vec3& center, float size, int divisions, const Vec4& color);

        /**
         * @brief Draw a frustum
         */
        void DrawFrustum(const Mat4& viewProjection, const Vec4& color);

        /**
         * @brief Draw an arrow
         */
        void DrawArrow(const Vec3& start, const Vec3& end, const Vec4& color, float headSize = 0.1f);

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Begin a new frame (clears accumulated primitives)
         */
        void BeginFrame();

        /**
         * @brief Flush all accumulated debug primitives to GPU
         */
        void Render(RHICommandContext& ctx, const ViewData& view);

        /**
         * @brief Set whether debug rendering is enabled
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set depth testing mode
         */
        void SetDepthTest(bool enabled) { m_depthTestEnabled = enabled; }

    private:
        void EnsureBuffers();
        void UpdateVertexBuffer();

        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        bool m_enabled = true;
        bool m_depthTestEnabled = true;

        // Accumulated vertices for current frame
        std::vector<DebugVertex> m_vertices;

        // GPU resources
        RHIBufferRef m_vertexBuffer;
        static constexpr size_t MaxDebugVertices = 65536;
    };

} // namespace RVX
