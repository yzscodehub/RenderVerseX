#pragma once

/**
 * @file ToneMapping.h
 * @brief Tone mapping post-process effect
 */

#include "Render/PostProcess/PostProcessStack.h"

namespace RVX
{
    /**
     * @brief Tone mapping operator types
     */
    enum class ToneMappingOperator : uint8
    {
        Reinhard,           // Simple Reinhard
        ReinhardExtended,   // Extended Reinhard with white point
        ACES,               // ACES filmic
        Uncharted2,         // Filmic curve from Uncharted 2
        Neutral,            // Neutral tonemapper
        None                // No tone mapping (pass-through)
    };

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

    private:
        ToneMappingOperator m_operator = ToneMappingOperator::ACES;
        float m_exposure = 1.0f;
        float m_gamma = 2.2f;
        float m_whitePoint = 11.2f;
    };

} // namespace RVX
