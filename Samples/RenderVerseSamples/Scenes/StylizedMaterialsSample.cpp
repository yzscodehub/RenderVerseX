/** @file StylizedMaterialsSample.cpp @brief Cooked-only ToonLit qualification scene. */

#include "Scenes/StylizedMaterialsSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
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
        const SampleInfo StylizedMaterialsInfo = []
        {
            SampleInfo info;
            info.id = "stylized-materials";
            info.displayName = "Stylized Materials";
            info.description =
                "Qualifies cooked ToonLit shading and renderer-owned screen-space outlines";
            info.defaultAssetId = "stylized-materials-toon";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.environmentPolicy = SampleEnvironmentPolicy::None;
            info.supportsRenderPathSelection = true;
            info.defaultRenderPath = SampleRenderPath::Auto;
            info.requiredCookedAssetIds = {"stylized-materials-toon"};
            info.domain = SampleDomain::Rendering;
            info.intent = SampleIntent::Regression;
            info.visualContractId = "stylized-materials-v1";
            info.deterministicQualification = true;
            return info;
        }();

        const AssessmentCapability ToonLitCapability{
            AssessmentCode("RENDER.CAPABILITY.TOON_LIT"),
            "Cooked ToonLit parameters are consumed by the selected renderer recipe.",
            true,
            "Qualification requires a renderer-owned ToonLit execution receipt."};
        const AssessmentCapability ToonOutlineCapability{
            AssessmentCode("RENDER.CAPABILITY.TOON_SCREEN_SPACE_OUTLINE"),
            "Renderer-owned HDR outlines use typed depth, normal, and material identity inputs.",
            true,
            "Qualification requires a renderer-owned toon-outline probe receipt."};

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

        [[nodiscard]] std::string DescribeToonExecutionState(
            const SampleRenderDiagnostics& diagnostics)
        {
            const SampleToonLightingExecutionDiagnostics& lighting =
                diagnostics.toonLightingActualExecution;
            const SampleToonOutlineExecutionDiagnostics& outline =
                diagnostics.toonOutlineActualExecution;
            return "lighting(requested=" + std::to_string(lighting.requested) +
                ", pipeline=" + std::to_string(lighting.pipelineReady) +
                ", inputs=" + std::to_string(lighting.inputsReady) +
                ", executed=" + std::to_string(lighting.executed) +
                ") outline(requested=" + std::to_string(outline.requested) +
                ", pipeline=" + std::to_string(outline.pipelineReady) +
                ", inputs=" + std::to_string(outline.inputsReady) +
                ", executed=" + std::to_string(outline.executed) +
                ", qualification=" +
                std::to_string(outline.qualificationReady) + ")";
        }

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(-0.35f, -0.60f, -0.45f)),
                .sunColor = Vec3(1.0f, 0.93f, 0.82f),
                .zenithColor = Vec3(0.06f, 0.12f, 0.29f),
                .horizonColor = Vec3(0.30f, 0.40f, 0.62f),
                .groundColor = Vec3(0.025f, 0.030f, 0.040f),
                .scatteringIntensity = 0.55f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.93f, 0.84f),
                .intensity = 18000.0f,
                .intensityUnit = SceneECS::LightIntensityUnit::Lux,
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
            return "Stylized-materials cooked ToonLit request reached a terminal state.";
        }
    } // namespace

    const SampleInfo& StylizedMaterialsSample::GetInfo() const noexcept
    {
        return StylizedMaterialsInfo;
    }

    SampleAssessmentContract StylizedMaterialsSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.STYLIZED_MATERIALS");
        contract.revision = "1";
        contract.capabilities = {ToonLitCapability, ToonOutlineCapability};
        return contract;
    }

    bool StylizedMaterialsSample::Setup(SampleContext& context,
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
                context.camera, radians(42.0f), aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 1.35f, 4.2f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.90f, 0.0f)))
        {
            outError = "Failed to configure the stylized-materials ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the stylized-materials ECS skybox";
            return false;
        }
        m_skyCreated = true;

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeDirectionalLight(), SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the stylized-materials directional light";
            return false;
        }
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024u : 2048u;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1u : 3u;
        context.renderSettings.shadows.maxDistance = 60.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;

        // The sample requests only public temporal/HDR policy. Toon lighting,
        // material classification, outlines, graph ordering, and all probe
        // data remain renderer-owned. There is intentionally no source-model
        // fallback if the cooked catalog package has not been staged.
        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.enableVelocityOutput = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::TAA;
        postProcess.enableBloom = true;
        postProcess.enableColorGrading = false;
        postProcess.enableSSR = false;
        postProcess.enableToonOutline = true;
        postProcess.toonOutlineDepthThreshold = 0.015f;
        postProcess.toonOutlineNormalThreshold = 0.20f;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = 9.0f;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.bloomThreshold = 1.25f;
        postProcess.bloomIntensity = 0.20f;
        postProcess.bloomRadius = 0.55f;

        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    void StylizedMaterialsSample::Update(SampleContext& context,
                                         float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal() && !m_model.IsFullyResident())
        {
            m_modelFailure = DescribeModelFailure(status);
        }
    }

    void StylizedMaterialsSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void StylizedMaterialsSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool resident = m_model.IsFullyResident();
        const bool stable = resident && m_toonLightingExecutionObserved &&
            m_toonOutlineExecutionObserved;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("CookedToonAssetAdmissionRequired");
        reporter.Enable("ToonLitRequested");
        reporter.Enable("ToonOutlineRequested");
        reporter.Enable("TemporalAARequested");
        reporter.Enable("VelocityOutputRequested");
        reporter.Enable("ACESToneMappingRequested");
        reporter.ResourceDiagnostic(
            "stylized-materials requires the catalog-admitted cooked model id "
            "stylized-materials-toon; source fallback is forbidden");
        if (resident)
        {
            reporter.Enable("CookedToonAssetResident");
        }
        if (m_skyCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        if (m_toonLightingExecutionObserved)
        {
            reporter.Enable("ToonLightingActualExecutionQualified");
        }
        if (m_toonOutlineQualificationObserved)
        {
            reporter.Enable("ToonOutlineActualExecutionQualified");
        }
    }

    void StylizedMaterialsSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const bool lightingReady =
            diagnostics.toonLightingActualExecution.IsExecutionReady();
        const bool outlineExecuted =
            diagnostics.toonOutlineActualExecution.IsExecutionReady();
        const bool outlineQualified =
            diagnostics.toonOutlineActualExecution.IsQualificationReady();

        m_toonLightingExecutionObserved =
            m_toonLightingExecutionObserved || lightingReady;
        m_toonOutlineExecutionObserved =
            m_toonOutlineExecutionObserved || outlineExecuted;
        m_toonOutlineQualificationObserved =
            m_toonOutlineQualificationObserved || outlineQualified;

        if (lightingReady)
        {
            PublishCapability(
                assessment,
                ToonLitCapability,
                "The exact presented schema-v11 main-view receipt proves ToonLit material binding and raster execution.",
                m_toonLightingCapabilityPublished);
        }
        if (outlineQualified)
        {
            PublishCapability(
                assessment,
                ToonOutlineCapability,
                "The exact presented schema-v11 main-view receipt proves HDR outline ordering and its post-fence material classification readback.",
                m_toonOutlineCapabilityPublished);
        }
    }

    SampleReadiness StylizedMaterialsSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Stylized-materials is waiting for its catalog-admitted cooked ToonLit package");
        }

        // This is deliberately execution-only readiness. Qualification is an
        // explicit host mode; making its readback mandatory here would make
        // the ordinary interactive showcase fail after rendering correctly.
        if (!diagnostics.toonLightingActualExecution.IsExecutionReady() ||
            !diagnostics.toonOutlineActualExecution.IsExecutionReady())
        {
            return SampleReadiness::Pending(
                "Stylized-materials is waiting for renderer-owned ToonLit and outline execution: " +
                DescribeToonExecutionState(diagnostics));
        }
        return SampleReadiness::Ready();
    }

    bool StylizedMaterialsSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        if (!readiness.IsReady())
        {
            outError = readiness.reason;
            return false;
        }
        if (!diagnostics.toonLightingActualExecution.IsExecutionReady())
        {
            outError = "Stylized-materials lacks presented ToonLit execution evidence";
            return false;
        }
        if (!diagnostics.toonOutlineActualExecution.IsExecutionReady())
        {
            outError =
                "Stylized-materials lacks presented Toon outline execution evidence";
            return false;
        }
        outError.clear();
        return true;
    }

    void StylizedMaterialsSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure =
                "Failed to cancel the stylized-materials cooked model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_model = {};
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_skyCreated = false;
        m_lightCreated = false;
        m_toonLightingExecutionObserved = false;
        m_toonOutlineExecutionObserved = false;
        m_toonOutlineQualificationObserved = false;
        m_toonLightingCapabilityPublished = false;
        m_toonOutlineCapabilityPublished = false;
    }
} // namespace RVX
