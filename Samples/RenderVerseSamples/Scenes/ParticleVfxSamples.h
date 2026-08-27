#pragma once

/** @file ParticleVfxSamples.h @brief Cooked-only fire and smoke VFX samples. */

#include "Samples/Sample.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleParticleEffectLoader.h"

#include "Scene/ECS/SceneEcsRuntime.h"

#include <string>

namespace RVX
{
    class ParticleFireSample final : public ISample
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
        bool TryCreateEmitter(SampleContext& context, std::string& outError);

        LoadedSampleParticleEffect m_effect;
        LoadedSampleModel m_opaqueGroundCaster;
        SceneECS::SceneEntityRef m_emitter{};
        std::string m_failure;
        SampleRenderPath m_renderPath = SampleRenderPath::Direct;
        bool m_deterministicEpochStarted = false;
        bool m_epochResetQueued = false;
        bool m_epochResetApplied = false;
        bool m_executionObserved = false;
        bool m_directOpaqueExecutionObserved = false;
        bool m_capabilityPublished = false;
        bool m_distortionCapabilityPublished = false;
        bool m_reactiveMaskCapabilityPublished = false;
        bool m_lightHintCapabilityPublished = false;
        bool m_skyCreated = false;
        bool m_shadowLightCreated = false;
    };

    class ParticleSmokeSample final : public ISample
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
        bool TryCreateEmitter(SampleContext& context, std::string& outError);

        LoadedSampleParticleEffect m_effect;
        LoadedSampleModel m_opaqueGroundCaster;
        SceneECS::SceneEntityRef m_emitter{};
        std::string m_failure;
        SampleRenderPath m_renderPath = SampleRenderPath::Direct;
        bool m_deterministicEpochStarted = false;
        bool m_epochResetQueued = false;
        bool m_epochResetApplied = false;
        bool m_executionObserved = false;
        bool m_directOpaqueExecutionObserved = false;
        bool m_particleCapabilityPublished = false;
        bool m_softDepthCapabilityPublished = false;
        bool m_reactiveMaskCapabilityPublished = false;
        bool m_skyCreated = false;
        bool m_shadowLightCreated = false;
    };
} // namespace RVX
