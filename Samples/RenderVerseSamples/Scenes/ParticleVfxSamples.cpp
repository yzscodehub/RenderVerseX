/** @file ParticleVfxSamples.cpp @brief Cooked-only deterministic particle scenarios. */

#include "Scenes/ParticleVfxSamples.h"

#include "Core/MathTypes.h"
#include "Particle/ECS/ParticleEcsBridge.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleParticleVfxDiagnostics.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 DeterministicConfigurationRevision = 2u;
    constexpr uint64 DeterministicRestartSequence = 1u;
    constexpr const char* CookedOpaqueGroundCasterAssetId =
        "particle-vfx-ground-caster-v1";

    const AssessmentCapability CpuParticleBillboardCapability{
        AssessmentCode("RENDER.CAPABILITY.CPU_PARTICLE_BILLBOARD"),
        "The renderer executes a cooked CPU billboard packet with a validated atlas.",
        true,
        "Qualification requires an exact presented renderer particle receipt without fallback."};
    const AssessmentCapability ParticleSoftDepthCapability{
        AssessmentCode("RENDER.CAPABILITY.PARTICLE_SOFT_DEPTH"),
        "The renderer consumes the scene depth snapshot through its canonical linearization path.",
        true,
        "Qualification requires a required soft-depth receipt, not an optional fallback."};
    const AssessmentCapability ParticleDistortionCapability{
        AssessmentCode("RENDER.CAPABILITY.PARTICLE_DISTORTION"),
        "The renderer executes and composites the effect's distortion vector pass.",
        true,
        "Qualification requires a presentation-bound distortion receipt without fallback."};
    const AssessmentCapability ParticleReactiveMaskCapability{
        AssessmentCode("RENDER.CAPABILITY.PARTICLE_REACTIVE_MASK"),
        "The renderer writes reactive-mask coverage for the particle effect.",
        true,
        "Qualification requires a presentation-bound reactive-mask write receipt."};
    const AssessmentCapability ParticleLightHintCapability{
        AssessmentCode("RENDER.CAPABILITY.PARTICLE_LIGHT_HINT"),
        "The renderer admits the effect's single bounded low-frequency light hint.",
        true,
        "Qualification requires exactly one requested and admitted light hint."};

    void PublishCapability(SampleAssessmentChannel& assessment,
                           const AssessmentCapability& capability,
                           const char* reason,
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
        observation.reason = reason;
        if (assessment.MarkCapabilityObservation(std::move(observation)))
        {
            published = true;
        }
    }

    const SampleInfo FireInfo = []
    {
        SampleInfo info;
        info.id = "particle-fire";
        info.displayName = "Particle Fire";
        info.description =
            "Cooked CPU billboard fire with an HDR flipbook, additive blend, distortion, and reactive mask.";
        info.assetPolicy = SampleAssetPolicy::Fixed;
        info.environmentPolicy = SampleEnvironmentPolicy::None;
        info.supportsRenderPathSelection = true;
        info.defaultRenderPath = SampleRenderPath::Direct;
        // Keep the effect as the scenario's primary asset. The fixture is an
        // additional cooked model whose ground and low occluder make depth,
        // direct opaque rasterization, and directional shadows observable.
        info.additionalDefaultAssetIds = {CookedOpaqueGroundCasterAssetId};
        info.requiredCookedAssetIds = {
            "particle-fire-v1",
            CookedOpaqueGroundCasterAssetId};
        info.domain = SampleDomain::VFX;
        info.intent = SampleIntent::Regression;
        info.visualContractId = "particle-fire-v1";
        info.deterministicQualification = true;
        info.defaultEffectAssetId = "particle-fire-v1";
        return info;
    }();

    const SampleInfo SmokeInfo = []
    {
        SampleInfo info;
        info.id = "particle-smoke";
        info.displayName = "Particle Smoke";
        info.description =
            "Cooked CPU billboard premultiplied smoke with required soft depth and a reactive mask.";
        info.assetPolicy = SampleAssetPolicy::Fixed;
        info.environmentPolicy = SampleEnvironmentPolicy::None;
        info.supportsRenderPathSelection = true;
        info.defaultRenderPath = SampleRenderPath::Direct;
        info.additionalDefaultAssetIds = {CookedOpaqueGroundCasterAssetId};
        info.requiredCookedAssetIds = {
            "particle-smoke-v1",
            CookedOpaqueGroundCasterAssetId};
        info.domain = SampleDomain::VFX;
        info.intent = SampleIntent::Regression;
        info.visualContractId = "particle-smoke-v1";
        info.deterministicQualification = true;
        info.defaultEffectAssetId = "particle-smoke-v1";
        return info;
    }();

    [[nodiscard]] SceneECS::Skybox MakeVfxSky()
    {
        return {
            .mode = SceneECS::SkyboxMode::Procedural,
            .sunDirection = normalize(Vec3(-0.38f, -0.82f, -0.43f)),
            .sunColor = Vec3(1.0f, 0.72f, 0.46f),
            .zenithColor = Vec3(0.012f, 0.020f, 0.050f),
            .horizonColor = Vec3(0.080f, 0.105f, 0.175f),
            .groundColor = Vec3(0.018f, 0.022f, 0.034f),
            .scatteringIntensity = 0.22f,
            .contributesToLighting = false,
        };
    }

    [[nodiscard]] Quat MakeLookRotation(const Vec3& direction, const Vec3& up)
    {
        const Vec3 forward = length(direction) < 0.001f
                                 ? Vec3(0.0f, -1.0f, 0.0f)
                                 : normalize(direction);
        Vec3 right = cross(up, forward);
        right = length(right) < 0.001f ? Vec3(1.0f, 0.0f, 0.0f)
                                        : normalize(right);
        const Vec3 correctedUp = cross(forward, right);

        Mat4 rotation(1.0f);
        rotation[0] = Vec4(right, 0.0f);
        rotation[1] = Vec4(correctedUp, 0.0f);
        rotation[2] = Vec4(-forward, 0.0f);
        return Mat4ToQuat(rotation);
    }

    [[nodiscard]] SceneECS::Light MakeVfxDirectionalLight()
    {
        return {
            .type = SceneECS::LightType::Directional,
            .color = Vec3(1.0f, 0.78f, 0.58f),
            // The fixed EV100 showcase uses a physical outdoor-strength key
            // light so the matte ground remains legible beneath HDR fire.
            .intensity = 1200.0f,
            .intensityUnit = SceneECS::LightIntensityUnit::Lux,
            .shadowBias = 0.0008f,
            .castsShadows = true,
        };
    }

    [[nodiscard]] std::string DescribeOpaqueFixtureFailure(
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
        return "Particle VFX opaque ground/caster fixture reached a terminal state";
    }

    [[nodiscard]] bool ConfigureCamera(SampleContext& context,
                                       const Vec3 position,
                                       const Vec3 target,
                                       std::string& outError)
    {
        const float32 aspect = static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(38.0f), aspect, 0.05f, 80.0f) ||
            !context.cameras.SetPose(context.camera, {.position = position}) ||
            !context.cameras.LookAt(context.camera, target))
        {
            outError = "Failed to configure deterministic particle VFX camera";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ConfigureVfxFrameSettings(SampleContext& context,
                                                   std::string& outError)
    {
        if (!ApplySampleRenderPathPolicy(
                context.options.renderPath,
                context.renderSettings.gpuCulling,
                outError))
        {
            return false;
        }
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableVelocityOutput = true;
        context.renderSettings.postProcess.antiAliasingMode =
            RenderAntiAliasingMode::TAA;
        context.renderSettings.postProcess.enableBloom = true;
        context.renderSettings.postProcess.enableColorGrading = false;
        context.renderSettings.postProcess.enableSSR = false;
        context.renderSettings.postProcess.toneMappingOperator =
            RenderToneMappingOperator::ACES;
        context.renderSettings.postProcess.exposureSettings.mode =
            RenderExposureMode::ManualEV100;
        context.renderSettings.postProcess.exposureSettings.manualEV100 = 8.5f;
        context.renderSettings.postProcess.bloomThreshold = 0.8f;
        context.renderSettings.postProcess.bloomIntensity = 0.35f;
        context.renderSettings.postProcess.bloomRadius = 0.55f;
        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = 2048u;
        context.renderSettings.shadows.cascadeCount = 3u;
        context.renderSettings.shadows.maxDistance = 40.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;
        context.renderSettings.shadows.filterRadiusTexels = 1.0f;
        return true;
    }

    [[nodiscard]] Particle::ParticleEcsConfig MakeConfig(
        const LoadedSampleParticleEffect& effect,
        const uint32 deterministicSeed,
        const uint32 maxParticles,
        const bool autoPlay,
        const uint64 configurationRevision = 1u)
    {
        return {
            .systemAssetId = {effect.GetResourceId()},
            .configurationRevision = configurationRevision,
            .deterministicSeed = deterministicSeed,
            .emissionRateScale = 1.0f,
            .simulationSpeed = 1.0f,
            .maxParticleCount = maxParticles,
            .autoPlay = autoPlay,
            .visible = true,
            .simulateWhenHidden = false,
        };
    }

    template <typename SampleType>
    bool SetupVfxSample(SampleContext& context,
                        LoadedSampleParticleEffect& effect,
                        LoadedSampleModel& opaqueGroundCaster,
                        bool& skyCreated,
                        bool& shadowLightCreated,
                        const Vec3 cameraPosition,
                        const Vec3 target,
                        std::string& outError)
    {
        static_assert(std::is_same_v<SampleType, ParticleFireSample> ||
                      std::is_same_v<SampleType, ParticleSmokeSample>);
        if (context.options.effectAssetId.empty() ||
            context.options.effectAssetIds.size() != 1u ||
            context.options.effectAssetIds.front() != context.options.effectAssetId)
        {
            outError = "Particle VFX sample requires one runner-admitted cooked effect id";
            return false;
        }
        if (context.options.deterministicSeed == 0u ||
            context.options.pipelineRecipe == SamplePipelineRecipe::Minimal ||
            context.options.pipelineRecipe == SamplePipelineRecipe::Headless)
        {
            outError = "Particle VFX sample requires a deterministic raster recipe and seed";
            return false;
        }
        if (!ConfigureVfxFrameSettings(context, outError) ||
            !ConfigureCamera(context, cameraPosition, target, outError))
        {
            return false;
        }
        if (!context.sceneLifetime.CreateAndAdoptWithFragments({}, MakeVfxSky()).IsValid())
        {
            outError = "Failed to create particle VFX procedural sky";
            return false;
        }
        skyCreated = true;

        const Vec3 lightDirection = normalize(Vec3(-0.38f, -0.82f, -0.43f));
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc, MakeVfxDirectionalLight(), SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create particle VFX shadow-eligible directional light";
            return false;
        }
        shadowLightCreated = true;

        // This pre-cooked model is the sole opaque geometry in the showcase:
        // its broad lower plane is a soft-depth reference and its raised
        // center block receives and casts the directional shadow.
        if (!context.models.RequestByAssetId(
                CookedOpaqueGroundCasterAssetId,
                opaqueGroundCaster,
                outError))
        {
            return false;
        }
        if (!context.particleEffects.RequestByAssetId(
                context.options.effectAssetId, effect, outError))
        {
            static_cast<void>(context.models.Cancel(opaqueGroundCaster));
            return false;
        }
        return true;
    }

    bool CreateEmitter(SampleContext& context,
                       const LoadedSampleParticleEffect& effect,
                       const uint32 deterministicSeed,
                       const uint32 maxParticles,
                       SceneECS::SceneEntityRef& outEmitter,
                       std::string& outError)
    {
        if (!effect.IsReady() || !effect.GetResourceId())
        {
            outError = "Cooked particle effect is not resource-resident";
            return false;
        }
        // The readiness pre-roll may take an arbitrary number of host frames.
        // Keep the runtime stopped until BeginDeterministicEpoch has reset the
        // shared effect clock and atomically queued this entity's exact restart.
        const Particle::ParticleEcsConfig config = MakeConfig(
            effect, deterministicSeed, maxParticles, false);
        const Particle::ParticleEcsIntent intent{
            .command = Particle::ParticleEcsCommand::None,
            .sequence = 0,
        };
        outEmitter = context.sceneLifetime.CreateAndAdoptWithFragments(
            {},
            SceneECS::ParticleRuntimeTag{},
            config,
            intent,
            Particle::ParticleEcsState{});
        if (!outEmitter.IsValid())
        {
            outError = "Failed to create particle VFX ECS emitter";
            return false;
        }
        return true;
    }

    [[nodiscard]] bool BeginParticleDeterministicEpoch(
        SampleContext& context,
        const LoadedSampleParticleEffect& effect,
        const uint32 deterministicSeed,
        const uint32 maxParticles,
        const SceneECS::SceneEntityRef emitter,
        bool& outEpochStarted,
        bool& outResetQueued,
        std::string& outError)
    {
        if (!effect.IsReady() || !emitter.IsValid())
        {
            outError =
                "Particle VFX deterministic epoch began before the cooked emitter was ready";
            return false;
        }

        // Queue both fragments in one public Scene command buffer.  The
        // Particle bridge sees one configuration revision and its paired
        // restart command at the BeginSimulation boundary of deterministic
        // tick zero; no pre-roll simulation state can survive this boundary.
        SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
        const Particle::ParticleEcsConfig resetConfig = MakeConfig(
            effect, deterministicSeed, maxParticles, true,
            DeterministicConfigurationRevision);
        const Particle::ParticleEcsIntent restart{
            .command = Particle::ParticleEcsCommand::Restart,
            .sequence = DeterministicRestartSequence,
        };
        const SceneECS::SceneCommandReceipt configurationReceipt =
            commands.Set(emitter.entity, resetConfig);
        const SceneECS::SceneCommandReceipt restartReceipt =
            commands.Set(emitter.entity, restart);
        const SceneECS::SceneCommandBufferReceipt submission =
            context.scene.SubmitCommandBuffer(
                std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
        if (configurationReceipt.IsRejected() || restartReceipt.IsRejected() ||
            submission.IsRejected() || submission.IsDiscarded())
        {
            outError =
                "Particle VFX deterministic epoch could not queue the atomic ECS reset";
            return false;
        }
        outEpochStarted = true;
        outResetQueued = true;
        return true;
    }

    [[nodiscard]] bool HasAppliedParticleDeterministicEpoch(
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
               config->configurationRevision == DeterministicConfigurationRevision &&
               config->deterministicSeed == deterministicSeed &&
               intent->command == Particle::ParticleEcsCommand::Restart &&
               intent->sequence == DeterministicRestartSequence &&
               state->lastAppliedConfigurationRevision ==
                   DeterministicConfigurationRevision &&
               state->lastConsumedCommandSequence ==
                   DeterministicRestartSequence;
    }
} // namespace

const SampleInfo& ParticleFireSample::GetInfo() const noexcept { return FireInfo; }

SampleAssessmentContract ParticleFireSample::GetAssessmentContract() const
{
    SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
    contract.code = AssessmentCode("SAMPLE.PARTICLE_FIRE");
    contract.revision = "1";
    contract.capabilities = {
        CpuParticleBillboardCapability,
        ParticleDistortionCapability,
        ParticleReactiveMaskCapability,
        ParticleLightHintCapability};
    return contract;
}

bool ParticleFireSample::Setup(SampleContext& context, std::string& outError)
{
    m_renderPath = context.options.renderPath;
    return SetupVfxSample<ParticleFireSample>(
        context, m_effect, m_opaqueGroundCaster, m_skyCreated,
        m_shadowLightCreated, {0.0f, 2.10f, 5.60f},
        {0.0f, 0.60f, 0.0f}, outError);
}

bool ParticleFireSample::BeginDeterministicEpoch(
    SampleContext& context,
    std::string& outError)
{
    if (context.options.fixedTimeNumerator != 1u ||
        context.options.fixedTimeDenominator != 60u)
    {
        outError = "particle-fire requires the manifest's fixed 1/60 second epoch";
        return false;
    }
    return BeginParticleDeterministicEpoch(
        context, m_effect, context.options.deterministicSeed, 256u, m_emitter,
        m_deterministicEpochStarted, m_epochResetQueued, outError);
}

void ParticleFireSample::Update(SampleContext& context, float deltaTime)
{
    static_cast<void>(deltaTime);
    const ResourceSceneAdapters::EcsSceneAssetLoadStatus opaqueFixtureStatus =
        context.models.UpdateReadiness(m_opaqueGroundCaster);
    if (opaqueFixtureStatus.IsTerminal())
    {
        m_failure = DescribeOpaqueFixtureFailure(opaqueFixtureStatus);
        return;
    }
    context.particleEffects.UpdateReadiness(m_effect);
    if (m_effect.state == SampleParticleEffectLoadState::Failed ||
        m_effect.state == SampleParticleEffectLoadState::Cancelled)
    {
        m_failure = m_effect.diagnostic;
        return;
    }
    if (m_effect.IsReady() && !m_emitter.IsValid() &&
        !TryCreateEmitter(context, m_failure))
    {
        return;
    }
    if (m_deterministicEpochStarted &&
        !HasAppliedParticleDeterministicEpoch(
            context, m_emitter, context.options.deterministicSeed))
    {
        // The ECS command buffer is consumed at BeginSimulation.  Remaining
        // pending for one host update is valid; do not accidentally restart
        // the CPU effect from a pre-roll frame.
        return;
    }
    if (m_deterministicEpochStarted)
    {
        m_epochResetApplied = true;
    }
}

void ParticleFireSample::OnInput(SampleContext& context) { static_cast<void>(context); }

bool ParticleFireSample::TryCreateEmitter(SampleContext& context, std::string& outError)
{
    return CreateEmitter(context, m_effect, context.options.deterministicSeed,
                         256u, m_emitter, outError);
}

void ParticleFireSample::AppendReport(SampleFeatureReporter& reporter) const
{
    const bool stable = m_epochResetApplied && m_executionObserved &&
                        m_directOpaqueExecutionObserved;
    reporter.SetScenarioContractRevision(1);
    reporter.SetScenarioPhase(stable ? "stable" :
                              m_failure.empty() ? "pending" : "failed");
    reporter.Enable("CookedParticleFireAdmissionRequired");
    reporter.Enable("CookedOpaqueGroundCasterAdmissionRequired");
    reporter.Enable("CpuBillboardRequested");
    reporter.Enable("AdditiveHdrFlipbookRequested");
    reporter.Enable("ParticleDistortionAndReactiveMaskRequested");
    AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
    if (m_epochResetQueued) reporter.Enable("ParticleFireDeterministicEpochResetQueued");
    if (m_epochResetApplied) reporter.Enable("ParticleFireDeterministicEpochResetApplied");
    reporter.ResourceDiagnostic(
        "particle-fire uses only catalog-admitted .rvxvfx, atlas, and opaque ground/caster artifacts");
    if (m_effect.IsReady()) reporter.Enable("CookedParticleFireResident");
    if (m_opaqueGroundCaster.IsFullyResident())
    {
        reporter.Enable("CookedOpaqueGroundCasterResident");
        reporter.Enable("OpaqueGroundAndOccluderFixture");
    }
    if (m_shadowLightCreated) reporter.Enable("ShadowEligibleDirectionalLight");
    if (m_emitter.IsValid()) reporter.Enable("ParticleFireEcsEmitterCreated");
    if (m_directOpaqueExecutionObserved) reporter.Enable("DirectOpaqueGroundExecutionQualified");
    if (m_executionObserved) reporter.Enable("ParticleFireActualExecutionQualified");
}

void ParticleFireSample::ObserveDiagnostics(
    const SampleRenderDiagnostics& diagnostics,
    SampleAssessmentChannel& assessment)
{
    m_directOpaqueExecutionObserved = m_directOpaqueExecutionObserved ||
        IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics);
    const bool executed = m_deterministicEpochStarted &&
        m_epochResetApplied &&
        GetSampleParticleVfxExecutionDiagnostics(diagnostics).HasFireExecution();
    m_executionObserved = m_executionObserved || executed;
    if (executed)
    {
        PublishCapability(
            assessment, CpuParticleBillboardCapability,
            "The exact presented frame executed the admitted additive CPU billboard fire packet with its atlas, distortion composite, reactive writes, and no fallback.",
            m_capabilityPublished);
        PublishCapability(
            assessment, ParticleDistortionCapability,
            "The exact presented fire receipt scheduled, executed, and composited distortion.",
            m_distortionCapabilityPublished);
        PublishCapability(
            assessment, ParticleReactiveMaskCapability,
            "The exact presented fire receipt wrote the reactive mask.",
            m_reactiveMaskCapabilityPublished);
        PublishCapability(
            assessment, ParticleLightHintCapability,
            "The exact presented fire receipt requested and admitted its one bounded light hint.",
            m_lightHintCapabilityPublished);
    }
}

SampleReadiness ParticleFireSample::GetReadiness(
    const SampleRenderDiagnostics& diagnostics) const
{
    if (!m_failure.empty()) return SampleReadiness::Failed(m_failure);
    if (!m_effect.IsReady() || !m_emitter.IsValid())
    {
        return SampleReadiness::Pending("particle-fire is waiting for its cooked effect and ECS emitter");
    }
    if (!m_opaqueGroundCaster.IsFullyResident())
    {
        return SampleReadiness::Pending(
            "particle-fire is waiting for its cooked opaque ground/caster fixture");
    }
    if (!m_shadowLightCreated)
    {
        return SampleReadiness::Failed(
            "particle-fire did not create its shadow-eligible directional light");
    }
    if (!m_deterministicEpochStarted)
    {
        // Host pre-roll needs a real resident emitter before it can reset the
        // shared effect clock. Execution checks begin only after that reset.
        return SampleReadiness::Ready();
    }
    if (!m_epochResetQueued)
    {
        return SampleReadiness::Failed(
            "particle-fire deterministic epoch did not queue its ECS reset");
    }
    if (!m_epochResetApplied)
    {
        return SampleReadiness::Pending(
            "particle-fire is waiting for its deterministic ECS reset to apply");
    }
    if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
    {
        return SampleReadiness::Pending(
            "particle-fire is waiting for a presented direct opaque ground/caster draw");
    }
    if (!GetSampleParticleVfxExecutionDiagnostics(diagnostics).HasFireExecution())
    {
        return SampleReadiness::Pending(
            "particle-fire is waiting for an exact additive flipbook, distortion, reactive-mask, and presentation-aligned particle receipt");
    }
    return SampleReadiness::Ready();
}

bool ParticleFireSample::ValidateResult(const SampleRenderDiagnostics& diagnostics,
                                        std::string& outError) const
{
    const SampleReadiness readiness = GetReadiness(diagnostics);
    outError = readiness.reason;
    return readiness.IsReady();
}

void ParticleFireSample::Shutdown(SampleContext& context)
{
    static_cast<void>(context.models.Cancel(m_opaqueGroundCaster));
    static_cast<void>(context.particleEffects.Cancel(m_effect));
}

const SampleInfo& ParticleSmokeSample::GetInfo() const noexcept { return SmokeInfo; }

SampleAssessmentContract ParticleSmokeSample::GetAssessmentContract() const
{
    SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
    contract.code = AssessmentCode("SAMPLE.PARTICLE_SMOKE");
    contract.revision = "1";
    contract.capabilities = {
        CpuParticleBillboardCapability,
        ParticleSoftDepthCapability,
        ParticleReactiveMaskCapability};
    return contract;
}

bool ParticleSmokeSample::Setup(SampleContext& context, std::string& outError)
{
    m_renderPath = context.options.renderPath;
    return SetupVfxSample<ParticleSmokeSample>(
        context, m_effect, m_opaqueGroundCaster, m_skyCreated,
        m_shadowLightCreated, {0.0f, 2.35f, 6.20f},
        {0.0f, 0.75f, 0.0f}, outError);
}

bool ParticleSmokeSample::BeginDeterministicEpoch(
    SampleContext& context,
    std::string& outError)
{
    if (context.options.fixedTimeNumerator != 1u ||
        context.options.fixedTimeDenominator != 60u)
    {
        outError = "particle-smoke requires the manifest's fixed 1/60 second epoch";
        return false;
    }
    return BeginParticleDeterministicEpoch(
        context, m_effect, context.options.deterministicSeed, 192u, m_emitter,
        m_deterministicEpochStarted, m_epochResetQueued, outError);
}

void ParticleSmokeSample::Update(SampleContext& context, float deltaTime)
{
    static_cast<void>(deltaTime);
    const ResourceSceneAdapters::EcsSceneAssetLoadStatus opaqueFixtureStatus =
        context.models.UpdateReadiness(m_opaqueGroundCaster);
    if (opaqueFixtureStatus.IsTerminal())
    {
        m_failure = DescribeOpaqueFixtureFailure(opaqueFixtureStatus);
        return;
    }
    context.particleEffects.UpdateReadiness(m_effect);
    if (m_effect.state == SampleParticleEffectLoadState::Failed ||
        m_effect.state == SampleParticleEffectLoadState::Cancelled)
    {
        m_failure = m_effect.diagnostic;
        return;
    }
    if (m_effect.IsReady() && !m_emitter.IsValid() &&
        !TryCreateEmitter(context, m_failure))
    {
        return;
    }
    if (m_deterministicEpochStarted &&
        !HasAppliedParticleDeterministicEpoch(
            context, m_emitter, context.options.deterministicSeed))
    {
        return;
    }
    if (m_deterministicEpochStarted)
    {
        m_epochResetApplied = true;
    }
}

void ParticleSmokeSample::OnInput(SampleContext& context) { static_cast<void>(context); }

bool ParticleSmokeSample::TryCreateEmitter(SampleContext& context, std::string& outError)
{
    return CreateEmitter(context, m_effect, context.options.deterministicSeed,
                         192u, m_emitter, outError);
}

void ParticleSmokeSample::AppendReport(SampleFeatureReporter& reporter) const
{
    const bool stable = m_epochResetApplied && m_executionObserved &&
                        m_directOpaqueExecutionObserved;
    reporter.SetScenarioContractRevision(1);
    reporter.SetScenarioPhase(stable ? "stable" :
                              m_failure.empty() ? "pending" : "failed");
    reporter.Enable("CookedParticleSmokeAdmissionRequired");
    reporter.Enable("CookedOpaqueGroundCasterAdmissionRequired");
    reporter.Enable("CpuBillboardRequested");
    reporter.Enable("PremultipliedSoftDepthRequested");
    reporter.Enable("ParticleReactiveMaskRequested");
    AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
    if (m_epochResetQueued) reporter.Enable("ParticleSmokeDeterministicEpochResetQueued");
    if (m_epochResetApplied) reporter.Enable("ParticleSmokeDeterministicEpochResetApplied");
    reporter.ResourceDiagnostic(
        "particle-smoke requires cooked .rvxvfx plus its catalog-admitted opaque depth ground/caster");
    if (m_effect.IsReady()) reporter.Enable("CookedParticleSmokeResident");
    if (m_opaqueGroundCaster.IsFullyResident())
    {
        reporter.Enable("CookedOpaqueGroundCasterResident");
        reporter.Enable("OpaqueGroundAndOccluderFixture");
    }
    if (m_shadowLightCreated) reporter.Enable("ShadowEligibleDirectionalLight");
    if (m_emitter.IsValid()) reporter.Enable("ParticleSmokeEcsEmitterCreated");
    if (m_directOpaqueExecutionObserved) reporter.Enable("DirectOpaqueGroundExecutionQualified");
    if (m_executionObserved) reporter.Enable("ParticleSmokeActualExecutionQualified");
}

void ParticleSmokeSample::ObserveDiagnostics(
    const SampleRenderDiagnostics& diagnostics,
    SampleAssessmentChannel& assessment)
{
    m_directOpaqueExecutionObserved = m_directOpaqueExecutionObserved ||
        IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics);
    const bool executed = m_deterministicEpochStarted &&
        m_epochResetApplied &&
        GetSampleParticleVfxExecutionDiagnostics(diagnostics).HasSmokeExecution();
    m_executionObserved = m_executionObserved || executed;
    if (executed)
    {
        PublishCapability(
            assessment, CpuParticleBillboardCapability,
            "The exact presented frame executed the admitted premultiplied CPU billboard smoke packet with its validated atlas and no fallback.",
            m_particleCapabilityPublished);
        PublishCapability(
            assessment, ParticleSoftDepthCapability,
            "The exact presented smoke frame completed required soft-depth snapshot, canonical linearization, and reactive-mask writes without an optional fallback.",
            m_softDepthCapabilityPublished);
        PublishCapability(
            assessment, ParticleReactiveMaskCapability,
            "The exact presented smoke receipt wrote the reactive mask.",
            m_reactiveMaskCapabilityPublished);
    }
}

SampleReadiness ParticleSmokeSample::GetReadiness(
    const SampleRenderDiagnostics& diagnostics) const
{
    if (!m_failure.empty()) return SampleReadiness::Failed(m_failure);
    if (!m_effect.IsReady() || !m_emitter.IsValid())
    {
        return SampleReadiness::Pending("particle-smoke is waiting for its cooked effect and ECS emitter");
    }
    if (!m_opaqueGroundCaster.IsFullyResident())
    {
        return SampleReadiness::Pending(
            "particle-smoke is waiting for its cooked opaque ground/caster fixture");
    }
    if (!m_shadowLightCreated)
    {
        return SampleReadiness::Failed(
            "particle-smoke did not create its shadow-eligible directional light");
    }
    if (!m_deterministicEpochStarted)
    {
        return SampleReadiness::Ready();
    }
    if (!m_epochResetQueued)
    {
        return SampleReadiness::Failed(
            "particle-smoke deterministic epoch did not queue its ECS reset");
    }
    if (!m_epochResetApplied)
    {
        return SampleReadiness::Pending(
            "particle-smoke is waiting for its deterministic ECS reset to apply");
    }
    if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
    {
        return SampleReadiness::Pending(
            "particle-smoke is waiting for a presented direct opaque ground/caster draw");
    }
    if (!GetSampleParticleVfxExecutionDiagnostics(diagnostics).HasSmokeExecution())
    {
        return SampleReadiness::Pending(
            "particle-smoke is waiting for a required soft-depth, premultiplied flipbook, reactive-mask, and presentation-aligned particle receipt");
    }
    return SampleReadiness::Ready();
}

bool ParticleSmokeSample::ValidateResult(const SampleRenderDiagnostics& diagnostics,
                                         std::string& outError) const
{
    const SampleReadiness readiness = GetReadiness(diagnostics);
    outError = readiness.reason;
    return readiness.IsReady();
}

void ParticleSmokeSample::Shutdown(SampleContext& context)
{
    static_cast<void>(context.models.Cancel(m_opaqueGroundCaster));
    static_cast<void>(context.particleEffects.Cancel(m_effect));
}
} // namespace RVX
