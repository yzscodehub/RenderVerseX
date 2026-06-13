#pragma once

/**
 * @file ParticleRenderer.h
 * @brief Particle rendering system
 */

#include "Particle/ParticleTypes.h"
#include "Particle/Rendering/SoftParticleConfig.h"
#include "Particle/Rendering/TrailRenderer.h"
#include "RHI/RHI.h"
#include "Render/Renderer/ViewData.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace RVX::Particle
{
    class ParticleSystemInstance;

    /**
     * @brief Explicit renderer creation contract for billboard particle pipelines.
     */
    struct ParticleRendererConfig
    {
        std::string shaderDirectory;
        RHIFormat colorTargetFormat = RHIFormat::RGBA16_FLOAT;
        RHIFormat depthStencilFormat = RHIFormat::D24_UNORM_S8_UINT;
        RHISampleCount sampleCount = RHISampleCount::Count1;
        bool reverseZ = false;

        // Optional bytecode overrides keep validation tests independent from a
        // platform shader compiler while production uses shaderDirectory.
        std::vector<uint8> vertexShaderBytecode;
        std::vector<uint8> pixelShaderBytecode;
    };

    /**
     * @brief Particle renderer - handles all particle rendering modes
     */
    class ParticleRenderer
    {
    public:
        ParticleRenderer() = default;
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

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Draw particles for an instance
         * @param ctx Command context
         * @param instance Particle system instance
         * @param view View data
         * @param depthTexture Scene depth texture (for soft particles)
         */
        bool DrawParticles(RHICommandContext& ctx,
                          ParticleSystemInstance* instance,
                          const ViewData& view,
                          RHITexture* depthTexture);

        /**
         * @brief Draw particles with indirect draw
         */
        bool DrawParticlesIndirect(RHICommandContext& ctx,
                                   ParticleSystemInstance* instance,
                                   const ViewData& view,
                                   RHITexture* depthTexture);

        // =====================================================================
        // Pipeline Access
        // =====================================================================

        RHIPipeline* GetBillboardPipeline(ParticleBlendMode blend, bool softParticle);
        RHIPipeline* GetStretchedBillboardPipeline(ParticleBlendMode blend, bool softParticle);
        RHIPipeline* GetMeshPipeline(ParticleBlendMode blend);
        RHIPipeline* GetTrailPipeline(ParticleBlendMode blend, bool softParticle);

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
        uint32 MakePipelineKey(ParticleRenderMode mode, ParticleBlendMode blend, bool soft);
        RHIPipeline* CreatePipelineIfNeeded(ParticleRenderMode mode, ParticleBlendMode blend, bool soft);
        const char* GetShaderNameForMode(ParticleRenderMode mode) const;
        void CreateQuadBuffers();
        bool ValidateConfig();
        bool CreateSharedResources();
        bool CreateShaders();
        bool CreateDescriptorLayout();
        bool CreateFallbackTextureResources();
        RHIDescriptorSetRef CreateParticleDescriptorSet(ParticleSystemInstance* instance);
        void UploadRenderConstants(const ViewData& view, const SoftParticleConfig& softConfig);
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
        RHISamplerRef m_sampler;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;

        // Trail renderer
        std::unique_ptr<TrailRenderer> m_trailRenderer;
    };

} // namespace RVX::Particle
