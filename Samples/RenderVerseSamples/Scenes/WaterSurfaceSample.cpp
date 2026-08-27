/** @file WaterSurfaceSample.cpp @brief Cooked-only deterministic Gerstner water scenario. */

#include "Scenes/WaterSurfaceSample.h"

#include "Core/MathTypes.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleFeatureReporter.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Samples/SampleWaterSurfaceDiagnostics.h"
#include "Scene/ECS/RenderFragments.h"
#include "Water/ECS/WaterEcsBridge.h"
#include "Water/GerstnerSurface.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace RVX
{
namespace
{
    constexpr const char* CookedWaterReferenceFixtureId =
        "water-surface-reference-fixture-v1";
    constexpr uint64 DeterministicConfigurationRevision = 2u;
    constexpr uint64 DeterministicRefreshSequence = 1u;
    constexpr uint32 WaterSurfaceAssetId = 18001u;
    constexpr uint32 WaterMaterialAssetId = 18002u;
    constexpr float64 GameplayQuerySeconds = 2.0;

    const AssessmentCapability GerstnerWaterSurfaceCapability{
        AssessmentCode("RENDER.CAPABILITY.GERSTNER_WATER_SURFACE"),
        "The renderer presents the immutable, canonical Gerstner water packet with no fallback.",
        true,
        "Qualification requires one exact presented water grid receipt."};
    const AssessmentCapability WaterDepthThicknessCapability{
        AssessmentCode("RENDER.CAPABILITY.WATER_DEPTH_THICKNESS"),
        "The renderer evaluates canonical depth linearization and scene-depth thickness for water.",
        true,
        "Qualification requires a presented depth/thickness renderer receipt."};
    const AssessmentCapability WaterBeerLambertCapability{
        AssessmentCode("RENDER.CAPABILITY.WATER_BEER_LAMBERT"),
        "The renderer applies bounded Beer-Lambert absorption to water thickness.",
        true,
        "Qualification requires a presented Beer-Lambert renderer receipt."};
    const AssessmentCapability WaterReactiveMaskCapability{
        AssessmentCode("RENDER.CAPABILITY.WATER_REACTIVE_MASK"),
        "The renderer writes water coverage into the reactive-mask attachment.",
        true,
        "Qualification requires one presented reactive-mask water write."};

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

    const SampleInfo WaterSurfaceInfo = []
    {
        SampleInfo info;
        info.id = "water-surface";
        info.displayName = "Water Surface";
        info.description =
            "Cooked-only deterministic Gerstner water with opaque-depth refraction, Beer-Lambert absorption, foam, and reactive-mask evidence.";
        info.assetPolicy = SampleAssetPolicy::Fixed;
        info.environmentPolicy = SampleEnvironmentPolicy::None;
        info.supportsRenderPathSelection = true;
        info.defaultRenderPath = SampleRenderPath::Direct;
        info.defaultAssetId = CookedWaterReferenceFixtureId;
        info.requiredCookedAssetIds = {CookedWaterReferenceFixtureId};
        info.domain = SampleDomain::Rendering;
        info.intent = SampleIntent::Regression;
        info.visualContractId = "water-surface-v1";
        info.deterministicQualification = true;
        return info;
    }();

    [[nodiscard]] SceneECS::Skybox MakeWaterSky()
    {
        return {
            .mode = SceneECS::SkyboxMode::Procedural,
            .sunDirection = normalize(Vec3(-0.30f, -0.88f, -0.36f)),
            .sunColor = Vec3(1.0f, 0.92f, 0.78f),
            .zenithColor = Vec3(0.018f, 0.075f, 0.160f),
            .horizonColor = Vec3(0.135f, 0.295f, 0.410f),
            .groundColor = Vec3(0.012f, 0.022f, 0.028f),
            .scatteringIntensity = 0.42f,
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

    [[nodiscard]] SceneECS::Light MakeWaterDirectionalLight()
    {
        return {
            .type = SceneECS::LightType::Directional,
            .color = Vec3(1.0f, 0.90f, 0.72f),
            .intensity = 950.0f,
            .intensityUnit = SceneECS::LightIntensityUnit::Lux,
            .shadowBias = 0.0008f,
            .castsShadows = true,
        };
    }

    [[nodiscard]] bool MakeCanonicalWaveSet(WaterGerstnerWaveSet& outWaves)
    {
        outWaves = {};
        outWaves.waveCount = 4u;
        outWaves.waves[0] = {
            .direction = Vec2(1.0f, 0.18f),
            .amplitude = 0.42f,
            .wavelength = 8.5f,
            .steepness = 0.38f,
            .speed = 1.18f,
            .phaseOffsetRadians = 0.0f,
        };
        outWaves.waves[1] = {
            .direction = Vec2(-0.22f, 1.0f),
            .amplitude = 0.22f,
            .wavelength = 4.2f,
            .steepness = 0.24f,
            .speed = 1.44f,
            .phaseOffsetRadians = 0.72f,
        };
        outWaves.waves[2] = {
            .direction = Vec2(0.72f, -0.66f),
            .amplitude = 0.12f,
            .wavelength = 2.1f,
            .steepness = 0.16f,
            .speed = 1.82f,
            .phaseOffsetRadians = -0.45f,
        };
        outWaves.waves[3] = {
            .direction = Vec2(-0.92f, -0.31f),
            .amplitude = 0.07f,
            .wavelength = 1.15f,
            .steepness = 0.08f,
            .speed = 2.26f,
            .phaseOffsetRadians = 1.18f,
        };
        return CanonicalizeWaterGerstnerWaveSet(outWaves);
    }

    [[nodiscard]] bool MakeWaterConfig(Water::WaterEcsConfig& outConfig,
                                        const uint64 revision)
    {
        WaterGerstnerWaveSet waves;
        if (!MakeCanonicalWaveSet(waves))
        {
            return false;
        }
        outConfig = {
            .surfaceAssetId = {WaterSurfaceAssetId},
            .materialAssetId = {WaterMaterialAssetId},
            .configurationRevision = revision,
            .size = {18.0f, 18.0f},
            .depth = 7.5f,
            .surfaceType = WaterRenderSnapshotSurfaceType::Lake,
            .simulationType = WaterRenderSnapshotSimulationType::Gerstner,
            .gerstnerWaves = waves,
            .shallowColor = {0.015f, 0.31f, 0.38f},
            .deepColor = {0.003f, 0.030f, 0.095f},
            .foamColor = {0.92f, 0.98f, 1.0f},
            .absorptionCoefficient = {0.18f, 0.065f, 0.018f},
            .transparency = 0.72f,
            .reflectionStrength = 0.68f,
            .refractionStrength = 0.075f,
            .roughness = 0.075f,
            .normalScale = 1.0f,
            .foamIntensity = 0.92f,
            .foamCrestThreshold = 0.58f,
            .autoActivate = true,
            .reflectionEnabled = true,
            .refractionEnabled = true,
            .foamEnabled = true,
        };
        return outConfig.gerstnerWaves.IsStructurallyValid();
    }

    [[nodiscard]] WaterRenderSnapshotPayload MakeGameplayPacket(
        const Water::WaterEcsConfig& config) noexcept
    {
        WaterRenderSnapshotPayload packet;
        packet.payloadStatus = WaterRenderSnapshotPayloadStatus::Ready;
        packet.componentId = 1u;
        packet.surfaceAssetId = config.surfaceAssetId;
        packet.materialAssetId = config.materialAssetId;
        packet.worldPosition = Vec3(0.0f);
        // A deliberately conservative envelope includes every canonical wave
        // displacement and mirrors the public gateway's Y-up contract.
        packet.worldBounds = AABB(Vec3(-32.0f, -32.0f, -32.0f),
                                  Vec3(32.0f, 32.0f, 32.0f));
        packet.size = config.size;
        packet.depth = config.depth;
        packet.surfaceType = config.surfaceType;
        packet.simulationType = config.simulationType;
        packet.gerstnerWaves = config.gerstnerWaves;
        packet.shallowColor = config.shallowColor;
        packet.deepColor = config.deepColor;
        packet.foamColor = config.foamColor;
        packet.absorptionCoefficient = config.absorptionCoefficient;
        packet.transparency = config.transparency;
        packet.reflectionStrength = config.reflectionStrength;
        packet.refractionStrength = config.refractionStrength;
        packet.roughness = config.roughness;
        packet.normalScale = config.normalScale;
        packet.foamIntensity = config.foamIntensity;
        packet.foamCrestThreshold = config.foamCrestThreshold;
        packet.reflectionEnabled = config.reflectionEnabled;
        packet.refractionEnabled = config.refractionEnabled;
        packet.foamEnabled = config.foamEnabled;
        packet.contentHash = packet.ComputeContentHash();
        return packet;
    }

    [[nodiscard]] bool HasGameplayQueryProof(
        const Water::WaterEcsConfig& config) noexcept
    {
        const WaterRenderSnapshotPayload packet = MakeGameplayPacket(config);
        const Water::GerstnerSurfaceEvaluation evaluated =
            Water::EvaluateGerstnerSurface(packet, Vec2(0.65f, -0.35f),
                                           GameplayQuerySeconds);
        const Water::GerstnerSignedVerticalDistance signedDistance =
            Water::EvaluateGerstnerSignedVerticalDistance(
                packet, Vec3(0.65f, 0.0f, -0.35f), GameplayQuerySeconds);
        return packet.IsStructurallyValid() && evaluated.valid &&
               signedDistance.valid && std::isfinite(evaluated.displacement.x) &&
               std::isfinite(evaluated.displacement.y) &&
               std::isfinite(evaluated.displacement.z) &&
               std::isfinite(evaluated.normal.x) &&
               std::isfinite(evaluated.normal.y) &&
               std::isfinite(evaluated.normal.z) &&
               length(evaluated.normal) > 0.99f &&
               std::isfinite(signedDistance.signedVerticalDistance);
    }

    [[nodiscard]] bool ConfigureWaterFrameSettings(SampleContext& context,
                                                     std::string& outError)
    {
        if (!ApplySampleRenderPathPolicy(context.options.renderPath,
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
        context.renderSettings.postProcess.exposureSettings.manualEV100 = 9.5f;
        context.renderSettings.postProcess.bloomThreshold = 1.0f;
        context.renderSettings.postProcess.bloomIntensity = 0.25f;
        context.renderSettings.postProcess.bloomRadius = 0.45f;
        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = 2048u;
        context.renderSettings.shadows.cascadeCount = 3u;
        context.renderSettings.shadows.maxDistance = 55.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;
        context.renderSettings.shadows.filterRadiusTexels = 1.0f;
        return true;
    }

    [[nodiscard]] bool ConfigureWaterCamera(SampleContext& context,
                                              std::string& outError)
    {
        const float32 aspect = static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(context.camera, radians(42.0f), aspect,
                                            0.05f, 120.0f) ||
            !context.cameras.SetPose(context.camera,
                                     {.position = {0.0f, 5.40f, 10.80f}}) ||
            !context.cameras.LookAt(context.camera, {0.0f, 0.20f, 0.0f}))
        {
            outError = "Failed to configure deterministic water surface camera";
            return false;
        }
        return true;
    }

    [[nodiscard]] std::string DescribeFixtureFailure(
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus& status)
    {
        if (!status.diagnostic.empty()) return status.diagnostic;
        if (!status.request.error.message.empty()) return status.request.error.message;
        return "Water surface opaque reference fixture reached a terminal state";
    }
} // namespace

const SampleInfo& WaterSurfaceSample::GetInfo() const noexcept
{
    return WaterSurfaceInfo;
}

SampleAssessmentContract WaterSurfaceSample::GetAssessmentContract() const
{
    SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
    contract.code = AssessmentCode("SAMPLE.WATER_SURFACE");
    contract.revision = "1";
    contract.capabilities = {
        GerstnerWaterSurfaceCapability,
        WaterDepthThicknessCapability,
        WaterBeerLambertCapability,
        WaterReactiveMaskCapability};
    return contract;
}

bool WaterSurfaceSample::Setup(SampleContext& context, std::string& outError)
{
    m_renderPath = context.options.renderPath;
    if (context.options.deterministicSeed == 0u ||
        context.options.pipelineRecipe == SamplePipelineRecipe::Minimal ||
        context.options.pipelineRecipe == SamplePipelineRecipe::Headless)
    {
        outError = "water-surface requires one admitted cooked reference fixture, raster recipe, and deterministic seed";
        return false;
    }
    if (!ConfigureWaterFrameSettings(context, outError) ||
        !ConfigureWaterCamera(context, outError))
    {
        return false;
    }
    if (!context.sceneLifetime.CreateAndAdoptWithFragments({}, MakeWaterSky()).IsValid())
    {
        outError = "Failed to create deterministic water surface sky";
        return false;
    }
    m_skyCreated = true;

    const Vec3 lightDirection = normalize(Vec3(-0.30f, -0.88f, -0.36f));
    SceneECS::RuntimeEntityDesc lightDesc;
    lightDesc.localTransform.rotation =
        MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f));
    if (!context.sceneLifetime.CreateAndAdoptWithFragments(
            lightDesc, MakeWaterDirectionalLight(), SceneECS::Visibility{}).IsValid())
    {
        outError = "Failed to create water surface shadow-eligible directional light";
        return false;
    }
    m_shadowLightCreated = true;

    if (!context.models.RequestByAssetId(CookedWaterReferenceFixtureId,
                                         m_opaqueReferenceFixture, outError))
    {
        return false;
    }

    Water::WaterEcsConfig config;
    if (!MakeWaterConfig(config, 1u))
    {
        outError = "Failed to construct canonical deterministic Gerstner wave set";
        static_cast<void>(context.models.Cancel(m_opaqueReferenceFixture));
        return false;
    }
    SceneECS::RuntimeEntityDesc surfaceDesc;
    surfaceDesc.localTransform.translation = Vec3(0.0f);
    surfaceDesc.bounds = {.center = Vec3(0.0f), .extents = {10.0f, 10.0f, 10.0f}};
    m_surface = context.sceneLifetime.CreateAndAdoptWithFragments(
        surfaceDesc, SceneECS::WaterRuntimeTag{}, config,
        Water::WaterEcsIntent{}, Water::WaterEcsState{});
    if (!m_surface.IsValid())
    {
        outError = "Failed to create deterministic Water ECS surface";
        static_cast<void>(context.models.Cancel(m_opaqueReferenceFixture));
        return false;
    }
    return true;
}

bool WaterSurfaceSample::BeginDeterministicEpoch(SampleContext& context,
                                                 std::string& outError)
{
    if (context.options.fixedTimeNumerator != 1u ||
        context.options.fixedTimeDenominator != 60u || !m_surface.IsValid())
    {
        outError = "water-surface requires the manifest's fixed 1/60 second epoch";
        return false;
    }
    const Water::WaterEcsConfig* current =
        context.scene.GetRegistry().TryGet<Water::WaterEcsConfig>(m_surface.entity);
    if (current == nullptr)
    {
        outError = "water-surface deterministic epoch could not read its ECS configuration";
        return false;
    }
    Water::WaterEcsConfig refreshed = *current;
    refreshed.configurationRevision = DeterministicConfigurationRevision;
    const Water::WaterEcsIntent refresh{
        .command = Water::WaterEcsCommand::Refresh,
        .sequence = DeterministicRefreshSequence,
    };
    SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
    const SceneECS::SceneCommandReceipt configReceipt =
        commands.Set(m_surface.entity, refreshed);
    const SceneECS::SceneCommandReceipt refreshReceipt =
        commands.Set(m_surface.entity, refresh);
    const SceneECS::SceneCommandBufferReceipt submission =
        context.scene.SubmitCommandBuffer(
            std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
    if (configReceipt.IsRejected() || refreshReceipt.IsRejected() ||
        submission.IsRejected() || submission.IsDiscarded())
    {
        outError = "water-surface deterministic epoch could not queue the ECS refresh";
        return false;
    }
    m_deterministicEpochStarted = true;
    return true;
}

void WaterSurfaceSample::Update(SampleContext& context, float deltaTime)
{
    static_cast<void>(deltaTime);
    const ResourceSceneAdapters::EcsSceneAssetLoadStatus fixtureStatus =
        context.models.UpdateReadiness(m_opaqueReferenceFixture);
    if (fixtureStatus.IsTerminal())
    {
        m_failure = DescribeFixtureFailure(fixtureStatus);
        return;
    }
    if (!m_deterministicEpochStarted || !m_surface.IsValid())
    {
        return;
    }
    const Water::WaterEcsConfig* config =
        context.scene.GetRegistry().TryGet<Water::WaterEcsConfig>(m_surface.entity);
    const Water::WaterEcsIntent* intent =
        context.scene.GetRegistry().TryGet<Water::WaterEcsIntent>(m_surface.entity);
    const Water::WaterEcsState* state =
        context.scene.GetRegistry().TryGet<Water::WaterEcsState>(m_surface.entity);
    if (config == nullptr || intent == nullptr || state == nullptr)
    {
        m_failure = "water-surface ECS fragments disappeared before deterministic proof";
        return;
    }
    m_ecsWaterApplied = state->status == Water::WaterEcsBindingStatus::Active &&
        config->configurationRevision == DeterministicConfigurationRevision &&
        intent->command == Water::WaterEcsCommand::Refresh &&
        intent->sequence == DeterministicRefreshSequence &&
        state->lastAppliedConfigurationRevision ==
            DeterministicConfigurationRevision &&
        state->lastConsumedCommandSequence == DeterministicRefreshSequence;
    if (m_ecsWaterApplied)
    {
        m_gameplayQueryObserved = m_gameplayQueryObserved ||
            HasGameplayQueryProof(*config);
        if (!m_gameplayQueryObserved)
        {
            m_failure = "water-surface public Gerstner gameplay query did not return a valid finite result";
        }
    }
}

void WaterSurfaceSample::OnInput(SampleContext& context)
{
    static_cast<void>(context);
}

void WaterSurfaceSample::AppendReport(SampleFeatureReporter& reporter) const
{
    const bool stable = m_ecsWaterApplied && m_gameplayQueryObserved &&
        m_directOpaqueExecutionObserved && m_executionObserved;
    reporter.SetScenarioContractRevision(1);
    reporter.SetScenarioPhase(stable ? "stable" :
                              m_failure.empty() ? "pending" : "failed");
    reporter.Enable("CookedWaterSurfaceReferenceAdmissionRequired");
    reporter.Enable("GerstnerWaterSurfaceRequested");
    reporter.Enable("WaterDepthThicknessRequested");
    reporter.Enable("WaterBeerLambertRequested");
    reporter.Enable("WaterReactiveMaskRequested");
    AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
    reporter.ResourceDiagnostic(
        "water-surface admits only the package-only Model V3 opaque reference fixture; water waves are immutable public ECS values");
    if (m_opaqueReferenceFixture.IsFullyResident())
    {
        reporter.Enable("CookedWaterSurfaceReferenceResident");
    }
    if (m_skyCreated) reporter.Enable("WaterSurfaceProceduralSkyCreated");
    if (m_shadowLightCreated) reporter.Enable("WaterSurfaceShadowLightCreated");
    if (m_deterministicEpochStarted) reporter.Enable("WaterSurfaceDeterministicEpochStarted");
    if (m_ecsWaterApplied) reporter.Enable("WaterSurfaceEcsRefreshApplied");
    if (m_gameplayQueryObserved) reporter.Enable("WaterSurfaceGameplayQueryQualified");
    if (m_directOpaqueExecutionObserved) reporter.Enable("DirectOpaqueReferenceExecutionQualified");
    if (m_executionObserved) reporter.Enable("WaterSurfaceActualExecutionQualified");
}

void WaterSurfaceSample::ObserveDiagnostics(
    const SampleRenderDiagnostics& diagnostics,
    SampleAssessmentChannel& assessment)
{
    m_directOpaqueExecutionObserved = m_directOpaqueExecutionObserved ||
        IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics);
    const bool executed = m_deterministicEpochStarted && m_ecsWaterApplied &&
        m_gameplayQueryObserved &&
        GetSampleWaterSurfaceExecutionDiagnostics(diagnostics).HasWaterExecution();
    m_executionObserved = m_executionObserved || executed;
    if (!executed)
    {
        return;
    }
    PublishCapability(assessment, GerstnerWaterSurfaceCapability,
                      "The exact presented renderer frame executed one canonical Gerstner water grid with no fallback.",
                      m_surfaceCapabilityPublished);
    PublishCapability(assessment, WaterDepthThicknessCapability,
                      "The exact presented renderer frame copied opaque/depth inputs and evaluated canonical linearized thickness.",
                      m_depthCapabilityPublished);
    PublishCapability(assessment, WaterBeerLambertCapability,
                      "The exact presented renderer frame evaluated Fresnel, Beer-Lambert absorption, and foam from immutable water data.",
                      m_beerLambertCapabilityPublished);
    PublishCapability(assessment, WaterReactiveMaskCapability,
                      "The exact presented renderer frame wrote water coverage to the reactive-mask attachment.",
                      m_reactiveMaskCapabilityPublished);
}

SampleReadiness WaterSurfaceSample::GetReadiness(
    const SampleRenderDiagnostics& diagnostics) const
{
    if (!m_failure.empty()) return SampleReadiness::Failed(m_failure);
    if (!m_opaqueReferenceFixture.IsFullyResident())
    {
        return SampleReadiness::Pending(
            "water-surface is waiting for its cooked opaque reference fixture");
    }
    if (!m_surface.IsValid())
    {
        return SampleReadiness::Failed("water-surface did not create its ECS surface");
    }
    if (!m_skyCreated || !m_shadowLightCreated)
    {
        return SampleReadiness::Failed(
            "water-surface did not create its fixed procedural lighting environment");
    }
    if (!m_deterministicEpochStarted)
    {
        return SampleReadiness::Ready();
    }
    if (!m_ecsWaterApplied)
    {
        return SampleReadiness::Pending(
            "water-surface is waiting for its deterministic ECS refresh");
    }
    if (!m_gameplayQueryObserved)
    {
        return SampleReadiness::Pending(
            "water-surface is waiting for its public Gerstner gameplay query proof");
    }
    if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
    {
        return SampleReadiness::Pending(
            "water-surface is waiting for a presented direct opaque reference draw");
    }
    if (!GetSampleWaterSurfaceExecutionDiagnostics(diagnostics).HasWaterExecution())
    {
        return SampleReadiness::Pending(
            "water-surface is waiting for a presentation-aligned water grid, snapshot, shading, and reactive-mask receipt");
    }
    return SampleReadiness::Ready();
}

bool WaterSurfaceSample::ValidateResult(const SampleRenderDiagnostics& diagnostics,
                                        std::string& outError) const
{
    const SampleReadiness readiness = GetReadiness(diagnostics);
    outError = readiness.reason;
    return readiness.IsReady();
}

void WaterSurfaceSample::Shutdown(SampleContext& context)
{
    static_cast<void>(context.models.Cancel(m_opaqueReferenceFixture));
}
} // namespace RVX
