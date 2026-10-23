/**
 * @file RenderQualityLabSample.cpp
 * @brief Deterministic cooked-only composition scene for graphics qualification.
 */

#include "Scenes/RenderQualityLabSample.h"

#include "Core/MathTypes.h"
#include "Particle/ECS/ParticleEcsBridge.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderQualityDiagnostics.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Samples/SampleWaterSurfaceDiagnostics.h"
#include "Scene/ECS/RenderFragments.h"
#include "Water/ECS/WaterEcsBridge.h"
#include "Water/GerstnerSurface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr const char* RenderQualityLabPrimaryAssetId =
            "post-processing-interior-v1";
        constexpr const char* RenderQualityLabEnvironmentAssetId =
            "pbr-reference-environment-cooked-v1";
        constexpr const char* RenderQualityLabTransparentAssetId =
            "transparent-materials-v1";
        constexpr const char* RenderQualityLabWaterReferenceAssetId =
            "water-surface-reference-fixture-v1";
        constexpr const char* RenderQualityLabParticleGroundAssetId =
            "particle-vfx-ground-caster-v1";
        constexpr const char* RenderQualityLabFireAssetId = "particle-fire-v1";
        constexpr const char* RenderQualityLabSmokeAssetId = "particle-smoke-v1";
        constexpr uint64 RenderQualityLabParticleConfigurationRevision = 2u;
        constexpr uint64 RenderQualityLabParticleRestartSequence = 1u;
        constexpr uint64 RenderQualityLabFireDeterministicInstanceKey =
            0x5256585f46495245ull;
        constexpr uint64 RenderQualityLabSmokeConfigurationRevision = 3u;
        constexpr uint64 RenderQualityLabSmokeRestartSequence = 2u;
        constexpr uint64 RenderQualityLabSmokeDeterministicInstanceKey =
            0x5256585f534d4f4bull;
        constexpr uint32 RenderQualityLabWaterSurfaceAssetId = 24001u;
        constexpr uint32 RenderQualityLabWaterMaterialAssetId = 24002u;

        const AssessmentCapability ToneMappingCapability{
            AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING"),
            "The lab presents the renderer-owned HDR-to-display tone-mapping stage.",
            true,
            "The lab requires a presentation-aligned tone-mapping receipt."};
        const AssessmentCapability TemporalAACapability{
            AssessmentCode("RENDER.CAPABILITY.TEMPORAL_AA"),
            "The lab presents a renderer-owned temporal resolve with committed history.",
            true,
            "The lab requires a presentation-aligned temporal resolve receipt."};
        const AssessmentCapability DirectionalShadowCapability{
            AssessmentCode("RENDER.CAPABILITY.DIRECTIONAL_CSM"),
            "The lab presents directional cascaded shadows on the composed scene.",
            true,
            "The lab requires completed directional shadow output and sampling."};
        const AssessmentCapability TranslucencyCapability{
            AssessmentCode("RENDER.CAPABILITY.SORTED_TRANSLUCENCY"),
            "The lab presents cooked transparent material packets through the sorted translucency path.",
            true,
            "The lab requires an ordered, snapshot-backed translucent receipt."};
        const AssessmentCapability ParticleCapability{
            AssessmentCode("RENDER.CAPABILITY.CPU_PARTICLE_BILLBOARD"),
            "The lab presents the cooked deterministic CPU billboard fire effect.",
            true,
            "The lab requires a presentation-aligned particle execution receipt."};
        const AssessmentCapability WaterCapability{
            AssessmentCode("RENDER.CAPABILITY.GERSTNER_WATER_SURFACE"),
            "The lab presents the cooked-scene Gerstner water surface contract.",
            true,
            "The lab requires a presentation-aligned water execution receipt."};

        const SampleInfo RenderQualityLabInfo = []
        {
            SampleInfo info;
            info.id = "render-quality-lab";
            info.displayName = "Render Quality Lab";
            info.description =
                "Composes cooked PBR, transparent materials, deterministic fire, water, shadows, and temporal post processing for graphics qualification.";
            info.defaultAssetId = RenderQualityLabPrimaryAssetId;
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.defaultEnvironmentId = RenderQualityLabEnvironmentAssetId;
            info.environmentPolicy = SampleEnvironmentPolicy::Required;
            info.supportsRenderPathSelection = true;
            info.defaultRenderPath = SampleRenderPath::Auto;
            info.additionalDefaultAssetIds = {
                RenderQualityLabTransparentAssetId,
                RenderQualityLabWaterReferenceAssetId,
                RenderQualityLabParticleGroundAssetId};
            info.requiredCookedAssetIds = {
                RenderQualityLabPrimaryAssetId,
                RenderQualityLabEnvironmentAssetId,
                RenderQualityLabTransparentAssetId,
                RenderQualityLabWaterReferenceAssetId,
                RenderQualityLabParticleGroundAssetId,
                RenderQualityLabFireAssetId,
                RenderQualityLabSmokeAssetId};
            info.defaultEffectAssetId = RenderQualityLabFireAssetId;
            info.requiredEffectAssetIds = {
                RenderQualityLabFireAssetId,
                RenderQualityLabSmokeAssetId};
            info.domain = SampleDomain::Qualification;
            info.intent = SampleIntent::Regression;
            info.visualContractId = "render-quality-lab-v3";
            info.deterministicQualification = true;
            return info;
        }();

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

        [[nodiscard]] Quat MakeLookRotation(const Vec3& direction,
                                             const Vec3& up)
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

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.94f, 0.84f),
                // The generated HDR fixture is authored around unit radiance.
                // Keep direct-light screen exposure equivalent to the former
                // 12000 lux / EV100 8.5 pairing while bringing the environment
                // into a useful qualification range: both values move by six
                // stops, so the directional contribution is unchanged.
                .intensity = 187.5f,
                .intensityUnit = SceneECS::LightIntensityUnit::Lux,
                .shadowBias = 0.0008f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] bool MakeCanonicalWaterConfig(
            Water::WaterEcsConfig& outConfig)
        {
            WaterGerstnerWaveSet waves;
            waves.waveCount = 4u;
            waves.waves[0] = {
                .direction = Vec2(1.0f, 0.18f),
                .amplitude = 0.42f,
                .wavelength = 8.5f,
                .steepness = 0.38f,
                .speed = 1.18f,
                .phaseOffsetRadians = 0.0f,
            };
            waves.waves[1] = {
                .direction = Vec2(-0.22f, 1.0f),
                .amplitude = 0.22f,
                .wavelength = 4.2f,
                .steepness = 0.24f,
                .speed = 1.44f,
                .phaseOffsetRadians = 0.72f,
            };
            waves.waves[2] = {
                .direction = Vec2(0.72f, -0.66f),
                .amplitude = 0.12f,
                .wavelength = 2.1f,
                .steepness = 0.16f,
                .speed = 1.82f,
                .phaseOffsetRadians = -0.45f,
            };
            waves.waves[3] = {
                .direction = Vec2(-0.92f, -0.31f),
                .amplitude = 0.07f,
                .wavelength = 1.15f,
                .steepness = 0.08f,
                .speed = 2.26f,
                .phaseOffsetRadians = 1.18f,
            };
            if (!CanonicalizeWaterGerstnerWaveSet(waves))
            {
                return false;
            }

            outConfig = {
                .surfaceAssetId = {RenderQualityLabWaterSurfaceAssetId},
                .materialAssetId = {RenderQualityLabWaterMaterialAssetId},
                .configurationRevision = 1u,
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

        [[nodiscard]] Particle::ParticleEcsConfig MakeParticleConfig(
            const LoadedSampleParticleEffect& effect,
            const uint32 seed,
            const uint64 deterministicInstanceKey,
            const uint32 maxParticleCount,
            const bool autoPlay,
            const uint64 revision)
        {
            return {
                .systemAssetId = {effect.GetResourceId()},
                .configurationRevision = revision,
                .deterministicSeed = seed,
                .deterministicInstanceKey = deterministicInstanceKey,
                .emissionRateScale = 1.0f,
                .simulationSpeed = 1.0f,
                .maxParticleCount = maxParticleCount,
                .autoPlay = autoPlay,
                .visible = true,
                .simulateWhenHidden = false,
            };
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
            return "RenderQualityLab model request reached a terminal state";
        }

        [[nodiscard]] std::string DescribeEnvironmentFailure(
            const ResourceSceneAdapters::EcsEnvironmentLoadStatus& status)
        {
            if (!status.diagnostic.empty())
            {
                return status.diagnostic;
            }
            if (!status.request.error.message.empty())
            {
                return status.request.error.message;
            }
            return "RenderQualityLab environment request reached a terminal state";
        }
    } // namespace

    const SampleInfo& RenderQualityLabSample::GetInfo() const noexcept
    {
        return RenderQualityLabInfo;
    }

    SampleAssessmentContract RenderQualityLabSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.RENDER_QUALITY_LAB");
        contract.revision = "1";
        contract.capabilities = {
            ToneMappingCapability,
            TemporalAACapability,
            DirectionalShadowCapability,
            TranslucencyCapability,
            ParticleCapability,
            WaterCapability};
        return contract;
    }

    bool RenderQualityLabSample::Setup(SampleContext& context,
                                       std::string& outError)
    {
        m_models.clear();
        m_environment = {};
        m_particleEffect = {};
        m_smokeEffect = {};
        m_waterSurface = {};
        m_particleEmitter = {};
        m_smokeEmitter = {};
        m_failure.clear();
        m_environmentRequested = false;
        m_modelsPlaced = false;
        m_waterCreated = false;
        m_particleEpochStarted = false;
        m_particleEpochResetQueued = false;
        m_particleEpochApplied = false;
        m_smokeEpochResetQueued = false;
        m_smokeEpochApplied = false;
        m_directionalLightCreated = false;
        m_toneMappingObserved = false;
        m_temporalAAObserved = false;
        m_shadowObserved = false;
        m_translucencyObserved = false;
        m_particleObserved = false;
        m_smokeObserved = false;
        m_waterObserved = false;
        m_toneMappingCapabilityPublished = false;
        m_temporalAACapabilityPublished = false;
        m_shadowCapabilityPublished = false;
        m_translucencyCapabilityPublished = false;
        m_particleCapabilityPublished = false;
        m_waterCapabilityPublished = false;
        m_renderPath = context.options.renderPath;

        if (!ApplySampleRenderPathPolicy(
                m_renderPath, context.renderSettings.gpuCulling, outError))
        {
            return false;
        }
        if (context.options.deterministicSeed == 0u ||
            context.options.fixedTimeNumerator != 1u ||
            context.options.fixedTimeDenominator != 60u ||
            context.options.pipelineRecipe == SamplePipelineRecipe::Minimal ||
            context.options.pipelineRecipe == SamplePipelineRecipe::Headless)
        {
            outError =
                "render-quality-lab requires a deterministic 1/60 raster recipe";
            return false;
        }

        const std::vector<std::string> expectedModelIds = {
            RenderQualityLabPrimaryAssetId,
            RenderQualityLabTransparentAssetId,
            RenderQualityLabWaterReferenceAssetId,
            RenderQualityLabParticleGroundAssetId};
        if (context.options.modelAssetIds != expectedModelIds ||
            context.options.environmentAssetId !=
                RenderQualityLabEnvironmentAssetId ||
            context.options.effectAssetIds.size() != 2u ||
            context.options.effectAssetIds[0] != RenderQualityLabFireAssetId ||
            context.options.effectAssetIds[1] != RenderQualityLabSmokeAssetId ||
            context.options.effectAssetId != RenderQualityLabFireAssetId)
        {
            outError =
                "render-quality-lab requires its fixed cooked model, environment, fire, and smoke asset set";
            return false;
        }

        const float32 aspect = static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(44.0f), aspect, 0.05f, 160.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 4.0f, 12.5f)}) ||
            !context.cameras.LookAt(context.camera,
                                    Vec3(0.0f, 1.0f, -1.0f)))
        {
            outError = "Failed to configure the RenderQualityLab camera";
            return false;
        }

        SampleEnvironmentLoadOptions environmentOptions;
        environmentOptions.quality = context.options.quality;
        environmentOptions.smoke = context.options.smoke;
        environmentOptions.exposure = 1.0f;
        if (!context.environments.RequestByAssetId(
                context.options.environmentAssetId,
                environmentOptions,
                m_environment,
                outError))
        {
            return false;
        }
        m_environmentRequested = true;

        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation = MakeLookRotation(
            normalize(Vec3(-0.32f, -0.78f, -0.48f)),
            Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc, MakeDirectionalLight(), SceneECS::Visibility{})
                 .IsValid())
        {
            outError =
                "Failed to create the RenderQualityLab directional light";
            return false;
        }
        m_directionalLightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024u : 2048u;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1u : 3u;
        context.renderSettings.shadows.maxDistance = 70.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;
        context.renderSettings.shadows.filterRadiusTexels = 1.0f;

        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.enableVelocityOutput = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        postProcess.enableBloom = true;
        postProcess.enableColorGrading = true;
        postProcess.enableSSR = true;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = 2.5f;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.bloomThreshold = 0.75f;
        postProcess.bloomIntensity = 0.28f;
        postProcess.bloomRadius = 0.55f;
        postProcess.colorGradingContrast = 1.04f;
        postProcess.colorGradingSaturation = 1.03f;

        Water::WaterEcsConfig waterConfig;
        if (!MakeCanonicalWaterConfig(waterConfig))
        {
            outError = "Failed to create the canonical RenderQualityLab water configuration";
            return false;
        }
        SceneECS::RuntimeEntityDesc waterDesc;
        waterDesc.localTransform.translation = Vec3(0.0f, -0.15f, 1.8f);
        waterDesc.bounds = {
            .center = Vec3(0.0f),
            .extents = Vec3(4.0f, 1.5f, 2.2f)};
        m_waterSurface = context.sceneLifetime.CreateAndAdoptWithFragments(
            waterDesc,
            SceneECS::WaterRuntimeTag{},
            waterConfig,
            Water::WaterEcsIntent{},
            Water::WaterEcsState{});
        if (!m_waterSurface.IsValid())
        {
            outError = "Failed to create the RenderQualityLab water entity";
            return false;
        }
        m_waterCreated = true;

        m_models.reserve(context.options.modelAssetIds.size());
        for (const std::string& assetId : context.options.modelAssetIds)
        {
            LoadedSampleModel model;
            if (!context.models.RequestByAssetId(assetId, model, outError))
            {
                for (LoadedSampleModel& requested : m_models)
                {
                    static_cast<void>(context.models.Cancel(requested));
                }
                outError = "RenderQualityLab failed to queue model '" +
                           assetId + "': " + outError;
                return false;
            }
            m_models.push_back(std::move(model));
        }

        if (!context.particleEffects.RequestByAssetId(
                context.options.effectAssetId,
                m_particleEffect,
                outError))
        {
            for (LoadedSampleModel& requested : m_models)
            {
                static_cast<void>(context.models.Cancel(requested));
            }
            outError = "RenderQualityLab failed to queue its cooked fire effect: " +
                       outError;
            return false;
        }
        if (!context.particleEffects.RequestByAssetId(
                RenderQualityLabSmokeAssetId,
                m_smokeEffect,
                outError))
        {
            static_cast<void>(context.particleEffects.Cancel(m_particleEffect));
            for (LoadedSampleModel& requested : m_models)
            {
                static_cast<void>(context.models.Cancel(requested));
            }
            outError = "RenderQualityLab failed to queue its cooked smoke effect: " +
                       outError;
            return false;
        }
        return true;
    }

    bool RenderQualityLabSample::PlaceModels(SampleContext& context,
                                             std::string& outError)
    {
        std::vector<Vec3> slots;
        if (!BuildDeterministicModelSlots(m_models.size(), slots))
        {
            outError = "RenderQualityLab model slot plan is invalid";
            return false;
        }

        constexpr std::array<float32, 4> TargetExtents{
            9.0f, // interior backdrop
            3.0f, // transparent material gallery
            3.2f, // water reference and shadow receiver
            3.2f  // fire/smoke ground and caster
        };
        for (size_t index = 0; index < m_models.size(); ++index)
        {
            LoadedSampleModel& model = m_models[index];
            if (!model.IsCPUReady())
            {
                outError = "RenderQualityLab model is not CPU-ready";
                return false;
            }
            const SceneECS::SceneEntityRef root = model.GetRootEntityRef();
            if (!root.IsValid() ||
                root.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
                !context.scene.GetEntityRef(root.entity).IsValid())
            {
                outError = "RenderQualityLab model root handle is stale";
                return false;
            }

            const SceneECS::LocalTransform* const localTransform =
                context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(
                    root.entity);
            AABB bounds;
            if (localTransform == nullptr ||
                !TryComputeSampleModelRenderableWorldBounds(
                    model, context.scene, bounds))
            {
                outError = "RenderQualityLab model has invalid bounds";
                return false;
            }
            const Vec3 size = bounds.GetSize();
            const float32 maximumDimension =
                std::max({size.x, size.y, size.z});
            if (!std::isfinite(maximumDimension) || maximumDimension <= 0.00001f)
            {
                outError = "RenderQualityLab model has degenerate bounds";
                return false;
            }

            const float32 scale = TargetExtents[index] / maximumDimension;
            SceneECS::LocalTransform placed = *localTransform;
            placed.scale = Vec3(scale);
            placed.translation = Vec3(
                slots[index].x - bounds.GetCenter().x * scale,
                -bounds.GetMin().y * scale,
                slots[index].z - bounds.GetCenter().z * scale);
            if (!context.scene.SetLocalTransform(root.entity, placed))
            {
                outError = "RenderQualityLab could not author a model transform";
                return false;
            }
        }
        m_modelsPlaced = true;
        return true;
    }

    bool RenderQualityLabSample::CreateParticleEmitter(
        SampleContext& context,
        const LoadedSampleParticleEffect& effect,
        const Vec3& position,
        const uint32 maxParticleCount,
        const uint64 deterministicInstanceKey,
        SceneECS::SceneEntityRef& outEmitter,
        std::string& outError)
    {
        if (!effect.IsReady() || !effect.GetResourceId())
        {
            outError = "RenderQualityLab particle effect is not resource-resident";
            return false;
        }
        SceneECS::RuntimeEntityDesc emitterDesc;
        emitterDesc.localTransform.translation = position;
        const Particle::ParticleEcsConfig config = MakeParticleConfig(
            effect,
            context.options.deterministicSeed,
            deterministicInstanceKey,
            maxParticleCount,
            false,
            1u);
        const Particle::ParticleEcsIntent intent{
            .command = Particle::ParticleEcsCommand::None,
            .sequence = 0,
        };
        outEmitter = context.sceneLifetime.CreateAndAdoptWithFragments(
            emitterDesc,
            SceneECS::ParticleRuntimeTag{},
            config,
            intent,
            Particle::ParticleEcsState{});
        if (!outEmitter.IsValid())
        {
            outError = "Failed to create the RenderQualityLab particle emitter";
            return false;
        }
        return true;
    }

    bool RenderQualityLabSample::QueueParticleEpochReset(
        SampleContext& context,
        const LoadedSampleParticleEffect& effect,
        const SceneECS::SceneEntityRef emitter,
        const uint32 maxParticleCount,
        const uint64 configurationRevision,
        const uint64 restartSequence,
        const uint64 deterministicInstanceKey,
        std::string& outError)
    {
        if (!emitter.IsValid() || !effect.IsReady())
        {
            outError =
                "RenderQualityLab deterministic epoch began before its cooked particle emitter was ready";
            return false;
        }
        SceneECS::SceneCommandBuffer commands = context.scene.CreateCommandBuffer();
        const Particle::ParticleEcsConfig config = MakeParticleConfig(
            effect,
            context.options.deterministicSeed,
            deterministicInstanceKey,
            maxParticleCount,
            true,
            configurationRevision);
        const Particle::ParticleEcsIntent restart{
            .command = Particle::ParticleEcsCommand::Restart,
            .sequence = restartSequence,
        };
        const SceneECS::SceneCommandReceipt configReceipt =
            commands.Set(emitter.entity, config);
        const SceneECS::SceneCommandReceipt restartReceipt =
            commands.Set(emitter.entity, restart);
        const SceneECS::SceneCommandBufferReceipt submission =
            context.scene.SubmitCommandBuffer(
                std::move(commands), SceneECS::SceneCommandBarrier::BeginSimulation);
        if (configReceipt.IsRejected() || restartReceipt.IsRejected() ||
            submission.IsRejected() || submission.IsDiscarded())
        {
            outError =
                "RenderQualityLab deterministic epoch could not queue the particle reset";
            return false;
        }
        return true;
    }

    bool RenderQualityLabSample::HasAppliedParticleEpoch(
        const SampleContext& context,
        const LoadedSampleParticleEffect& effect,
        const SceneECS::SceneEntityRef emitter,
        const uint32 maxParticleCount,
        const uint64 configurationRevision,
        const uint64 restartSequence,
        const uint64 deterministicInstanceKey) const noexcept
    {
        if (!emitter.IsValid() || !effect.IsReady() || !effect.GetResourceId())
        {
            return false;
        }
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
               config->maxParticleCount == maxParticleCount &&
               config->configurationRevision == configurationRevision &&
               config->deterministicSeed == context.options.deterministicSeed &&
               config->deterministicInstanceKey == deterministicInstanceKey &&
               intent->command == Particle::ParticleEcsCommand::Restart &&
               intent->sequence == restartSequence &&
               state->lastAppliedConfigurationRevision ==
                   configurationRevision &&
               state->lastConsumedCommandSequence ==
                   restartSequence;
    }

    bool RenderQualityLabSample::BeginDeterministicEpoch(
        SampleContext& context,
        std::string& outError)
    {
        if (context.options.fixedTimeNumerator != 1u ||
            context.options.fixedTimeDenominator != 60u ||
            context.options.deterministicSeed == 0u)
        {
            outError = "render-quality-lab requires the manifest's fixed 1/60 epoch";
            return false;
        }
        if (!QueueParticleEpochReset(
                context,
                m_particleEffect,
                m_particleEmitter,
                256u,
                RenderQualityLabParticleConfigurationRevision,
                RenderQualityLabParticleRestartSequence,
                RenderQualityLabFireDeterministicInstanceKey,
                outError))
        {
            return false;
        }
        m_particleEpochResetQueued = true;
        if (!QueueParticleEpochReset(
                context,
                m_smokeEffect,
                m_smokeEmitter,
                192u,
                RenderQualityLabSmokeConfigurationRevision,
                RenderQualityLabSmokeRestartSequence,
                RenderQualityLabSmokeDeterministicInstanceKey,
                outError))
        {
            return false;
        }
        m_smokeEpochResetQueued = true;
        m_particleEpochStarted = true;
        return true;
    }

    void RenderQualityLabSample::Update(SampleContext& context,
                                        float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (!m_failure.empty())
        {
            return;
        }

        bool allModelsCPUReady = !m_models.empty();
        for (LoadedSampleModel& model : m_models)
        {
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
                context.models.UpdateReadiness(model);
            if (status.IsTerminal() && !model.IsCPUReady())
            {
                m_failure = DescribeModelFailure(status);
                return;
            }
            allModelsCPUReady &= model.IsCPUReady();
        }

        const ResourceSceneAdapters::EcsEnvironmentLoadStatus environmentStatus =
            context.environments.UpdateReadiness(m_environment);
        if (environmentStatus.IsTerminal() && !m_environment.IsValid())
        {
            m_failure = DescribeEnvironmentFailure(environmentStatus);
            return;
        }

        context.particleEffects.UpdateReadiness(m_particleEffect);
        if (m_particleEffect.state == SampleParticleEffectLoadState::Failed ||
            m_particleEffect.state == SampleParticleEffectLoadState::Cancelled)
        {
            m_failure = m_particleEffect.diagnostic.empty()
                            ? "RenderQualityLab fire effect failed to load"
                            : m_particleEffect.diagnostic;
            return;
        }
        context.particleEffects.UpdateReadiness(m_smokeEffect);
        if (m_smokeEffect.state == SampleParticleEffectLoadState::Failed ||
            m_smokeEffect.state == SampleParticleEffectLoadState::Cancelled)
        {
            m_failure = m_smokeEffect.diagnostic.empty()
                            ? "RenderQualityLab smoke effect failed to load"
                            : m_smokeEffect.diagnostic;
            return;
        }

        if (!m_modelsPlaced && allModelsCPUReady)
        {
            std::string error;
            if (!PlaceModels(context, error))
            {
                m_failure = error.empty() ? "RenderQualityLab model placement failed"
                                          : std::move(error);
                return;
            }
        }
        if (m_particleEffect.IsReady() && !m_particleEmitter.IsValid())
        {
            std::string error;
            if (!CreateParticleEmitter(
                    context,
                    m_particleEffect,
                    Vec3(-2.7f, 1.2f, -2.3f),
                    256u,
                    RenderQualityLabFireDeterministicInstanceKey,
                    m_particleEmitter,
                    error))
            {
                m_failure = error.empty() ? "RenderQualityLab particle setup failed"
                                          : std::move(error);
                return;
            }
        }
        if (m_smokeEffect.IsReady() && !m_smokeEmitter.IsValid())
        {
            std::string error;
            if (!CreateParticleEmitter(
                    context,
                    m_smokeEffect,
                    Vec3(2.7f, 1.1f, -2.3f),
                    192u,
                    RenderQualityLabSmokeDeterministicInstanceKey,
                    m_smokeEmitter,
                    error))
            {
                m_failure = error.empty() ? "RenderQualityLab smoke setup failed"
                                           : std::move(error);
                return;
            }
        }
        if (m_particleEpochStarted && m_particleEpochResetQueued &&
            !m_particleEpochApplied)
        {
            m_particleEpochApplied = HasAppliedParticleEpoch(
                context,
                m_particleEffect,
                m_particleEmitter,
                256u,
                RenderQualityLabParticleConfigurationRevision,
                RenderQualityLabParticleRestartSequence,
                RenderQualityLabFireDeterministicInstanceKey);
        }
        if (m_particleEpochStarted && m_smokeEpochResetQueued &&
            !m_smokeEpochApplied)
        {
            m_smokeEpochApplied = HasAppliedParticleEpoch(
                context,
                m_smokeEffect,
                m_smokeEmitter,
                192u,
                RenderQualityLabSmokeConfigurationRevision,
                RenderQualityLabSmokeRestartSequence,
                RenderQualityLabSmokeDeterministicInstanceKey);
        }
    }

    void RenderQualityLabSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void RenderQualityLabSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        const bool stable = m_modelsPlaced && m_waterCreated &&
                            m_particleEpochApplied && m_smokeEpochApplied &&
                            m_toneMappingObserved &&
                            m_temporalAAObserved && m_shadowObserved &&
                            m_translucencyObserved && m_particleObserved &&
                            m_smokeObserved && m_waterObserved;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable"
                                         : m_failure.empty() ? "pending"
                                                             : "failed");
        reporter.Enable("RenderQualityLabComposition");
        reporter.Enable("CookedOnlyRenderQualityLabAssets");
        reporter.Enable("SevenAssetQualificationSet");
        reporter.Enable("Fixed60HzQualificationEpoch");
        // These are scenario declarations, not execution evidence.  The host
        // qualification producer publishes the corresponding completion-owned
        // artifacts separately and must not infer success from these flags.
        reporter.Enable("Warmup60FramesAnd300SamplePolicyDeclared");
        reporter.Enable("SevenSemanticAttachmentPolicyDeclared");
        reporter.Enable("ForwardPlusDeferredComparisonPolicyDeclared");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.ResourceDiagnostic(
            "RenderQualityLab composes the cooked interior, transparent material, water reference, particle ground, fire, smoke, and PBR environment identities; source import and runtime cooking are forbidden.");
        if (m_environmentRequested && m_environment.IsValid())
        {
            reporter.Enable("CookedPbrEnvironmentResident");
        }
        if (m_modelsPlaced)
        {
            reporter.Enable("DeterministicModelSlotsPlaced");
        }
        if (m_waterCreated)
        {
            reporter.Enable("GerstnerWaterEntityCreated");
        }
        if (m_particleEmitter.IsValid())
        {
            reporter.Enable("CookedFireParticleEmitterCreated");
        }
        if (m_smokeEffect.IsReady())
        {
            reporter.Enable("CookedSmokeEffectResident");
        }
        if (m_smokeEmitter.IsValid())
        {
            reporter.Enable("CookedSmokeParticleEmitterCreated");
        }
        if (m_particleEpochResetQueued)
        {
            reporter.Enable("ParticleEpochResetQueued");
        }
        if (m_particleEpochApplied)
        {
            reporter.Enable("ParticleEpochResetApplied");
        }
        if (m_smokeEpochResetQueued)
        {
            reporter.Enable("SmokeParticleEpochResetQueued");
        }
        if (m_smokeEpochApplied)
        {
            reporter.Enable("SmokeParticleEpochResetApplied");
        }
        if (m_toneMappingObserved)
        {
            reporter.Enable("ToneMappingExecutionObserved");
        }
        if (m_temporalAAObserved)
        {
            reporter.Enable("TemporalAAExecutionObserved");
        }
        if (m_shadowObserved)
        {
            reporter.Enable("DirectionalShadowExecutionObserved");
        }
        if (m_translucencyObserved)
        {
            reporter.Enable("SortedTranslucencyExecutionObserved");
        }
        if (m_particleObserved)
        {
            reporter.Enable("ParticleFireExecutionObserved");
        }
        if (m_smokeObserved)
        {
            reporter.Enable("ParticleSmokeExecutionObserved");
        }
        if (m_waterObserved)
        {
            reporter.Enable("WaterExecutionObserved");
        }
    }

    void RenderQualityLabSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (!IsSampleRenderQualityFrameAvailable(diagnostics))
        {
            return;
        }

        const bool completeSameFrameComposition =
            HasSampleRenderQualityCompositionExecution(diagnostics);
        if (completeSameFrameComposition)
        {
            m_toneMappingObserved = true;
            m_temporalAAObserved = true;
            m_shadowObserved = true;
            m_translucencyObserved = true;
            m_particleObserved = true;
            m_smokeObserved = true;
            m_waterObserved = true;
        }

        if (m_toneMappingObserved)
        {
            PublishCapability(
                assessment,
                ToneMappingCapability,
                "The presented RenderQualityLab frame completed the renderer-owned tone-mapping contract.",
                m_toneMappingCapabilityPublished);
        }
        if (m_temporalAAObserved)
        {
            PublishCapability(
                assessment,
                TemporalAACapability,
                "The presented RenderQualityLab frame completed temporal resolve and committed its history.",
                m_temporalAACapabilityPublished);
        }
        if (m_shadowObserved)
        {
            PublishCapability(
                assessment,
                DirectionalShadowCapability,
                "The presented RenderQualityLab frame resolved and sampled all requested directional shadow cascades.",
                m_shadowCapabilityPublished);
        }
        if (m_translucencyObserved)
        {
            PublishCapability(
                assessment,
                TranslucencyCapability,
                "The presented RenderQualityLab frame executed ordered cooked translucent material packets without fallback.",
                m_translucencyCapabilityPublished);
        }
        if (m_particleObserved && m_smokeObserved)
        {
            PublishCapability(
                assessment,
                ParticleCapability,
                "The presented RenderQualityLab captures executed both cooked deterministic CPU fire and smoke packets.",
                m_particleCapabilityPublished);
        }
        if (m_waterObserved)
        {
            PublishCapability(
                assessment,
                WaterCapability,
                "The presented RenderQualityLab frame executed the canonical Gerstner water surface.",
                m_waterCapabilityPublished);
        }
    }

    bool RenderQualityLabSample::HasRequiredPresentation(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return HasSampleRenderQualityPresentedScene(
                   diagnostics,
                   static_cast<uint32>(m_models.size())) &&
               IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics);
    }

    SampleReadiness RenderQualityLabSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
        {
            return SampleReadiness::Failed(m_failure);
        }
        if (!m_environmentRequested || !m_environment.IsValid())
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for its cooked PBR environment");
        }
        if (!m_modelsPlaced)
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for deterministic model placement");
        }
        for (const LoadedSampleModel& model : m_models)
        {
            if (!model.IsFullyResident())
            {
                return SampleReadiness::Pending(
                    "render-quality-lab is waiting for all cooked models to become resident");
            }
        }
        if (!m_waterCreated)
        {
            return SampleReadiness::Failed(
                "render-quality-lab did not create its public water entity");
        }
        if (!m_particleEffect.IsReady() || !m_particleEmitter.IsValid() ||
            !m_smokeEffect.IsReady() || !m_smokeEmitter.IsValid())
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for its cooked fire/smoke effects and emitters");
        }
        if (!m_particleEpochStarted)
        {
            return SampleReadiness::Ready();
        }
        if (!m_particleEpochResetQueued || !m_particleEpochApplied ||
            !m_smokeEpochResetQueued || !m_smokeEpochApplied)
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for deterministic fire/smoke particle epoch resets");
        }
        if (!HasRequiredPresentation(diagnostics))
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for a presented composed render frame");
        }
        if (!m_toneMappingObserved || !m_temporalAAObserved ||
            !m_shadowObserved || !m_translucencyObserved ||
            !m_particleObserved || !m_smokeObserved || !m_waterObserved)
        {
            return SampleReadiness::Pending(
                "render-quality-lab is waiting for all required composed feature receipts");
        }
        return SampleReadiness::Ready();
    }

    bool RenderQualityLabSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    bool RenderQualityLabSample::BuildDeterministicModelSlots(
        const std::size_t modelCount,
        std::vector<Vec3>& outSlots) noexcept
    {
        constexpr std::size_t MaximumSlots = 4u;
        if (modelCount == 0u || modelCount > MaximumSlots)
        {
            outSlots.clear();
            return false;
        }

        // Roles remain fixed by the sample's cooked-asset contract. The
        // interior owns the background volume, the material gallery stays
        // central, and the two receiver fixtures align with the deterministic
        // fire/smoke emitters instead of forming four unrelated equal boxes.
        const std::array<Vec3, MaximumSlots> roleSlots{
            Vec3(0.0f, 0.0f, -5.0f),
            Vec3(0.0f, 0.0f, -1.0f),
            Vec3(2.7f, 0.0f, -2.3f),
            Vec3(-2.7f, 0.0f, -2.3f)};
        outSlots.assign(roleSlots.begin(), roleSlots.begin() + modelCount);
        return true;
    }

    bool RenderQualityLabSample::GetDeterministicParticleInstanceKeys(
        uint64& outFireKey,
        uint64& outSmokeKey) noexcept
    {
        outFireKey = RenderQualityLabFireDeterministicInstanceKey;
        outSmokeKey = RenderQualityLabSmokeDeterministicInstanceKey;
        return outFireKey != 0 && outSmokeKey != 0 && outFireKey != outSmokeKey;
    }

    void RenderQualityLabSample::Shutdown(SampleContext& context)
    {
        for (LoadedSampleModel& model : m_models)
        {
            static_cast<void>(context.models.Cancel(model));
        }
        static_cast<void>(context.environments.Cancel(m_environment));
        static_cast<void>(context.particleEffects.Cancel(m_particleEffect));
        static_cast<void>(context.particleEffects.Cancel(m_smokeEffect));
        m_models.clear();
        m_environment = {};
        m_particleEffect = {};
        m_smokeEffect = {};
        m_waterSurface = {};
        m_particleEmitter = {};
        m_smokeEmitter = {};
        m_environmentRequested = false;
        m_modelsPlaced = false;
        m_waterCreated = false;
        m_particleEpochStarted = false;
        m_particleEpochResetQueued = false;
        m_particleEpochApplied = false;
        m_smokeEpochResetQueued = false;
        m_smokeEpochApplied = false;
        m_directionalLightCreated = false;
        m_toneMappingObserved = false;
        m_temporalAAObserved = false;
        m_shadowObserved = false;
        m_translucencyObserved = false;
        m_particleObserved = false;
        m_smokeObserved = false;
        m_waterObserved = false;
        m_toneMappingCapabilityPublished = false;
        m_temporalAACapabilityPublished = false;
        m_shadowCapabilityPublished = false;
        m_translucencyCapabilityPublished = false;
        m_particleCapabilityPublished = false;
        m_waterCapabilityPublished = false;
        m_renderPath = SampleRenderPath::Auto;
        m_failure.clear();
    }
} // namespace RVX
