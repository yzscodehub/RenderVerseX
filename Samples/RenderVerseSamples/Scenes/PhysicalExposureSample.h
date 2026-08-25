#pragma once

/** @file PhysicalExposureSample.h @brief Deterministic Manual EV100 scenario. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <string>

namespace RVX
{
    /**
     * @brief Proves a fixed Manual EV100 policy through the public renderer contract.
     *
     * The sample authors only World state and RenderContracts values. Exposure
     * state, pre-exposure, post-processing resources, and presentation receipts
     * remain renderer-owned.
     */
    class PhysicalExposureSample final : public ISample
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
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
        bool m_manualExposureCapabilityPublished = false;
        bool m_manualExposureExecutionQualified = false;
    };
} // namespace RVX
