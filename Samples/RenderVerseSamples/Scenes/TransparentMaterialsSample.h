#pragma once

/** @file TransparentMaterialsSample.h @brief Cooked-only GFX15 transparency scene. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

#include <string>

namespace RVX
{
    /**
     * @brief A fixed cooked package that demonstrates the four GFX15 surface
     * blend/shading contracts without authoring renderer state.
     */
    class TransparentMaterialsSample final : public ISample
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
        [[nodiscard]] SampleReadiness GetReadiness(
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
        bool m_translucencyExecutionObserved = false;
        bool m_translucencyCapabilityPublished = false;
    };
} // namespace RVX
