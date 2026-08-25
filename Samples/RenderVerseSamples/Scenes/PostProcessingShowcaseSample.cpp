/** @file PostProcessingShowcaseSample.cpp @brief Typed post-processing demonstration scene. */

#include "Scenes/PostProcessingShowcaseSample.h"

#include "Core/MathTypes.h"
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
        const SampleInfo PostProcessingShowcaseInfo{
            "post-processing",
            "Post Processing",
            "Demonstrates renderer-owned GTAO, Bloom, tone mapping, color grading, and FXAA in a deterministic interior",
            "interior-rendering-p0a",
            SampleAssetPolicy::Fixed,
            "pbr-reference-environment",
            SampleEnvironmentPolicy::Required,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentCapability BloomCapability{
            AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_BLOOM"),
            "Bloom is requested by the deterministic post-processing sample.",
            true,
            "Post-processing qualification requires Bloom."};
        const AssessmentCapability ToneMappingCapability{
            AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_TONE_MAPPING"),
            "Tone mapping is requested by the deterministic post-processing sample.",
            true,
            "Post-processing qualification requires one tone-mapping transform."};
        const AssessmentCapability ColorGradingCapability{
            AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_COLOR_GRADING"),
            "Color grading is requested by the deterministic post-processing sample.",
            true,
            "Post-processing qualification requires color grading."};
        const AssessmentCapability FXAACapability{
            AssessmentCode("RENDER.CAPABILITY.POST_PROCESS_FXAA"),
            "FXAA is requested by the deterministic post-processing sample.",
            true,
            "Post-processing qualification requires FXAA."};
        const AssessmentCapability GTAOCapability{
            AssessmentCode("RENDER.CAPABILITY.GTAO"),
            "GTAO is requested through the public ambient-occlusion policy.",
            true,
            "GTAO qualification requires bilateral, temporal, indirect-only, and probe evidence."};

        void PublishRequestedPostProcessCapability(
            SampleAssessmentChannel& assessment,
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
            return "Post-processing sample model request reached a terminal state.";
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
            return "Post-processing sample environment request reached a terminal state.";
        }

        [[nodiscard]] std::string DescribePipelineState(
            const SampleRenderDiagnostics& diagnostics)
        {
            std::ostringstream stream;
            stream << "graphCompiled=" << diagnostics.graphCompiled
                   << ", graphPasses=" << diagnostics.renderGraphTotalPasses
                   << ", requestedPostEffects="
                   << diagnostics.requestedPostProcessEffectCount
                   << ", enabledPostEffects="
                   << diagnostics.enabledPostProcessEffectCount
                   << ", unsupportedPostEffects="
                   << diagnostics.unsupportedPostProcessSkippedCount
                   << ", postProcessGraphPasses="
                   << diagnostics.postProcessGraphPassCount
                   << ", actualExecutionQualified="
                   << diagnostics.postProcessActualExecution
                          .IsQualificationReady()
                   << ", actualExecutionFrame="
                   << diagnostics.postProcessActualExecution
                          .receiptFrameSequence
                   << ", presentedFrame="
                   << diagnostics.postProcessActualExecution
                          .lastPresentedFrameSequence;
            return stream.str();
        }
    } // namespace

    const SampleInfo& PostProcessingShowcaseSample::GetInfo() const noexcept
    {
        return PostProcessingShowcaseInfo;
    }

    SampleAssessmentContract
        PostProcessingShowcaseSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.POST_PROCESSING");
        contract.revision = "2";
        contract.capabilities = {
            BloomCapability,
            ToneMappingCapability,
            ColorGradingCapability,
            FXAACapability,
            GTAOCapability};
        return contract;
    }

    bool PostProcessingShowcaseSample::Setup(SampleContext& context,
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
                context.camera, radians(75.0f), aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 2.4f, 6.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 2.3f, -4.0f)))
        {
            outError = "Failed to configure the post-processing ECS camera";
            return false;
        }

        SampleEnvironmentLoadOptions environmentOptions;
        environmentOptions.quality = context.options.quality;
        environmentOptions.smoke = context.options.smoke;
        environmentOptions.exposure = 1.0f;
        const bool environmentRequested =
            !context.options.environmentAssetId.empty()
                ? context.environments.RequestByAssetId(
                      context.options.environmentAssetId,
                      environmentOptions, m_environment, outError)
                : context.environments.Request(
                      context.options.environmentPath,
                      context.options.environmentContentIdentity,
                      environmentOptions, m_environment, outError);
        if (!environmentRequested)
        {
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
            outError = "Failed to create the post-processing ECS light";
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

        // This is a pure public policy request.  The Render feature registry
        // owns all passes, input/output color domains, and execution receipts.
        auto& postProcess = context.renderSettings.postProcess;
        postProcess.enabled = true;
        postProcess.antiAliasingMode = RenderAntiAliasingMode::FXAA;
        postProcess.enableVelocityOutput = false;
        postProcess.enableBloom = true;
        postProcess.enableColorGrading = true;
        postProcess.ambientOcclusion.enabled = true;
        postProcess.enableSSR = false;
        postProcess.toneMappingOperator = RenderToneMappingOperator::ACES;
        postProcess.exposureSettings.mode = RenderExposureMode::ManualEV100;
        postProcess.exposureSettings.manualEV100 = 0.0f;
        postProcess.exposureSettings.exposureCompensationEV = 0.0f;
        postProcess.gamma = 2.2f;
        postProcess.bloomThreshold = 0.65f;
        postProcess.bloomIntensity = 0.35f;
        postProcess.bloomRadius = 0.60f;
        postProcess.colorGradingContrast = 1.06f;
        postProcess.colorGradingSaturation = 1.04f;
        postProcess.colorGradingBrightness = 0.0f;
        return context.models.RequestByAssetId(
            context.options.assetId, m_model, outError);
    }

    void PostProcessingShowcaseSample::Update(SampleContext& context,
                                               float deltaTime)
    {
        static_cast<void>(deltaTime);

        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        const ResourceSceneAdapters::EcsEnvironmentLoadStatus environmentStatus =
            context.environments.UpdateReadiness(m_environment);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
        }
        if (environmentStatus.IsTerminal() && !m_environment.IsValid() &&
            m_modelFailure.empty())
        {
            m_modelFailure = DescribeEnvironmentFailure(environmentStatus);
        }
    }

    void PostProcessingShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void PostProcessingShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        const bool stable = m_model.IsFullyResident() &&
            m_bloomCapabilityPublished &&
            m_toneMappingCapabilityPublished &&
            m_colorGradingCapabilityPublished &&
            m_fxaaCapabilityPublished && m_postProcessExecutionQualified;
        reporter.SetScenarioContractRevision(2);
        reporter.SetScenarioPhase(stable ? "stable" :
                                  m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
        }
        if (m_environment.IsValid())
        {
            reporter.Enable("EnvironmentResourceLoad");
            reporter.Enable("EnvironmentIBL");
            reporter.Enable("DiffuseIBL");
            reporter.Enable("SpecularIBL");
        }
        reporter.Enable("EnginePostProcessConfiguration");
        reporter.Enable("BloomRequested");
        reporter.Enable("ACESToneMappingRequested");
        reporter.Enable("ColorGradingRequested");
        reporter.Enable("FXAARequested");
        reporter.Enable("TemporalAAExplicitlyDisabled");
        reporter.Enable("AmbientOcclusionRequested");
        if (m_ambientOcclusionExecutionReady)
        {
            reporter.Enable("AmbientOcclusionExecutionReady");
        }
        if (m_ambientOcclusionQualificationReady)
        {
            reporter.Enable("AmbientOcclusionActualExecutionQualified");
            reporter.Enable("AmbientOcclusionBilateralFiltering");
            reporter.Enable("AmbientOcclusionTemporalHistorySampled");
            reporter.Enable("AmbientOcclusionHistoryCommittedAfterPresent");
            reporter.Enable("AmbientOcclusionIndirectOnlyLighting");
            reporter.Enable("AmbientOcclusionNonNeutralProbe");
        }
        if (m_postProcessExecutionQualified)
        {
            reporter.Enable("PostProcessActualExecutionQualified");
        }
        reporter.ResourceDiagnostic(
            "post-process ownership=Render/PostProcess feature pipeline");
        reporter.ResourceDiagnostic(
            "sample ownership=scene and public post-process policy configuration");
        if (m_skyboxCreated && m_environment.IsValid())
        {
            reporter.Enable("EnvironmentSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
    }

    void PostProcessingShowcaseSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        m_postProcessExecutionQualified =
            diagnostics.postProcessActualExecution.IsQualificationReady();
        m_ambientOcclusionExecutionReady =
            diagnostics.ambientOcclusionActualExecution.IsExecutionReady();
        m_ambientOcclusionQualificationReady =
            diagnostics.ambientOcclusionActualExecution.IsQualificationReady();
        if (!m_postProcessExecutionQualified)
        {
            return;
        }

        // Capability publication requires actual execution evidence from the
        // exact main view that reached presentation. The sample never infers
        // execution from settings or graph scheduling.
        PublishRequestedPostProcessCapability(
            assessment, BloomCapability,
            "Bloom execution was observed in the presented post-process receipt.",
            m_bloomCapabilityPublished);
        PublishRequestedPostProcessCapability(
            assessment, ToneMappingCapability,
            "Tone mapping execution was observed in the presented post-process receipt.",
            m_toneMappingCapabilityPublished);
        PublishRequestedPostProcessCapability(
            assessment, ColorGradingCapability,
            "Color grading execution was observed in the presented post-process receipt.",
            m_colorGradingCapabilityPublished);
        PublishRequestedPostProcessCapability(
            assessment, FXAACapability,
            "FXAA execution was observed in the presented post-process receipt.",
            m_fxaaCapabilityPublished);
        if (m_ambientOcclusionQualificationReady)
        {
            PublishRequestedPostProcessCapability(
                assessment, GTAOCapability,
                "The presented GTAO receipt proves bilateral filtering, temporal history, indirect-only consumers, and a non-neutral probe.",
                m_ambientOcclusionCapabilityPublished);
        }
    }

    SampleReadiness PostProcessingShowcaseSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
        {
            return SampleReadiness::Failed(m_modelFailure);
        }
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Post-processing showcase is waiting for the model to become fully resident");
        }
        if (m_environment.status.IsTerminal() && !m_environment.IsValid())
        {
            return SampleReadiness::Failed(
                DescribeEnvironmentFailure(m_environment.status));
        }
        if (!m_environment.IsValid() || !diagnostics.textureIBLEnabled)
        {
            return SampleReadiness::Pending(
                "Post-processing showcase is waiting for realized reference IBL");
        }
        const bool ready = diagnostics.visibleObjectCount > 0 &&
            diagnostics.graphCompiled && diagnostics.renderGraphTotalPasses >= 8 &&
            diagnostics.postProcessActualExecution.IsQualificationReady();
        if (!ready)
        {
            return SampleReadiness::Pending(
                "Waiting for the engine-owned post-process pipeline: " +
                DescribePipelineState(diagnostics));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Post-processing showcase is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool PostProcessingShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void PostProcessingShowcaseSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure =
                "Failed to cancel the post-processing ECS model request.";
        }
        if (m_environment.request.IsValid() &&
            !context.environments.Cancel(m_environment))
        {
            if (!m_modelFailure.empty())
            {
                m_modelFailure += ' ';
            }
            m_modelFailure +=
                "Failed to cancel the post-processing ECS environment request.";
        }
        if (!m_model.request.IsValid())
        {
            m_model = {};
        }
        if (!m_environment.request.IsValid())
        {
            m_environment = {};
        }
        if (!m_model.request.IsValid() && !m_environment.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_bloomCapabilityPublished = false;
        m_toneMappingCapabilityPublished = false;
        m_colorGradingCapabilityPublished = false;
        m_fxaaCapabilityPublished = false;
        m_ambientOcclusionCapabilityPublished = false;
        m_postProcessExecutionQualified = false;
        m_ambientOcclusionExecutionReady = false;
        m_ambientOcclusionQualificationReady = false;
    }
} // namespace RVX
