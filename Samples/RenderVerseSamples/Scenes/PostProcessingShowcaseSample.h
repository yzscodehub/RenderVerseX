#pragma once

/** @file PostProcessingShowcaseSample.h @brief Typed post-processing demonstration scene. */

#include "Samples/Sample.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"

namespace RVX
{
    /**
     * @brief Configures a deterministic scene and public post-process policy.
     *
     * The renderer owns all effect selection, resource lifetime, color-domain
     * transitions, and graph scheduling.  This scenario only creates public
     * World state and declares the requested public rendering settings.
     */
    class PostProcessingShowcaseSample final : public ISample
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
        LoadedSampleEnvironment m_environment;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        std::string m_modelFailure;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
        bool m_bloomCapabilityPublished = false;
        bool m_toneMappingCapabilityPublished = false;
        bool m_colorGradingCapabilityPublished = false;
        bool m_fxaaCapabilityPublished = false;
        bool m_ambientOcclusionCapabilityPublished = false;
        /** Exact latest presented-frame post-process qualification result. */
        bool m_postProcessExecutionQualified = false;
        /** Renderer execution is intentionally independent of the opt-in probe. */
        bool m_ambientOcclusionExecutionReady = false;
        /** True only after the explicit post-Present GTAO probe is complete. */
        bool m_ambientOcclusionQualificationReady = false;
    };
} // namespace RVX
