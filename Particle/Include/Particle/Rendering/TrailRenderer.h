#pragma once

/**
 * @file TrailRenderer.h
 * @brief Trail/ribbon rendering for particles
 */

#include "Particle/ParticleTypes.h"
#include "Particle/Modules/TrailModule.h"
#include "RHI/RHI.h"
#include "Render/Renderer/ViewData.h"
#include <unordered_map>
#include <vector>

namespace RVX::Particle
{
    /**
     * @brief Trail vertex data
     */
    struct TrailVertex
    {
        Vec3 position;
        Vec3 direction;
        float width;
        float texCoordU;
        Vec4 color;
    };

    /**
     * @brief Trail history for a single particle
     */
    struct TrailHistory
    {
        std::vector<TrailVertex> points;
        float age = 0.0f;
        bool alive = true;
    };

    /**
     * @brief Trail/ribbon renderer
     */
    class TrailRenderer
    {
    public:
        TrailRenderer() = default;
        ~TrailRenderer();

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(IRHIDevice* device, uint32 maxTrailVertices);
        void Shutdown();

        // =====================================================================
        // Frame Update
        // =====================================================================

        /// Begin a new frame of trail updates
        void BeginFrame();

        /// Add a trail point for a particle
        void AddTrailPoint(uint32 particleId, 
                          const Vec3& position,
                          const Vec3& velocity,
                          float width,
                          const Vec4& color);

        /// Mark a particle's trail as dead
        void MarkTrailDead(uint32 particleId);

        /// End frame and upload to GPU
        void EndFrame(RHICommandContext& ctx);

        // =====================================================================
        // Rendering
        // =====================================================================

        void Draw(RHICommandContext& ctx, const ViewData& view);

        // =====================================================================
        // Configuration
        // =====================================================================

        void SetTrailConfig(const TrailModule* config) { m_config = config; }

    private:
        void UpdateTrailHistory(uint32 particleId, const TrailVertex& vertex);
        void BuildTrailMesh();
        void UploadToGPU();

        IRHIDevice* m_device = nullptr;
        uint32 m_maxVertices = 0;

        // Trail configuration
        const TrailModule* m_config = nullptr;

        // Per-particle trail history
        std::unordered_map<uint32, TrailHistory> m_trailHistories;

        // Built mesh data
        std::vector<TrailVertex> m_vertices;
        std::vector<uint32> m_indices;

        // GPU buffers
        RHIBufferRef m_vertexBuffer;
        RHIBufferRef m_indexBuffer;
        uint32 m_indexCount = 0;
    };

} // namespace RVX::Particle
