/** @file PhysicalExposureSample.cpp @brief Deterministic Manual EV100 scenario. */

#include "Scenes/PhysicalExposureSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderExposure.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr float32 ManualExposureEV100 = 10.0f;

        const SampleInfo PhysicalExposureInfo{
            "physical-exposure",
            "Physical Exposure",
            "Demonstrates renderer-owned fixed Manual EV100 and pre-exposure",
            "pbr-material-grid",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentCapability ManualExposureCapability{
            AssessmentCode("RENDER.CAPABILITY.PHYSICAL_MANUAL_EXPOSURE"),
            "Manual EV100 is requested through the public post-process contract.",
            true,
            "Qualification requires a presented renderer exposure receipt, not a requested setting."};

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

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.96f, 0.88f),
                .intensity = 12000.0f,
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
            return "Physical-exposure model request reached a terminal state.";
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
                   << ", mode=" << static_cast<uint32>(exposure.exposureMode)
                   << ", stateHash=" << exposure.exposureStateContentHash
                   << ", preExposure=" << exposure.preExposure;
            return stream.str();
        }

        [[nodiscard]] bool HasManualExposurePipelineReadiness(
            const SamplePostProcessExecutionDiagnostics& exposure) noexcept
        {
            // A qualification probe is intentionally not a readiness
            // prerequisite: the host arms it for the final capture only after
            // this public render pipeline has become healthy.
            return exposure.HasQualifiedPresentation() &&
                   exposure.HasSharedExposureState() &&
                   exposure.exposureMode == RenderExposureMode::ManualEV100 &&
                   exposure.exposureRequested && exposure.exposurePipelineReady &&
                   exposure.exposureInputsReady && exposure.exposureExecuted &&
                   exposure.exposureHistogramPassCount == 0 &&
                   exposure.exposureAdaptationPassCount == 1 &&
                   exposure.taaRequested && exposure.taaPipelineReady &&
                   exposure.taaInputsReady && exposure.taaExecuted &&
                   exposure.bloomPassCount == 1;
        }

        void PublishManualExposureCapability(SampleAssessmentChannel& assessment,
                                             bool& published)
        {
            if (published)
            {
                return;
            }

            AssessmentCapabilityObservation observation;
            observation.capability = ManualExposureCapability;
            observation.checkpoint = AssessmentCheckpoints::ScenarioStable;
            observation.value = DiagnosticValue<bool>::Available(true);
            observation.reason =
                "The presented post-process receipt proves a Manual EV100 exposure state and shared pre-exposure consumers.";
            if (assessment.MarkCapabilityObservation(std::move(observation)))
            {
                published = true;
            }
        }
    } // namespace

    const SampleInfo& PhysicalExposureSample::GetInfo() const noexcept
    {
        return PhysicalExposureInfo;
    }

    SampleAssessmentContract PhysicalExposureSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.PHYSICAL_EXPOSURE");
        contract.revision = "1";
        contract.capabilities = {ManualExposureCapability};
        return contract;
    }

    bool PhysicalExposureSample::Setup(SampleContext& context,
                                       std::string& outError)
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
            outError = "Failed to configure the physical-exposure ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the physical-exposure ECS skybox";
            return false;
        }
        m_skyboxCreated = true;

        const Vec3 lightDirection = normalize(Vec3(-0.25f, -0.55f, -0.65f));
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc, MakeDirectionalLight(), SceneECS::Visibility{}).IsValid())
        {
            outError =
                "Failed to create the physical-exposure ECS directional light";
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
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = ManualExposureEV100;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;

        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    void PhysicalExposureSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
        }
    }

    void PhysicalExposureSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void PhysicalExposureSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() &&
            m_manualExposureCapabilityPublished &&
            m_manualExposureExecutionQualified;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("EnginePostProcessConfiguration");
        reporter.Enable("ManualEV100Requested");
        reporter.Enable("ManualEV100Stable");
        reporter.Enable("BloomRequested");
        reporter.Enable("TemporalAARequested");
        reporter.Enable("VelocityOutputRequested");
        reporter.Enable("FXAAExplicitlyDisabled");
        reporter.ResourceDiagnostic(
            "physical exposure mode=manual-ev100, ev100=10.000000");
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        if (m_manualExposureExecutionQualified)
        {
            reporter.Enable("ManualEV100ActualExecutionQualified");
            reporter.Enable("SharedPreExposureActualExecutionQualified");
        }
    }

    void PhysicalExposureSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const SamplePostProcessExecutionDiagnostics& execution =
            diagnostics.postProcessActualExecution;
        m_manualExposureExecutionQualified =
            execution.IsManualEV100ExposureQualificationReady() &&
            std::abs(execution.targetEV100 - ManualExposureEV100) <= 1.0e-5f &&
            std::abs(execution.adaptedEV100 - ManualExposureEV100) <= 1.0e-5f &&
            execution.HasPhysicallyConsistentExposureProbe(0.0f);
        if (m_manualExposureExecutionQualified)
        {
            PublishManualExposureCapability(
                assessment, m_manualExposureCapabilityPublished);
        }
    }

    SampleReadiness PhysicalExposureSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Physical-exposure showcase is waiting for the model to become fully resident");
        }
        if (diagnostics.visibleObjectCount == 0 || !diagnostics.graphCompiled ||
            !HasManualExposurePipelineReadiness(
                diagnostics.postProcessActualExecution))
        {
            return SampleReadiness::Pending(
                "Waiting for the renderer-owned Manual EV100 pipeline: " +
                DescribeExposureState(diagnostics));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Physical-exposure showcase is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool PhysicalExposureSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        if (!readiness.IsReady())
        {
            outError = readiness.reason;
            return false;
        }
        if (!m_manualExposureExecutionQualified)
        {
            outError =
                "Physical-exposure showcase did not produce an exact completed GPU exposure probe";
            return false;
        }
        if (!m_manualExposureCapabilityPublished)
        {
            outError =
                "Physical-exposure showcase did not publish its qualified capability observation";
            return false;
        }
        outError.clear();
        return true;
    }

    void PhysicalExposureSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure =
                "Failed to cancel the physical-exposure ECS model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_manualExposureCapabilityPublished = false;
        m_manualExposureExecutionQualified = false;
    }
} // namespace RVX
