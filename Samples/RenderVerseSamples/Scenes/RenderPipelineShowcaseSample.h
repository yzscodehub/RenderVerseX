#pragma once

/** @file RenderPipelineShowcaseSample.h @brief Engine pass-chain scene. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

namespace RVX
{
    /** @brief Configures scene intent while Render owns pass scheduling. */
    class RenderPipelineShowcaseSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        bool Setup(SampleContext& context, std::string& outError) override;
        void Update(SampleContext& context, float deltaTime) override;
        void OnInput(SampleContext& context) override;
        void AppendReport(SampleFeatureReporter& reporter) const override;
        SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        LoadedSampleModel m_model;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        std::string m_modelFailure;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
    };
} // namespace RVX
