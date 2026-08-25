/** @file AutoExposureSample.cpp @brief Deterministic histogram auto-exposure scenario. */

#include "Scenes/AutoExposureSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr uint32 NeutralEndTick = 60;
        constexpr uint32 BrightEndTick = 120;
        constexpr float32 NeutralIlluminanceLux = 12000.0f;
        constexpr float32 BrightIlluminanceLux = 90000.0f;
        constexpr float32 DarkIlluminanceLux = 500.0f;

        const SampleInfo AutoExposureInfo{
            "auto-exposure",
            "Histogram Auto Exposure",
            "Demonstrates deterministic histogram exposure adaptation across neutral, bright, and dark lighting",
            "pbr-material-grid",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentCapability HistogramAutoExposureCapability{
            AssessmentCode("RENDER.CAPABILITY.HISTOGRAM_AUTO_EXPOSURE"),
            "Histogram auto exposure is requested by the deterministic luminance sequence.",
            true,
            "Qualification requires the renderer to present a committed histogram exposure candidate."};

        [[nodiscard]] Quat MakeLookRotation(const Vec3& direction,
                                             const Vec3& up)
        {
            const Vec3 forward = length(direction) < 0.001f
                ? Vec3(0.0f, -1.0f, 0.0f)
                : normalize(direction);
            const Vec3 right = length(cross(up, forward)) < 0.001f
                ? Vec3(1.0f, 0.0f, 0.0f)
                : normalize(cross(up, forward));
            const Vec3 correctedUp = cross(forward, right);

            Mat4 rotation(1.0f);
            rotation[0] = Vec4(right, 0.0f);
            rotation[1] = Vec4(correctedUp, 0.0f);
            rotation[2] = Vec4(-forward, 0.0f);
            return Mat4ToQuat(rotation);
        }

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(-0.25f, -0.55f, -0.65f)),
                .sunColor = Vec3(1.0f, 0.96f, 0.88f),
                .zenithColor = Vec3(0.08f, 0.19f, 0.44f),
                .horizonColor = Vec3(0.58f, 0.68f, 0.82f),
                .groundColor = Vec3(0.06f, 0.07f, 0.08f),
                .scatteringIntensity = 0.60f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight(float32 illuminanceLux)
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.96f, 0.88f),
                .intensity = illuminanceLux,
                .intensityUnit = SceneECS::LightIntensityUnit::Lux,
                .shadowBias = 0.0008f,
                .castsShadows = true,
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
            return "Auto-exposure model request reached a terminal state.";
        }

        [[nodiscard]] std::string DescribeExposureState(
            const SampleRenderDiagnostics& diagnostics)
        {
            const auto& exposure = diagnostics.postProcessActualExecution;
            std::ostringstream stream;
            stream << "graphCompiled=" << diagnostics.graphCompiled
                   << ", visibleObjects=" << diagnostics.visibleObjectCount
                   << ", exposureRequested=" << exposure.exposureRequested
                   << ", pipelineReady=" << exposure.exposurePipelineReady
                   << ", inputsReady=" << exposure.exposureInputsReady
                   << ", executed=" << exposure.exposureExecuted
                   << ", histogramPasses=" << exposure.exposureHistogramPassCount
                   << ", adaptationPasses="
                   << exposure.exposureAdaptationPassCount
                   << ", historySampled="
                   << exposure.exposureHistorySampled
                   << ", historyCommitted="
                   << exposure.exposureHistoryCommitted
                   << ", probeReady=" << exposure.exposureProbeReady;
            return stream.str();
        }

        [[nodiscard]] bool HasHistogramAutoExposurePipelineReadiness(
            const SamplePostProcessExecutionDiagnostics& exposure) noexcept
        {
            // The host arms its explicit GPU readback after this renderer-owned
            // path is ready. Scenario readiness must not manufacture numerical
            // evidence by waiting for a probe that has not been requested yet.
            return exposure.HasQualifiedPresentation() &&
                   exposure.HasSharedExposureState() &&
                   exposure.exposureMode == RenderExposureMode::HistogramAuto &&
                   exposure.exposureRequested && exposure.exposurePipelineReady &&
                   exposure.exposureInputsReady && exposure.exposureExecuted &&
                   exposure.exposureHistogramPassCount == 2 &&
                   exposure.exposureAdaptationPassCount == 1 &&
                   exposure.taaRequested && exposure.taaPipelineReady &&
                   exposure.taaInputsReady && exposure.taaExecuted &&
                   exposure.bloomPassCount == 1;
        }

        void PublishAutoExposureCapability(SampleAssessmentChannel& assessment,
                                           bool& published)
        {
            if (published)
            {
                return;
            }

            AssessmentCapabilityObservation observation;
            observation.capability = HistogramAutoExposureCapability;
            observation.checkpoint = AssessmentCheckpoints::ScenarioStable;
            observation.value = DiagnosticValue<bool>::Available(true);
            observation.reason =
                "The presented post-process receipt proves histogram adaptation and an exact post-Present exposure-history commit.";
            if (assessment.MarkCapabilityObservation(std::move(observation)))
            {
                published = true;
            }
        }
    } // namespace

    const SampleInfo& AutoExposureSample::GetInfo() const noexcept
    {
        return AutoExposureInfo;
    }

    SampleAssessmentContract AutoExposureSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.AUTO_EXPOSURE");
        contract.revision = "1";
        contract.capabilities = {HistogramAutoExposureCapability};
        return contract;
    }

    bool AutoExposureSample::Setup(SampleContext& context, std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        if (!ApplySampleRenderPathPolicy(
                m_renderPath, context.renderSettings.gpuCulling, outError))
        {
            return false;
        }

        const float32 aspect = static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1U));
        if (!context.cameras.SetPerspective(
                context.camera, radians(45.0f), aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 1.35f, 4.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.35f, 0.0f)))
        {
            outError = "Failed to configure the auto-exposure ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the auto-exposure ECS skybox";
            return false;
        }
        m_skyboxCreated = true;

        const Vec3 lightDirection = normalize(Vec3(-0.25f, -0.55f, -0.65f));
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f));
        m_directionalLight = context.sceneLifetime.CreateAndAdoptWithFragments(
            lightDesc, MakeDirectionalLight(NeutralIlluminanceLux),
            SceneECS::Visibility{});
        if (!m_directionalLight.IsValid())
        {
            outError = "Failed to create the auto-exposure ECS directional light";
            return false;
        }
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024U : 2048U;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1U : 3U;
        context.renderSettings.shadows.maxDistance = 50.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;

        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.enableVelocityOutput = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        postProcess.enableBloom = true;
        postProcess.enableColorGrading = false;
        postProcess.enableSSAO = false;
        postProcess.enableSSR = false;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.gamma = 2.2f;
        postProcess.bloomThreshold = 1.0f;
        postProcess.bloomIntensity = 0.30f;
        postProcess.bloomRadius = 0.60f;
        postProcess.exposureSettings.mode = RenderExposureMode::HistogramAuto;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.exposureSettings.histogramLowPercentile = 0.50f;
        postProcess.exposureSettings.histogramHighPercentile = 0.95f;
        postProcess.exposureSettings.histogramKeyValue = 0.18f;
        postProcess.exposureSettings.minEV100 = -8.0f;
        postProcess.exposureSettings.maxEV100 = 20.0f;
        postProcess.exposureSettings.brighteningAdaptationSpeed = 4.0f;
        postProcess.exposureSettings.darkeningAdaptationSpeed = 1.0f;

        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    bool AutoExposureSample::BeginDeterministicEpoch(
        SampleContext& context,
        std::string& outError)
    {
        m_fixedStepCount = 0;
        m_brightStageObserved = false;
        m_darkStageObserved = false;
        m_autoExposureCapabilityPublished = false;
        m_autoExposureExecutionQualified = false;
        if (!ApplyLuminanceStage(context, LuminanceStage::Neutral))
        {
            outError =
                "Auto-exposure sample could not reset its deterministic luminance timeline";
            return false;
        }
        return true;
    }

    void AutoExposureSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
            return;
        }
        ++m_fixedStepCount;
        const LuminanceStage requestedStage = m_fixedStepCount <= NeutralEndTick
            ? LuminanceStage::Neutral
            : m_fixedStepCount <= BrightEndTick ? LuminanceStage::Bright
                                                 : LuminanceStage::Dark;
        if (requestedStage != m_luminanceStage &&
            !ApplyLuminanceStage(context, requestedStage))
        {
            m_modelFailure =
                "Auto-exposure sample could not publish its deterministic ECS light stage.";
            return;
        }

        if (m_luminanceStage == LuminanceStage::Bright)
        {
            m_brightStageObserved = true;
        }
        else if (m_luminanceStage == LuminanceStage::Dark)
        {
            m_darkStageObserved = true;
        }
    }

    void AutoExposureSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void AutoExposureSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() && m_brightStageObserved &&
            m_darkStageObserved && m_autoExposureCapabilityPublished &&
            m_autoExposureExecutionQualified;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("EnginePostProcessConfiguration");
        reporter.Enable("HistogramAutoExposureRequested");
        reporter.Enable("BloomRequested");
        reporter.Enable("TemporalAARequested");
        reporter.Enable("VelocityOutputRequested");
        reporter.Enable("FXAAExplicitlyDisabled");
        reporter.Enable("DeterministicLuminanceTimeline");
        reporter.ResourceDiagnostic(
            "auto exposure timeline=1/60 neutral[1,60],bright[61,120],dark[121,+)");
        if (m_brightStageObserved)
        {
            reporter.Enable("BrightLuminanceStageObserved");
        }
        if (m_darkStageObserved)
        {
            reporter.Enable("DarkLuminanceStageObserved");
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        if (m_autoExposureExecutionQualified)
        {
            reporter.Enable("HistogramAutoExposureActualExecutionQualified");
            reporter.Enable("ExposureHistoryCommittedAfterPresent");
            reporter.Enable("SharedPreExposureActualExecutionQualified");
        }
    }

    void AutoExposureSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const SamplePostProcessExecutionDiagnostics& exposure =
            diagnostics.postProcessActualExecution;
        // Tick 180 is sixty fixed steps into the dark stage. A valid
        // adaptation must still be approaching the lower dark-scene target
        // from the higher EV reached during the preceding bright stage.
        m_autoExposureExecutionQualified =
            exposure.IsHistogramAutoExposureQualificationReady() &&
            exposure.HasPhysicallyConsistentExposureProbe(0.0f) &&
            exposure.targetEV100 >= -8.0f &&
            exposure.targetEV100 <= 20.0f &&
            exposure.adaptedEV100 >= -8.0f &&
            exposure.adaptedEV100 <= 20.0f &&
            exposure.targetEV100 + 1.0e-4f < exposure.adaptedEV100;
        if (m_autoExposureExecutionQualified)
        {
            PublishAutoExposureCapability(
                assessment, m_autoExposureCapabilityPublished);
        }
    }

    SampleReadiness AutoExposureSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Auto-exposure showcase is waiting for the model to become fully resident");
        }
        if (!m_brightStageObserved || !m_darkStageObserved)
        {
            return SampleReadiness::Pending(
                "Auto-exposure showcase is waiting for the fixed neutral-to-bright-to-dark timeline");
        }
        if (diagnostics.visibleObjectCount == 0 || !diagnostics.graphCompiled ||
            !diagnostics.postProcessActualExecution
                 .available ||
            !HasHistogramAutoExposurePipelineReadiness(
                diagnostics.postProcessActualExecution))
        {
            return SampleReadiness::Pending(
                "Waiting for the renderer-owned histogram exposure pipeline: " +
                DescribeExposureState(diagnostics));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Auto-exposure showcase is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool AutoExposureSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        if (!readiness.IsReady())
        {
            outError = readiness.reason;
            return false;
        }
        if (!m_autoExposureExecutionQualified)
        {
            outError =
                "Auto-exposure showcase did not produce a completed dark-adaptation GPU probe";
            return false;
        }
        if (!m_autoExposureCapabilityPublished)
        {
            outError =
                "Auto-exposure showcase did not publish its qualified capability observation";
            return false;
        }
        outError.clear();
        return true;
    }

    void AutoExposureSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure = "Failed to cancel the auto-exposure ECS model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_directionalLight = {};
        m_fixedStepCount = 0;
        m_luminanceStage = LuminanceStage::Neutral;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_brightStageObserved = false;
        m_darkStageObserved = false;
        m_autoExposureCapabilityPublished = false;
        m_autoExposureExecutionQualified = false;
    }

    bool AutoExposureSample::ApplyLuminanceStage(SampleContext& context,
                                                  LuminanceStage stage)
    {
        if (!m_directionalLight.IsValid())
        {
            return false;
        }

        const float32 illuminance = stage == LuminanceStage::Neutral
            ? NeutralIlluminanceLux
            : stage == LuminanceStage::Bright ? BrightIlluminanceLux
                                              : DarkIlluminanceLux;
        if (!context.scene.SetFragment(
                m_directionalLight.entity, MakeDirectionalLight(illuminance)))
        {
            return false;
        }

        m_luminanceStage = stage;
        return true;
    }
} // namespace RVX
