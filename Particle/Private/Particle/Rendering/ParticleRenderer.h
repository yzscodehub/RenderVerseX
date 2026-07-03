#pragma once

/**
 * @file ParticleRenderer.h
 * @brief Particle rendering system
 */

#include "Particle/ParticleRenderStats.h"
#include "Particle/ParticleTypes.h"
#include "Particle/Rendering/SoftParticleConfig.h"
#include "RHI/RHI.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Particle
{
    class ParticleSystemInstance;
    class TrailRenderer;

    /**
     * @brief Explicit renderer creation contract for billboard particle pipelines.
     */
    struct ParticleRendererConfig
    {
        std::string shaderDirectory;
        RHIFormat colorTargetFormat = RHIFormat::RGBA16_FLOAT;
        // Matches PipelineCache::GetDefaultDepthStencilFormat() without pulling
        // the full render pipeline cache into this public Particle header.
        RHIFormat depthStencilFormat = RHIFormat::D32_FLOAT;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        bool reverseZ = false;

        // Optional bytecode overrides keep validation tests independent from a
        // platform shader compiler while production uses shaderDirectory.
        std::vector<uint8> vertexShaderBytecode;
        std::vector<uint8> pixelShaderBytecode;
    };

    /**
     * @brief Minimal view constants needed by the particle renderer.
     *
     * ParticlePass adapts Render::ViewData into this local contract so the
     * billboard renderer does not depend on Render module view types.
     */
    struct ParticleRendererViewData
    {
        Mat4 viewMatrix = Mat4Identity();
        Mat4 projectionMatrix = Mat4Identity();
        Mat4 viewProjectionMatrix = Mat4Identity();
        Mat4 inverseViewMatrix = Mat4Identity();
        Vec3 cameraPosition{0.0f, 0.0f, 0.0f};
        Vec3 cameraForward{0.0f, 0.0f, -1.0f};
        uint32 viewportWidth = 0;
        uint32 viewportHeight = 0;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
    };

    /**
     * @brief Particle renderer - handles all particle rendering modes
     */
    class ParticleRenderer
    {
    public:
        ParticleRenderer();
        ~ParticleRenderer();

        // Non-copyable
        ParticleRenderer(const ParticleRenderer&) = delete;
        ParticleRenderer& operator=(const ParticleRenderer&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        void Initialize(IRHIDevice* device);
        void Initialize(IRHIDevice* device, const ParticleRendererConfig& config);
        void Shutdown();
        bool IsInitialized() const { return m_device != nullptr; }
        bool IsRenderingSupported() const { return m_renderingSupported; }
        const std::string& GetUnsupportedReason() const { return m_unsupportedReason; }
        const ParticleRendererConfig& GetConfig() const { return m_config; }
        const ParticleRendererDrawStats& GetLastDrawStats() const { return m_lastDrawStats; }

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Draw particles for an instance
         * @param ctx Command context
         * @param instance Particle system instance
         * @param view Particle renderer view constants
         * @param depthTexture Scene depth texture (for soft particles)
         */
        bool DrawParticles(RHICommandContext& ctx,
                          ParticleSystemInstance* instance,
                          const ParticleRendererViewData& view,
                          RHITextureView* sceneDepthView,
                          ParticleDepthMode depthMode = ParticleDepthMode::FixedFunction,
                          bool allowSoftParticles = true);

        /**
         * @brief Draw particles with indirect draw
         */
        bool DrawParticlesIndirect(RHICommandContext& ctx,
                                   ParticleSystemInstance* instance,
                                   const ParticleRendererViewData& view,
                                   RHITextureView* sceneDepthView,
                                   ParticleDepthMode depthMode = ParticleDepthMode::FixedFunction,
                                   bool allowSoftParticles = true);

        // =====================================================================
        // Pipeline Access
        // =====================================================================

        RHIPipeline* GetBillboardPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode);
        RHIPipeline* GetStretchedBillboardPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode);
        RHIPipeline* GetMeshPipeline(ParticleBlendMode blend);
        RHIPipeline* GetTrailPipeline(ParticleBlendMode blend, ParticleDepthMode depthMode);

        // =====================================================================
        // Resource Management
        // =====================================================================

        /// Get or create quad vertex buffer
        RHIBuffer* GetQuadVertexBuffer();

        /// Get or create quad index buffer
        RHIBuffer* GetQuadIndexBuffer();

        /// Index format used by the shared quad index buffer
        RHIFormat GetQuadIndexFormat() const { return RHIFormat::R16_UINT; }

        // =====================================================================
        // Trail Renderer
        // =====================================================================

        TrailRenderer* GetTrailRenderer() { return m_trailRenderer.get(); }

    private:
        uint32 MakePipelineKey(ParticleRenderMode mode, ParticleBlendMode blend, ParticleDepthMode depthMode);
        RHIPipeline* CreatePipelineIfNeeded(ParticleRenderMode mode, ParticleBlendMode blend, ParticleDepthMode depthMode);
        const char* GetShaderNameForMode(ParticleRenderMode mode) const;
        void CreateQuadBuffers();
        bool ValidateConfig();
        bool CreateSharedResources();
        bool CreateShaders();
        bool CreateDescriptorLayout();
        bool CreateFallbackTextureResources();
        RHIDescriptorSetRef CreateParticleDescriptorSet(ParticleSystemInstance* instance,
                                                        RHITextureView* sceneDepthView);
        void UploadRenderConstants(const ParticleRendererViewData& view,
                                   const SoftParticleConfig& softConfig,
                                   bool sceneDepthTestEnabled);
        SoftParticleConfig ResolveSoftParticleConfig(const ParticleSystemInstance& instance,
                                                     RHITextureView* sceneDepthView,
                                                     ParticleDepthMode depthMode,
                                                     bool allowSoftParticles);
        void SetUnsupported(const std::string& reason);

        IRHIDevice* m_device = nullptr;
        ParticleRendererConfig m_config;
        bool m_renderingSupported = false;
        std::string m_unsupportedReason = "Particle render pipelines are not implemented";

        // Shaders and layout
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        RHIDescriptorSetLayoutRef m_descriptorSetLayout;
        RHIPipelineLayoutRef m_pipelineLayout;

        // Pipeline cache
        std::unordered_map<uint32, RHIPipelineRef> m_pipelineCache;

        // Shared geometry
        RHIBufferRef m_quadVertexBuffer;
        RHIBufferRef m_quadIndexBuffer;

        // Render constants
        RHIBufferRef m_renderConstantsBuffer;

        // Default particle texture resources
        RHITextureRef m_fallbackTexture;
        RHITextureViewRef m_fallbackTextureView;
        RHITextureRef m_fallbackDepthTexture;
        RHITextureViewRef m_fallbackDepthTextureView;
        RHISamplerRef m_sampler;
        RHISamplerRef m_depthSampler;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;
        ParticleRendererDrawStats m_lastDrawStats;

        // Trail renderer
        std::unique_ptr<TrailRenderer> m_trailRenderer;
    };

} // namespace RVX::Particle
