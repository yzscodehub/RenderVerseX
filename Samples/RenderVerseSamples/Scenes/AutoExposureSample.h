#pragma once

/** @file AutoExposureSample.h @brief Deterministic histogram auto-exposure scenario. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <string>

namespace RVX
{
    /**
     * @brief Drives neutral, bright, then dark ECS lighting at fixed sample ticks.
     *
     * The scenario deliberately changes only a public World light. The renderer
     * owns histogram construction, exposure adaptation, temporal history, and
     * all post-process resource access.
     */
    class AutoExposureSample final : public ISample
    {
    public:
        [[nodiscard]] const SampleInfo& GetInfo() const noexcept override;
        [[nodiscard]] SampleAssessmentContract
            GetAssessmentContract() const override;
        bool Setup(SampleContext& context, std::string& outError) override;
        bool BeginDeterministicEpoch(SampleContext& context,
                                     std::string& outError) override;
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
        enum class LuminanceStage : uint8
        {
            Neutral = 0,
            Bright,
            Dark
        };

        [[nodiscard]] bool ApplyLuminanceStage(SampleContext& context,
                                               LuminanceStage stage);

        LoadedSampleModel m_model;
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        SceneECS::SceneEntityRef m_directionalLight;
        std::string m_modelFailure;
        uint32 m_fixedStepCount = 0;
        LuminanceStage m_luminanceStage = LuminanceStage::Neutral;
        bool m_skyboxCreated = false;
        bool m_lightCreated = false;
        bool m_brightStageObserved = false;
        bool m_darkStageObserved = false;
        bool m_autoExposureCapabilityPublished = false;
        bool m_autoExposureExecutionQualified = false;
    };
} // namespace RVX
