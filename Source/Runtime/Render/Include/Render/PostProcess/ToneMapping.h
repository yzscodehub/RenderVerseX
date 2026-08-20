#pragma once

/**
 * @file ToneMapping.h
 * @brief Tone mapping post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"
#include "Render/PostProcess/ToneMappingTypes.h"
#include "Render/RenderDiagnostics.h"


namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    /**
     * @brief Tone mapping post-process pass
     * 
     * Converts HDR scene color to LDR with gamma correction.
     */
    class ToneMappingPass : public IPostProcessPass
    {
    public:
        ToneMappingPass();
        ~ToneMappingPass() override = default;

        const char* GetName() const override { return "ToneMapping"; }
        int32 GetPriority() const override { return 900; }  // Near the end

        void Configure(const PostProcessSettings& settings) override;
        void AddToGraph(RenderGraph& graph, RGTextureHandle input, RGTextureHandle output) override;

        /** @brief Arm an optional, one-frame raw input probe before graph build. */
        void SetPixelProbeRequest(const RenderFrameCaptureRequest& request,
                                  uint64 frameSequence,
                                  uint64 requiredSceneRevision,
                                  uint64 runtimeSurfaceGeneration);

        /**
         * @brief Map the raw probe after the carrying submission has completed.
         * @warning The caller must establish completion for frameSequence first.
         */
        [[nodiscard]] bool CompletePixelProbe(uint64 requestId,
                                              uint64 frameSequence,
                                              RenderFramePixelProbeResult& outResult);

        /**
         * @brief Provide GPU resources required by the fullscreen ToneMapping path
         */
        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);

        // =========================================================================
        // Configuration
        // =========================================================================

        void SetOperator(ToneMappingOperator op) { m_operator = op; }
        ToneMappingOperator GetOperator() const { return m_operator; }

        void SetExposure(float exposure) { m_exposure = exposure; }
        float GetExposure() const { return m_exposure; }

        void SetGamma(float gamma) { m_gamma = gamma; }
        float GetGamma() const { return m_gamma; }

        void SetWhitePoint(float whitePoint) { m_whitePoint = whitePoint; }
        float GetWhitePoint() const { return m_whitePoint; }

        void SetOutputColorSpace(ToneMappingOutputColorSpace colorSpace) { m_outputColorSpace = colorSpace; }
        ToneMappingOutputColorSpace GetOutputColorSpace() const { return m_outputColorSpace; }

    private:
        struct PixelProbeReadback
        {
            RenderFramePixelProbeResult result{};
            RHIBufferRef buffer{};
            uint32 rowPitch = 0;
            bool armed = false;
            bool recorded = false;
        };

        bool EnsureRuntimeResources();
        bool UpdateConstants(uint32 width,
                             uint32 height,
                             ToneMappingOperator op,
                             ToneMappingOutputColorSpace outputColorSpace,
                             float exposure,
                             float gamma,
                             float whitePoint);

        ToneMappingOperator m_operator = ToneMappingOperator::ACES;
        ToneMappingOutputColorSpace m_outputColorSpace = ToneMappingOutputColorSpace::SRGB;
        float m_exposure = 1.0f;
        float m_gamma = 2.2f;
        float m_whitePoint = 11.2f;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        IRHIDevice* m_resourceDevice = nullptr;
        RHIBufferRef m_constantBuffer;
        RHISamplerRef m_sampler;
        PixelProbeReadback m_pixelProbe{};
    };

} // namespace RVX
