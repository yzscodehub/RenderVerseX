#pragma once

/** @file WaterSurfaceSample.h @brief Cooked-only deterministic Gerstner water scenario. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"

#include "Scene/ECS/SceneEcsRuntime.h"

#include <string>

namespace RVX
{
    /**
     * @brief One renderer-neutral water surface paired with an opaque cooked fixture.
     *
     * The scenario owns only public World/ECS values, a frozen gameplay query,
     * and semantic assessment observations.  Render/RHI resources and passes
     * remain entirely on the host/composition side of the boundary.
     */
    class WaterSurfaceSample final : public ISample
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
        [[nodiscard]] SampleReadiness GetReadiness(
            const SampleRenderDiagnostics& diagnostics) const override;
        bool ValidateResult(const SampleRenderDiagnostics& diagnostics,
                            std::string& outError) const override;
        void Shutdown(SampleContext& context) override;

    private:
        LoadedSampleModel m_opaqueReferenceFixture;
        SceneECS::SceneEntityRef m_surface{};
        std::string m_failure;
        SampleRenderPath m_renderPath = SampleRenderPath::Direct;
        bool m_deterministicEpochStarted = false;
        bool m_ecsWaterApplied = false;
        bool m_gameplayQueryObserved = false;
        bool m_directOpaqueExecutionObserved = false;
        bool m_executionObserved = false;
        bool m_skyCreated = false;
        bool m_shadowLightCreated = false;
        bool m_surfaceCapabilityPublished = false;
        bool m_depthCapabilityPublished = false;
        bool m_beerLambertCapabilityPublished = false;
        bool m_reactiveMaskCapabilityPublished = false;
    };
} // namespace RVX
