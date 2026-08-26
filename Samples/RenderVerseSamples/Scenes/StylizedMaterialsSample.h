#pragma once

/** @file StylizedMaterialsSample.h @brief Cooked-only ToonLit qualification scene. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

#include <string>

namespace RVX
{
    /**
     * @brief A backend-neutral consumer of the cooked ToonLit sample package.
     *
     * The scene authors only world state and public RenderContracts settings.
     * It deliberately cannot fabricate a Toon material, create a render pass,
     * or substitute a source-imported model if its cooked package is absent.
     */
    class StylizedMaterialsSample final : public ISample
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
        bool m_skyCreated = false;
        bool m_lightCreated = false;
        bool m_toonLightingExecutionObserved = false;
        bool m_toonOutlineExecutionObserved = false;
        bool m_toonOutlineQualificationObserved = false;
        bool m_toonLightingCapabilityPublished = false;
        bool m_toonOutlineCapabilityPublished = false;
    };
} // namespace RVX
