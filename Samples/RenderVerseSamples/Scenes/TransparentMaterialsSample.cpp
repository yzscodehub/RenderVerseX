/** @file TransparentMaterialsSample.cpp @brief Cooked-only GFX15 transparency scene. */

#include "Scenes/TransparentMaterialsSample.h"

#include "Core/MathTypes.h"
#include "Particle/ECS/ParticleEcsBridge.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <string>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr const char* CookedSmokeEffectAssetId = "particle-smoke-v1";
        constexpr uint64 SmokeConfigurationRevision = 2u;
        constexpr uint64 SmokeRestartSequence = 1u;

        const SampleInfo TransparentMaterialsInfo = []
        {
            SampleInfo info;
            info.id = "transparent-materials";
            info.displayName = "Transparent Materials";
            info.description =
                "Qualifies cooked AlphaBlend, Premultiplied, Additive, and ThinTransmission materials with mixed mesh and smoke weighted-blended OIT.";
            info.defaultAssetId = "transparent-materials-v1";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.environmentPolicy = SampleEnvironmentPolicy::None;
            info.supportsRenderPathSelection = true;
            info.defaultRenderPath = SampleRenderPath::Auto;
            info.requiredCookedAssetIds = {
                "transparent-materials-v1",
                CookedSmokeEffectAssetId};
            info.domain = SampleDomain::Rendering;
            info.intent = SampleIntent::Regression;
            info.visualContractId = "transparent-materials-v1";
            info.deterministicQualification = true;
            info.defaultEffectAssetId = CookedSmokeEffectAssetId;
            return info;
        }();

        const AssessmentCapability TranslucencyCapability{
            AssessmentCode("RENDER.CAPABILITY.SORTED_TRANSLUCENCY"),
            "The renderer executes the four cooked GFX15 translucency material classes with a copied opaque snapshot and reactive-mask output.",
            true,
            "Qualification requires renderer-owned category draw, ordering, snapshot, and reactive-mask receipts."};

        const AssessmentCapability WeightedBlendedOITCapability{
            AssessmentCode("RENDER.CAPABILITY.WEIGHTED_BLENDED_OIT"),
            "The renderer executes weighted-blended OIT with one clear, mixed mesh and particle accumulation contributors, and one composite on the presented main view.",
            true,
            "Qualification requires the exact translucent-feature receipt to prove clear, both weighted contributor classes, and composite graph work in order without fallback."};

        void PublishCapability(SampleAssessmentChannel& assessment,
                               const AssessmentCapability& capability,
                               bool& published)
        {
            if (published)
            {
                return;
            }
            AssessmentCapabilityObservation observation;
            observation.capability = capability;
            observation.checkpoint = AssessmentCheckpoints::ScenarioStable;
            observation.value = DiagnosticValue<bool>::Available(true);
            observation.reason =
                "The exact presented frame reports all four cooked translucent classes plus mixed mesh and particle weighted accumulation without a renderer fallback.";
            if (assessment.MarkCapabilityObservation(std::move(observation)))
            {
                published = true;
            }
        }

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(-0.30f, -0.65f, -0.40f)),
                .sunColor = Vec3(1.0f, 0.91f, 0.78f),
                .zenithColor = Vec3(0.025f, 0.050f, 0.110f),
                .horizonColor = Vec3(0.12f, 0.18f, 0.28f),
                .groundColor = Vec3(0.010f, 0.012f, 0.018f),
                .scatteringIntensity = 0.45f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.93f, 0.84f),
                .intensity = 15000.0f,
                .intensityUnit = SceneECS::LightIntensityUnit::Lux,
                .castsShadows = true,
            };
        }

        [[nodiscard]] bool HasTranslucencyExecution(
            const SampleRenderDiagnostics& diagnostics)
        {
            return diagnostics.transparentAlphaBlendDrawCount != 0 &&
                diagnostics.transparentPremultipliedDrawCount != 0 &&
                diagnostics.transparentAdditiveDrawCount != 0 &&
                diagnostics.transparentThinTransmissionDrawCount != 0 &&
                diagnostics.transparentOpaqueSceneColorPyramidReady &&
                diagnostics.transparentOpaqueSceneColorPyramidBuilt &&
                diagnostics.transparentReactiveMaskAttachmentBound &&
                diagnostics.transparentReactiveMaskWriteDrawCount != 0 &&
                diagnostics.transparentPriorityBucketCount >= 4 &&
                diagnostics.transparentFallbackCount == 0;
        }

        [[nodiscard]] bool HasWeightedBlendedOITExecution(
            const SampleRenderDiagnostics& diagnostics)
        {
            const SampleWeightedBlendedOITExecutionDiagnostics& execution =
                diagnostics.weightedBlendedOITActualExecution;
            return execution.HasQualifiedExecution() &&
                   execution.meshWeightedGraphPassCount >= 1u &&
                   execution.particleWeightedGraphPassCount >= 1u;
        }

        [[nodiscard]] Particle::ParticleEcsConfig MakeSmokeEmitterConfig(
            const LoadedSampleParticleEffect& effect,
            const uint32 deterministicSeed,
            const bool autoPlay,
            const uint64 configurationRevision = 1u)
        {
            return {
                .systemAssetId = {effect.GetResourceId()},
                .configurationRevision = configurationRevision,
                .deterministicSeed = deterministicSeed,
                .emissionRateScale = 1.0f,
                .simulationSpeed = 1.0f,
                .maxParticleCount = 192u,
                .autoPlay = autoPlay,
                .visible = true,
                .simulateWhenHidden = false,
            };
        }

        bool CreateSmokeEmitter(SampleContext& context,
                                const LoadedSampleParticleEffect& effect,
                                SceneECS::SceneEntityRef& outEmitter,
                                std::string& outError)
        {
            if (!effect.IsReady() || !effect.GetResourceId())
            {
                outError =
                    "transparent-materials cannot create smoke emitter before its cooked effect is resident";
                return false;
            }

            const Particle::ParticleEcsConfig config = MakeSmokeEmitterConfig(
                effect, context.options.deterministicSeed, false);
            const Particle::ParticleEcsIntent intent{
                .command = Particle::ParticleEcsCommand::None,
                .sequence = 0,
            };
            outEmitter = context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, SceneECS::ParticleRuntimeTag{}, config, intent,
                Particle::ParticleEcsState{});
            if (!outEmitter.IsValid())
            {
                outError =
                    "transparent-materials failed to create its smoke ECS emitter";
                return false;
            }
            return true;
        }

        bool BeginSmokeDeterministicEpoch(
            SampleContext& context,
            const LoadedSampleParticleEffect& effect,
            const SceneECS::SceneEntityRef emitter,
            bool& outEpochStarted,
            bool& outResetQueued,
            std::string& outError)
        {
            if (!effect.IsReady() || !emitter.IsValid())
            {
                outError =
                    "transparent-materials deterministic epoch began before its cooked smoke emitter was ready";
                return false;
            }

            SceneECS::SceneCommandBuffer commands =
                context.scene.CreateCommandBuffer();
            const Particle::ParticleEcsConfig resetConfig =
                MakeSmokeEmitterConfig(effect, context.options.deterministicSeed,
                                       true, SmokeConfigurationRevision);
            const Particle::ParticleEcsIntent restart{
                .command = Particle::ParticleEcsCommand::Restart,
                .sequence = SmokeRestartSequence,
            };
            const SceneECS::SceneCommandReceipt configurationReceipt =
                commands.Set(emitter.entity, resetConfig);
            const SceneECS::SceneCommandReceipt restartReceipt =
                commands.Set(emitter.entity, restart);
            const SceneECS::SceneCommandBufferReceipt submission =
                context.scene.SubmitCommandBuffer(
                    std::move(commands),
                    SceneECS::SceneCommandBarrier::BeginSimulation);
            if (configurationReceipt.IsRejected() || restartReceipt.IsRejected() ||
                submission.IsRejected() || submission.IsDiscarded())
            {
                outError =
                    "transparent-materials could not queue its atomic smoke emitter reset";
                return false;
            }

            outEpochStarted = true;
            outResetQueued = true;
            return true;
        }

        [[nodiscard]] bool HasAppliedSmokeDeterministicEpoch(
            SampleContext& context,
            const SceneECS::SceneEntityRef emitter,
            const uint32 deterministicSeed) noexcept
        {
            const Particle::ParticleEcsConfig* const config =
                context.scene.GetRegistry().TryGet<Particle::ParticleEcsConfig>(
                    emitter.entity);
            const Particle::ParticleEcsIntent* const intent =
                context.scene.GetRegistry().TryGet<Particle::ParticleEcsIntent>(
                    emitter.entity);
            const Particle::ParticleEcsState* const state =
                context.scene.GetRegistry().TryGet<Particle::ParticleEcsState>(
                    emitter.entity);
            return config != nullptr && intent != nullptr && state != nullptr &&
                   config->autoPlay &&
                   config->configurationRevision == SmokeConfigurationRevision &&
                   config->deterministicSeed == deterministicSeed &&
                   intent->command == Particle::ParticleEcsCommand::Restart &&
                   intent->sequence == SmokeRestartSequence &&
                   state->lastAppliedConfigurationRevision ==
                       SmokeConfigurationRevision &&
                   state->lastConsumedCommandSequence == SmokeRestartSequence;
        }

        [[nodiscard]] std::string DescribeModelFailure(
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status)
        {
            if (!status.diagnostic.empty())
            {
                return status.diagnostic;
            }
            if (!status.request.error.message.empty())
            {
                return status.request.error.message;
            }
            return "Transparent-materials cooked request reached a terminal state.";
        }
    } // namespace

    const SampleInfo& TransparentMaterialsSample::GetInfo() const noexcept
    {
        return TransparentMaterialsInfo;
    }

    SampleAssessmentContract TransparentMaterialsSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.TRANSPARENT_MATERIALS");
        contract.revision = "3";
        contract.capabilities = {TranslucencyCapability,
                                 WeightedBlendedOITCapability};
        return contract;
    }

    bool TransparentMaterialsSample::Setup(SampleContext& context,
                                           std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        if (!ApplySampleRenderPathPolicy(
                m_renderPath, context.renderSettings.gpuCulling, outError))
        {
            return false;
        }

        const float32 aspect = static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(38.0f), aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 1.35f, 5.5f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 1.35f, 0.0f)))
        {
            outError = "Failed to configure the transparent-materials ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the transparent-materials ECS skybox";
            return false;
        }
        m_skyCreated = true;
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeDirectionalLight(), SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the transparent-materials directional light";
            return false;
        }
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024u : 2048u;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1u : 3u;
        context.renderSettings.shadows.maxDistance = 50.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;

        // All material classification, sort ordering, snapshot production,
        // reactive-mask attachment, and blend state resolution are renderer
        // owned. The Sample supplies only public camera/light/post settings.
        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.enableVelocityOutput = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        postProcess.enableBloom = true;
        postProcess.enableColorGrading = false;
        postProcess.enableSSR = false;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = 9.0f;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.bloomThreshold = 1.35f;
        postProcess.bloomIntensity = 0.18f;
        postProcess.bloomRadius = 0.50f;

        // This scenario requests the public GFX22 capability. The renderer
        // alone resolves its quality/capability policy; readiness stays false
        // until the presented receipt proves physical OIT graph execution.
        m_weightedBlendedOITRequested = true;

        if (context.options.effectAssetId != CookedSmokeEffectAssetId ||
            context.options.effectAssetIds.size() != 1u ||
            context.options.effectAssetIds.front() != CookedSmokeEffectAssetId)
        {
            outError =
                "transparent-materials requires the runner-admitted cooked particle-smoke-v1 effect";
            return false;
        }
        if (context.options.deterministicSeed == 0u)
        {
            outError =
                "transparent-materials requires a non-zero deterministic seed for mixed weighted OIT";
            return false;
        }
        if (!context.models.RequestByAssetId(
                context.options.assetId, m_model, outError))
        {
            return false;
        }
        if (!context.particleEffects.RequestByAssetId(
                CookedSmokeEffectAssetId, m_smokeEffect, outError))
        {
            static_cast<void>(context.models.Cancel(m_model));
            return false;
        }
        return true;
    }

    bool TransparentMaterialsSample::BeginDeterministicEpoch(
        SampleContext& context,
        std::string& outError)
    {
        if (context.options.fixedTimeNumerator != 1u ||
            context.options.fixedTimeDenominator != 60u)
        {
            outError =
                "transparent-materials requires the manifest's fixed 1/60 second epoch";
            return false;
        }
        return BeginSmokeDeterministicEpoch(
            context, m_smokeEffect, m_smokeEmitter,
            m_deterministicEpochStarted, m_epochResetQueued, outError);
    }

    void TransparentMaterialsSample::Update(SampleContext& context,
                                            float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal() && !m_model.IsFullyResident())
        {
            m_failure = DescribeModelFailure(status);
            return;
        }

        context.particleEffects.UpdateReadiness(m_smokeEffect);
        if (m_smokeEffect.state == SampleParticleEffectLoadState::Failed ||
            m_smokeEffect.state == SampleParticleEffectLoadState::Cancelled)
        {
            m_failure = m_smokeEffect.diagnostic.empty()
                ? "transparent-materials cooked smoke effect reached a terminal failure"
                : m_smokeEffect.diagnostic;
            return;
        }
        if (m_smokeEffect.IsReady() && !m_smokeEmitter.IsValid() &&
            !TryCreateSmokeEmitter(context, m_failure))
        {
            return;
        }
        if (m_deterministicEpochStarted &&
            !HasAppliedSmokeDeterministicEpoch(
                context, m_smokeEmitter, context.options.deterministicSeed))
        {
            // The atomic reset is consumed at BeginSimulation.  Remain
            // pending until this public ECS state proves the pre-roll packet
            // cannot contribute to the qualified OIT frame.
            return;
        }
        if (m_deterministicEpochStarted)
        {
            m_epochResetApplied = true;
        }
    }

    bool TransparentMaterialsSample::TryCreateSmokeEmitter(
        SampleContext& context,
        std::string& outError)
    {
        return CreateSmokeEmitter(context, m_smokeEffect, m_smokeEmitter,
                                  outError);
    }

    void TransparentMaterialsSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void TransparentMaterialsSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() &&
            m_smokeEffect.IsReady() && m_smokeEmitter.IsValid() &&
            m_epochResetApplied &&
            m_translucencyExecutionObserved &&
            m_weightedBlendedOITExecutionObserved;
        reporter.SetScenarioContractRevision(3);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_failure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("CookedTransparentMaterialsAssetAdmissionRequired");
        reporter.Enable("AlphaBlendRequested");
        reporter.Enable("PremultipliedBlendRequested");
        reporter.Enable("AdditiveBlendRequested");
        reporter.Enable("ThinTransmissionRequested");
        reporter.Enable("SortedTranslucencyRequested");
        reporter.Enable("CookedParticleSmokeEffectAdmissionRequired");
        reporter.Enable("MixedMeshParticleWeightedBlendedOITRequested");
        if (m_weightedBlendedOITRequested)
        {
            reporter.Enable("WeightedBlendedOITRequested");
        }
        reporter.ResourceDiagnostic(
            "transparent-materials requires catalog-admitted cooked model "
            "transparent-materials-v1 and effect particle-smoke-v1; source fallback is forbidden");
        if (m_model.IsFullyResident())
        {
            reporter.Enable("CookedTransparentMaterialsAssetResident");
        }
        if (m_smokeEffect.IsReady())
        {
            reporter.Enable("CookedParticleSmokeEffectResident");
        }
        if (m_smokeEmitter.IsValid())
        {
            reporter.Enable("TransparentMaterialsSmokeEcsEmitterCreated");
        }
        if (m_epochResetQueued)
        {
            reporter.Enable("TransparentMaterialsSmokeDeterministicEpochResetQueued");
        }
        if (m_epochResetApplied)
        {
            reporter.Enable("TransparentMaterialsSmokeDeterministicEpochResetApplied");
        }
        if (m_skyCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        if (m_translucencyExecutionObserved)
        {
            reporter.Enable("TransparentMaterialsActualExecutionQualified");
        }
        if (m_weightedBlendedOITExecutionObserved)
        {
            reporter.Enable("WeightedBlendedOITActualExecutionQualified");
        }
    }

    void TransparentMaterialsSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const bool executed = HasTranslucencyExecution(diagnostics);
        const bool weightedBlendedOITExecuted =
            HasWeightedBlendedOITExecution(diagnostics);
        m_translucencyExecutionObserved = m_translucencyExecutionObserved || executed;
        m_weightedBlendedOITExecutionObserved =
            m_weightedBlendedOITExecutionObserved || weightedBlendedOITExecuted;
        if (executed)
        {
            PublishCapability(assessment, TranslucencyCapability,
                              m_translucencyCapabilityPublished);
        }
        if (weightedBlendedOITExecuted)
        {
            PublishCapability(assessment, WeightedBlendedOITCapability,
                              m_weightedBlendedOITCapabilityPublished);
        }
    }

    SampleReadiness TransparentMaterialsSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
        {
            return SampleReadiness::Failed(m_failure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for its catalog-admitted cooked package");
        }
        if (!m_smokeEffect.IsReady() || !m_smokeEmitter.IsValid())
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for its catalog-admitted cooked smoke effect and ECS emitter");
        }
        if (!m_deterministicEpochStarted)
        {
            // The host starts the shared deterministic epoch only after
            // resident assets and public ECS state become ready. The final
            // contract below remains fail-closed after that reset.
            return SampleReadiness::Ready();
        }
        if (!m_epochResetQueued)
        {
            return SampleReadiness::Failed(
                "transparent-materials deterministic smoke epoch did not queue its ECS reset");
        }
        if (!m_epochResetApplied)
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for its deterministic smoke ECS reset to apply");
        }
        if (!HasTranslucencyExecution(diagnostics))
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for renderer-owned category draw, opaque snapshot, reactive-mask, sorting, and no-fallback receipts");
        }
        if (!HasWeightedBlendedOITExecution(diagnostics))
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for presented weighted-blended OIT clear, mixed mesh and particle accumulation, and composite receipts");
        }
        return SampleReadiness::Ready();
    }

    bool TransparentMaterialsSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        if (!readiness.IsReady())
        {
            outError = readiness.reason;
            return false;
        }
        outError.clear();
        return true;
    }

    void TransparentMaterialsSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context.models.Cancel(m_model));
        static_cast<void>(context.particleEffects.Cancel(m_smokeEffect));
        m_model = {};
        m_smokeEffect = {};
        m_smokeEmitter = {};
        m_renderPath = SampleRenderPath::Auto;
        m_skyCreated = false;
        m_lightCreated = false;
        m_weightedBlendedOITRequested = false;
        m_deterministicEpochStarted = false;
        m_epochResetQueued = false;
        m_epochResetApplied = false;
        m_translucencyExecutionObserved = false;
        m_translucencyCapabilityPublished = false;
        m_weightedBlendedOITExecutionObserved = false;
        m_weightedBlendedOITCapabilityPublished = false;
        m_failure.clear();
    }
} // namespace RVX
