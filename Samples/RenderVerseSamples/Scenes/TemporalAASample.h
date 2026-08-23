#pragma once

/** @file TemporalAASample.h @brief Deterministic temporal anti-aliasing showcase. */

#include "Core/MathTypes.h"
#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

#include <string>

namespace RVX
{
    /**
     * @brief Exercises the renderer-owned temporal resolve with deterministic motion.
     *
     * The scenario owns only World state and public frame settings. It never
     * accesses history textures, RenderGraph nodes, pipelines, or RHI objects.
     */
    class TemporalAASample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        [[nodiscard]] SampleAssessmentContract
            GetAssessmentContract() const override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        void ObserveDiagnostics(const SampleRenderDiagnostics& diagnostics,
                                SampleAssessmentChannel& assessment) override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        LoadedSampleModel m_model;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        std::string m_modelFailure;
        Vec3 m_baseTranslation = Vec3(0.0f);
        float32 m_simulationSeconds = 0.0f;
        bool m_baseTransformCaptured = false;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
        bool m_temporalCapabilityPublished = false;
        bool m_temporalExecutionQualified = false;
    };
} // namespace RVX
