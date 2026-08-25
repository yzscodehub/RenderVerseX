/** @file TemporalAASample.cpp @brief Deterministic temporal anti-aliasing showcase. */

#include "Scenes/TemporalAASample.h"

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
        const SampleInfo TemporalAAInfo{
            "temporal-aa",
            "Temporal Anti-Aliasing",
            "Demonstrates jittered rasterization, motion reprojection, history rejection, and present-safe TAA history",
            "pbr-material-grid",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentCapability TemporalAACapability{
            AssessmentCode("RENDER.CAPABILITY.TEMPORAL_AA"),
            "TAA is requested by a deterministic moving-model sample.",
            true,
            "Temporal qualification requires a sampled prior history and an exact present-time history commit."};

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
                .intensity = 5.0f,
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
            return "Temporal-AA sample model request reached a terminal state.";
        }

        [[nodiscard]] std::string DescribeTemporalState(
            const SampleRenderDiagnostics& diagnostics)
        {
            const auto& temporal = diagnostics.postProcessActualExecution;
            std::ostringstream stream;
            stream << "graphCompiled=" << diagnostics.graphCompiled
                   << ", visibleObjects=" << diagnostics.visibleObjectCount
                   << ", receiptSchema="
                   << temporal.executionReceiptSchemaVersion
                   << ", requested=" << temporal.taaRequested
                   << ", pipelineReady=" << temporal.taaPipelineReady
                   << ", inputsReady=" << temporal.taaInputsReady
                   << ", executed=" << temporal.taaExecuted
                   << ", historySampled=" << temporal.historySampled
                   << ", historyCommitted=" << temporal.historyCommitted
                   << ", fallback=" << temporal.fallbackUsed;
            return stream.str();
        }

        void PublishTemporalCapability(
            SampleAssessmentChannel& assessment,
            bool& published)
        {
            if (published)
            {
                return;
            }

            AssessmentCapabilityObservation observation;
            observation.capability = TemporalAACapability;
            observation.checkpoint = AssessmentCheckpoints::ScenarioStable;
            observation.value = DiagnosticValue<bool>::Available(true);
            observation.reason =
                "The presented main-view receipt sampled prior TAA history and committed the matching candidate after Present.";
            if (assessment.MarkCapabilityObservation(std::move(observation)))
            {
                published = true;
            }
        }
    } // namespace

    const SampleInfo& TemporalAASample::GetInfo() const noexcept
    {
        return TemporalAAInfo;
    }

    SampleAssessmentContract TemporalAASample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.TEMPORAL_AA");
        contract.revision = "1";
        contract.capabilities = {TemporalAACapability};
        return contract;
    }

    bool TemporalAASample::Setup(SampleContext& context,
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
            outError = "Failed to configure the temporal-AA ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the temporal-AA ECS skybox";
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
            outError = "Failed to create the temporal-AA ECS light";
            return false;
        }
        m_lightCreated = true;

        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.enableVelocityOutput = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        postProcess.enableBloom = false;
        postProcess.enableColorGrading = false;
        postProcess.enableSSAO = false;
        postProcess.enableSSR = false;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = 0.0f;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.gamma = 2.2f;

        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    void TemporalAASample::Update(SampleContext& context, float deltaTime)
    {
        if (std::isfinite(deltaTime) && deltaTime > 0.0f)
        {
            m_simulationSeconds += deltaTime;
        }

        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
            return;
        }
        if (!m_model.IsFullyResident())
        {
            return;
        }

        const SceneECS::SceneEntityRef root = m_model.GetRootEntityRef();
        const SceneECS::LocalTransform* const authored =
            context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(root.entity);
        if (!root.IsValid() || authored == nullptr)
        {
            m_modelFailure =
                "Temporal-AA model root is missing its ECS local transform.";
            return;
        }
        if (!m_baseTransformCaptured)
        {
            m_baseTranslation = authored->translation;
            m_baseTransformCaptured = true;
        }

        SceneECS::LocalTransform animated = *authored;
        animated.translation = m_baseTranslation +
            Vec3(std::sin(m_simulationSeconds * 2.0f) * 0.22f,
                 0.0f,
                 0.0f);
        if (!context.scene.SetLocalTransform(root.entity, animated))
        {
            m_modelFailure =
                "Temporal-AA sample could not publish deterministic model motion.";
        }
    }

    void TemporalAASample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void TemporalAASample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() &&
            m_temporalCapabilityPublished && m_temporalExecutionQualified;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("EnginePostProcessConfiguration");
        reporter.Enable("TemporalAARequested");
        reporter.Enable("VelocityOutputRequested");
        reporter.Enable("DeterministicModelMotion");
        reporter.Enable("FXAAExplicitlyDisabled");
        if (m_temporalExecutionQualified)
        {
            reporter.Enable("TemporalAAActualExecutionQualified");
            reporter.Enable("TemporalHistorySampled");
            reporter.Enable("TemporalHistoryCommittedAfterPresent");
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        reporter.ResourceDiagnostic(
            "temporal ownership=Render/PostProcess; sample ownership=deterministic ECS motion and public AA policy");
    }

    void TemporalAASample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        m_temporalExecutionQualified = diagnostics.postProcessActualExecution
            .IsTemporalAAQualificationReady();
        if (m_temporalExecutionQualified)
        {
            PublishTemporalCapability(
                assessment, m_temporalCapabilityPublished);
        }
    }

    SampleReadiness TemporalAASample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Temporal-AA showcase is waiting for the model to become fully resident");
        }
        if (!m_baseTransformCaptured)
        {
            return SampleReadiness::Pending(
                "Temporal-AA showcase is waiting for deterministic ECS motion");
        }
        if (diagnostics.visibleObjectCount == 0 || !diagnostics.graphCompiled ||
            !diagnostics.postProcessActualExecution
                 .IsTemporalAAQualificationReady())
        {
            return SampleReadiness::Pending(
                "Waiting for the renderer-owned temporal pipeline: " +
                DescribeTemporalState(diagnostics));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Temporal-AA showcase is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool TemporalAASample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void TemporalAASample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure = "Failed to cancel the temporal-AA ECS model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_baseTranslation = Vec3(0.0f);
        m_simulationSeconds = 0.0f;
        m_baseTransformCaptured = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_temporalCapabilityPublished = false;
        m_temporalExecutionQualified = false;
    }
} // namespace RVX
