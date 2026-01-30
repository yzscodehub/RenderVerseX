/**
 * @file SSAO.h
 * @brief Screen-Space Ambient Occlusion effect
 * 
 * Implements GTAO (Ground Truth Ambient Occlusion) or HBAO+.
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <memory>
#include <vector>

namespace RVX
{
    class IRHIDevice;
    class RHICommandContext;
    class RHITexture;

    /**
     * @brief SSAO quality preset
     */
    enum class SSAOQuality : uint8
    {
        Low,        ///< Fast, lower quality (4 samples)
        Medium,     ///< Balanced (8 samples)
        High,       ///< High quality (16 samples)
        Ultra       ///< Maximum quality (32 samples)
    };

    /**
     * @brief SSAO configuration
     */
    struct SSAOConfig
    {
        SSAOQuality quality = SSAOQuality::Medium;
        
        float radius = 0.5f;            ///< Sample radius in world units
        float intensity = 1.0f;         ///< AO intensity multiplier
        float bias = 0.025f;            ///< Depth bias to reduce self-occlusion
        float power = 2.0f;             ///< Power curve for AO falloff
        
        float fadeStart = 50.0f;        ///< Distance to start fading out AO
        float fadeEnd = 100.0f;         ///< Distance where AO is fully faded
        
        bool halfResolution = false;    ///< Render at half resolution
        bool useNormals = true;         ///< Use normal buffer for improved quality
        bool temporalFilter = true;     ///< Enable temporal filtering
        
        int blurPasses = 2;             ///< Number of bilateral blur passes
        float blurSharpness = 8.0f;     ///< Bilateral blur edge sharpness
    };

    /**
     * @brief Screen-Space Ambient Occlusion
     * 
     * Implements high-quality SSAO using ground truth approximation.
     * 
     * Features:
     * - Multi-scale sampling for accurate AO
     * - Bilateral blur for edge-aware filtering
     * - Temporal accumulation for stability
     * - Half-resolution option for performance
     */
    class SSAO
    {
    public:
        SSAO() = default;
        ~SSAO();

        // Non-copyable
        SSAO(const SSAO&) = delete;
        SSAO& operator=(const SSAO&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize SSAO
         */
        void Initialize(IRHIDevice* device, uint32 width, uint32 height);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Resize internal buffers
         */
        void Resize(uint32 width, uint32 height);

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
        const SSAOConfig& GetConfig() const { return m_config; }

        /**
         * @brief Set configuration
         */
        void SetConfig(const SSAOConfig& config);

        /**
         * @brief Enable/disable SSAO
         */
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsEnabled() const { return m_enabled; }

        // =========================================================================
        // Rendering
        // =========================================================================

        /**
         * @brief Compute SSAO
         * @param ctx Command context
         * @param depthTexture Scene depth buffer
         * @param normalTexture Scene normal buffer (optional but recommended)
         * @param viewMatrix Camera view matrix
         * @param projMatrix Camera projection matrix
         */
        void Compute(RHICommandContext& ctx,
                     RHITexture* depthTexture,
                     RHITexture* normalTexture,
                     const Mat4& viewMatrix,
                     const Mat4& projMatrix);

        /**
         * @brief Get the AO result texture
         */
        RHITexture* GetResult() const { return m_aoResult.Get(); }

        /**
         * @brief Get the blurred AO texture
         */
        RHITexture* GetBlurredResult() const { return m_aoBlurred.Get(); }

    private:
        void CreateResources(uint32 width, uint32 height);
        void CreateNoiseTexture();
        void CreateSampleKernel();
        void ComputeSSAO(RHICommandContext& ctx, RHITexture* depth, RHITexture* normal);
        void BlurSSAO(RHICommandContext& ctx, RHITexture* depth);

        IRHIDevice* m_device = nullptr;
        SSAOConfig m_config;
        bool m_enabled = true;

        uint32 m_width = 0;
        uint32 m_height = 0;

        // Render targets
        RHITextureRef m_aoResult;
        RHITextureRef m_aoBlurred;
        RHITextureRef m_aoHistory;  // For temporal filtering

        // Resources
        RHITextureRef m_noiseTexture;
        RHIBufferRef m_sampleKernelBuffer;
        RHIBufferRef m_constantBuffer;

        // Pipelines
        RHIPipelineRef m_ssaoPipeline;
        RHIPipelineRef m_blurHPipeline;
        RHIPipelineRef m_blurVPipeline;
        RHIPipelineRef m_temporalPipeline;

        // Sample kernel
        std::vector<Vec4> m_sampleKernel;
    };

} // namespace RVX
