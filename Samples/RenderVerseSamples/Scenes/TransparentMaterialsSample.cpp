/** @file TransparentMaterialsSample.cpp @brief Cooked-only GFX15 transparency scene. */

#include "Scenes/TransparentMaterialsSample.h"

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
        const SampleInfo TransparentMaterialsInfo = []
        {
            SampleInfo info;
            info.id = "transparent-materials";
            info.displayName = "Transparent Materials";
            info.description =
                "Qualifies cooked AlphaBlend, Premultiplied, Additive, and ThinTransmission materials";
            info.defaultAssetId = "transparent-materials-v1";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.environmentPolicy = SampleEnvironmentPolicy::None;
            info.supportsRenderPathSelection = true;
            info.defaultRenderPath = SampleRenderPath::Auto;
            info.requiredCookedAssetIds = {"transparent-materials-v1"};
            info.domain = SampleDomain::Rendering;
            info.intent = SampleIntent::Regression;
            info.visualContractId = "transparent-materials-v1";
            info.deterministicQualification = true;
            return info;
        }();

        const AssessmentCapability TranslucencyCapability{
            AssessmentCode("RENDER.CAPABILITY.SORTED_TRANSLUCENCY"),
            "The renderer executes the four cooked GFX15 translucency material classes with a copied opaque snapshot and reactive-mask output.",
            true,
            "Qualification requires renderer-owned category draw, ordering, snapshot, and reactive-mask receipts."};

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
                "The exact presented frame reports all four cooked translucent classes without a renderer fallback.";
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
                diagnostics.transparentOpaqueSceneSnapshotReady &&
                diagnostics.transparentOpaqueSceneSnapshotCopied &&
                diagnostics.transparentReactiveMaskAttachmentBound &&
                diagnostics.transparentReactiveMaskWriteDrawCount != 0 &&
                diagnostics.transparentPriorityBucketCount >= 4 &&
                diagnostics.transparentFallbackCount == 0;
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
        contract.revision = "1";
        contract.capabilities = {TranslucencyCapability};
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

        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    void TransparentMaterialsSample::Update(SampleContext& context,
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

    void TransparentMaterialsSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void TransparentMaterialsSample::AppendReport(SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() &&
            m_translucencyExecutionObserved;
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        reporter.Enable("CookedTransparentMaterialsAssetAdmissionRequired");
        reporter.Enable("AlphaBlendRequested");
        reporter.Enable("PremultipliedBlendRequested");
        reporter.Enable("AdditiveBlendRequested");
        reporter.Enable("ThinTransmissionRequested");
        reporter.Enable("SortedTranslucencyRequested");
        reporter.ResourceDiagnostic(
            "transparent-materials requires catalog-admitted cooked asset "
            "transparent-materials-v1; source fallback is forbidden");
        if (m_model.IsFullyResident())
        {
            reporter.Enable("CookedTransparentMaterialsAssetResident");
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
    }

    void TransparentMaterialsSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const bool executed = HasTranslucencyExecution(diagnostics);
        m_translucencyExecutionObserved = m_translucencyExecutionObserved || executed;
        if (executed)
        {
            PublishCapability(assessment, TranslucencyCapability,
                              m_translucencyCapabilityPublished);
        }
    }

    SampleReadiness TransparentMaterialsSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for its catalog-admitted cooked package");
        }
        if (!HasTranslucencyExecution(diagnostics))
        {
            return SampleReadiness::Pending(
                "transparent-materials is waiting for renderer-owned category draw, opaque snapshot, reactive-mask, sorting, and no-fallback receipts");
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
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure =
                "Failed to cancel the transparent-materials cooked model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_model = {};
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_skyCreated = false;
        m_lightCreated = false;
        m_translucencyExecutionObserved = false;
        m_translucencyCapabilityPublished = false;
    }
} // namespace RVX
