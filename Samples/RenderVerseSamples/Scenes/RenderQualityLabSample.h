#pragma once

/**
 * @file RenderQualityLabSample.h
 * @brief Deterministic cooked-only composition scene for graphics qualification.
 */

#include "Core/MathTypes.h"
#include "Samples/Sample.h"
#include "Samples/SampleEnvironmentLoader.h"
#include "Samples/SampleModelLoader.h"
#include "Samples/SampleParticleEffectLoader.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <cstddef>
#include <string>
#include <vector>

namespace RVX
{
    /**
     * @brief Public-only composite scene used by the graphics qualification
     *        harness.
     *
     * The scenario authors World/ECS entities and public frame policy only.
     * Renderer composition, graph scheduling, attachment capture, and metric
     * evaluation remain outside the Sample boundary.
     */
    class RenderQualityLabSample final : public ISample
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
        [[nodiscard]] bool PlaceModels(SampleContext& context,
                                       std::string& outError);
        [[nodiscard]] bool CreateParticleEmitter(SampleContext& context,
                                                 const LoadedSampleParticleEffect& effect,
                                                 const Vec3& position,
                                                 uint32 maxParticleCount,
                                                 uint64 deterministicInstanceKey,
                                                 SceneECS::SceneEntityRef& outEmitter,
                                                 std::string& outError);
        [[nodiscard]] bool QueueParticleEpochReset(SampleContext& context,
                                                   const LoadedSampleParticleEffect& effect,
                                                   SceneECS::SceneEntityRef emitter,
                                                   uint32 maxParticleCount,
                                                   uint64 configurationRevision,
                                                   uint64 restartSequence,
                                                   uint64 deterministicInstanceKey,
                                                   std::string& outError);
        [[nodiscard]] bool HasAppliedParticleEpoch(
            const SampleContext& context,
            const LoadedSampleParticleEffect& effect,
            SceneECS::SceneEntityRef emitter,
            uint32 maxParticleCount,
            uint64 configurationRevision,
            uint64 restartSequence,
            uint64 deterministicInstanceKey) const noexcept;
        [[nodiscard]] bool HasRequiredPresentation(
            const SampleRenderDiagnostics& diagnostics) const noexcept;

        /**
         * @brief Build the stable horizontal slots used by all lab captures.
         *
         * Keeping this small value-only policy in the scenario makes the
         * placement deterministic without exposing a renderer or graph type.
         */
        [[nodiscard]] static bool BuildDeterministicModelSlots(
            std::size_t modelCount,
            std::vector<Vec3>& outSlots) noexcept;
        [[nodiscard]] static bool GetDeterministicParticleInstanceKeys(
            uint64& outFireKey,
            uint64& outSmokeKey) noexcept;

        std::vector<LoadedSampleModel> m_models;
        LoadedSampleEnvironment m_environment;
        LoadedSampleParticleEffect m_particleEffect;
        LoadedSampleParticleEffect m_smokeEffect;
        SceneECS::SceneEntityRef m_waterSurface{};
        SceneECS::SceneEntityRef m_particleEmitter{};
        SceneECS::SceneEntityRef m_smokeEmitter{};
        SampleRenderPath m_renderPath = SampleRenderPath::Auto;
        std::string m_failure;
        bool m_environmentRequested = false;
        bool m_modelsPlaced = false;
        bool m_waterCreated = false;
        bool m_particleEpochStarted = false;
        bool m_particleEpochResetQueued = false;
        bool m_particleEpochApplied = false;
        bool m_smokeEpochResetQueued = false;
        bool m_smokeEpochApplied = false;
        bool m_directionalLightCreated = false;
        bool m_toneMappingObserved = false;
        bool m_temporalAAObserved = false;
        bool m_shadowObserved = false;
        bool m_translucencyObserved = false;
        bool m_particleObserved = false;
        bool m_smokeObserved = false;
        bool m_waterObserved = false;
        bool m_toneMappingCapabilityPublished = false;
        bool m_temporalAACapabilityPublished = false;
        bool m_shadowCapabilityPublished = false;
        bool m_translucencyCapabilityPublished = false;
        bool m_particleCapabilityPublished = false;
        bool m_waterCapabilityPublished = false;

        friend class RenderQualityLabSampleValidationAccess;
    };
} // namespace RVX
