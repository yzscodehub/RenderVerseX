/** @file ModelViewerSample.cpp @brief Interactive model inspection scene. */

#include "Scenes/ModelViewerSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Samples/SampleScenarioDiagnostics.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace RVX
{
    namespace
    {
        constexpr float32 ModelViewerVerticalFov = 0.78539816339f;

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(0.35f, 0.65f, 0.45f)),
                .sunColor = Vec3(1.0f, 0.94f, 0.84f),
                .zenithColor = Vec3(0.10f, 0.24f, 0.52f),
                .horizonColor = Vec3(0.55f, 0.67f, 0.80f),
                .groundColor = Vec3(0.07f, 0.08f, 0.10f),
                .scatteringIntensity = 0.65f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight(float32 intensity)
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.95f, 0.86f),
                .intensity = intensity,
                .intensityUnit = SceneECS::LightIntensityUnit::Lux,
                .shadowBias = 0.001f,
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
            return "Model Viewer ECS model request reached a terminal state.";
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
            return "Model Viewer ECS environment request reached a terminal state.";
        }

        const SampleInfo ModelViewerInfo{
            "model-viewer",
            "Model Viewer",
            "Loads, frames, and interactively inspects a catalog or user model",
            "r7-triangle",
            SampleAssetPolicy::UserModelOrDefault,
            "",
            SampleEnvironmentPolicy::Optional,
            true,
            SampleRenderPath::Auto,
        };

        const AssessmentCapability CookedModelRasterCapability{
            AssessmentCode("RENDER.CAPABILITY.COOKED_MODEL_RASTER"),
            "A catalog-admitted cooked StandardLit model reaches the presented opaque raster path.",
            true,
            "Qualification requires Material V2 facts from a renderer-owned presented binding receipt, no fallback or overflow, and qualified Direct or GPU-driven execution."};

        void PublishCookedModelRasterCapability(
            SampleAssessmentChannel& assessment,
            bool& published)
        {
            if (published)
            {
                return;
            }

            AssessmentCapabilityObservation observation;
            observation.capability = CookedModelRasterCapability;
            observation.checkpoint = AssessmentCheckpoints::ScenarioStable;
            observation.value = DiagnosticValue<bool>::Available(true);
            observation.reason =
                "A present-bound Material V2 StandardLit opaque receipt proved cooked model raster execution without fallback.";
            if (assessment.MarkCapabilityObservation(std::move(observation)))
            {
                published = true;
            }
        }
    } // namespace

    const SampleInfo& ModelViewerSample::GetInfo() const noexcept
    {
        return ModelViewerInfo;
    }

    SampleAssessmentContract ModelViewerSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract = MakeDefaultSampleAssessmentContract();
        contract.code = AssessmentCode("SAMPLE.MODEL_VIEWER");
        contract.revision = "2";
        contract.capabilities = {CookedModelRasterCapability};
        return contract;
    }

    bool ModelViewerSample::Setup(SampleContext& context,
                                  std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        const float32 initialAspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, ModelViewerVerticalFov, initialAspect, 0.05f, 1000.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 0.0f, 3.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f)))
        {
            outError = "Failed to configure the Model Viewer ECS camera";
            return false;
        }

        m_textureEnvironment = !context.options.environmentPath.empty();
        if (m_textureEnvironment)
        {
            SampleEnvironmentLoadOptions environmentOptions;
            environmentOptions.quality = context.options.quality;
            environmentOptions.smoke = context.options.smoke;
            environmentOptions.exposure = 1.0f;
            // The environment coordinator owns its sole Skybox entity.  A sample
            // procedural Skybox would make that atomic adoption ambiguous.
            const bool requested = !context.options.environmentAssetId.empty()
                ? context.environments.RequestByAssetId(
                      context.options.environmentAssetId,
                      environmentOptions,
                      m_environment,
                      outError)
                : context.environments.Request(
                      context.options.environmentPath,
                      context.options.environmentContentIdentity,
                      environmentOptions,
                      m_environment,
                      outError);
            if (!requested)
            {
                return false;
            }
        }
        else if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                     {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the Model Viewer ECS skybox";
            return false;
        }
        m_skyboxCreated = !m_textureEnvironment;

        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-50.0f), radians(25.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(m_textureEnvironment ? 1.5f : 4.0f),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the Model Viewer ECS light";
            return false;
        }
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024u : 2048u;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1u : 3u;
        const bool temporalQuality =
            context.options.qualityTier == SampleQualityTier::High ||
            context.options.qualityTier == SampleQualityTier::Ultra;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableVelocityOutput =
            temporalQuality;
        context.renderSettings.postProcess.antiAliasingMode =
            temporalQuality ? RenderAntiAliasingMode::TAA
                            : RenderAntiAliasingMode::FXAA;
        context.renderSettings.postProcess.ambientOcclusion.enabled =
            temporalQuality;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSR = false;
        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::Auto;
                break;
            case SampleRenderPath::Direct:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceDisabled;
                break;
            case SampleRenderPath::GPUDriven:
                context.renderSettings.gpuCulling.mode =
                    RenderGPUDrivenMode::ForceEnabled;
                break;
            default:
                outError = "Model Viewer received an invalid render path";
                return false;
        }
        if (!context.options.assetId.empty())
        {
            return context.models.RequestByAssetId(
                context.options.assetId, m_model, outError);
        }
        if (!context.options.modelPath.empty())
        {
            return context.models.Request(
                context.options.modelPath, m_model, outError);
        }
        outError = "Model Viewer requires a catalog asset id or an admitted model path";
        return false;
    }

    bool ModelViewerSample::ActivateModel(SampleContext& context)
    {
        m_modelActivationAttempted = true;
        if (!TryComputeSampleModelRenderableWorldBounds(m_model, context.scene, m_bounds))
        {
            m_modelActivationError =
                "Loaded ECS model has no valid current renderable hierarchy bounds";
            return false;
        }
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(
            m_bounds,
            aspect,
            ModelViewerVerticalFov);
        if (!m_cameraFrame.valid)
        {
            m_modelActivationError =
                "Loaded model has no finite renderable bounds for camera framing";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::ExteriorInspect;
        orbitSettings.bounds = m_bounds;
        orbitSettings.pivot = m_cameraFrame.target;
        orbitSettings.distance = m_cameraFrame.distance;
        orbitSettings.pitch = 0.35877067f;
        orbitSettings.maxDistance =
            std::max(m_cameraFrame.distance * 20.0f, 0.001f);
        orbitSettings.zoomExponent = 0.08f;
        // Inspection zoom keeps enough framing to preserve depth precision for
        // near-coplanar material layers such as automotive paint and clear coat.
        orbitSettings.minimumFramingScale = 0.55f;
        orbitSettings.verticalFovRadians = ModelViewerVerticalFov;
        orbitSettings.aspectRatio = aspect;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            m_modelActivationError =
                "Model Viewer could not initialize its orbit camera";
            return false;
        }
        if (!m_orbitCamera.Apply(context.cameras, context.camera))
        {
            m_modelActivationError = "Model Viewer could not apply its ECS orbit camera pose";
            return false;
        }
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        m_cameraFrame.valid = orbitPose.valid;
        m_modelActivated = true;
        return true;
    }

    void ModelViewerSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (m_model.request.IsValid())
        {
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
                context.models.UpdateReadiness(m_model);
            if (status.IsTerminal())
            {
                m_modelActivationError = DescribeModelFailure(status);
            }
        }
        if (m_textureEnvironment && m_environment.request.IsValid())
        {
            const ResourceSceneAdapters::EcsEnvironmentLoadStatus status =
                context.environments.UpdateReadiness(m_environment);
            if (status.IsTerminal())
            {
                m_environmentFailure = DescribeEnvironmentFailure(status);
            }
        }

        if (!m_modelActivationAttempted && m_model.IsCPUReady())
        {
            if (!ActivateModel(context))
                static_cast<void>(context.models.Cancel(m_model));
        }
        if (m_model.IsFullyResident())
        {
            // Renderable activation is owned by SceneAssetLoadCoordinator so
            // model instances, resource lifetime and extraction all use one
            // transaction boundary. The sample records its observable state.
            m_renderablesEnabled = true;
        }
        if (m_renderablesEnabled &&
            m_modelActivated &&
            context.options.deterministicCameraOrbit &&
            !m_qualificationCapturePrepared)
        {
            SampleOrbitCameraInput input;
            input.orbitActive = true;
            const float phase =
                static_cast<float>(m_automationFrameCount) * 0.041f;
            input.pointerDelta = Vec2(0.8f, std::sin(phase) * 0.18f);
            const uint32 zoomPhase = m_automationFrameCount % 240u;
            if (zoomPhase == 60u)
            {
                input.scrollDelta = 1.0f;
                ++m_automationZoomEventCount;
            }
            else if (zoomPhase == 180u)
            {
                input.scrollDelta = -1.0f;
                ++m_automationZoomEventCount;
            }
            static_cast<void>(
                m_orbitCamera.ApplyInput(input, context.cameras, context.camera));
            ++m_automationFrameCount;
        }
    }

    bool ModelViewerSample::PrepareQualificationCapture(
        SampleContext& context,
        std::string& outError)
    {
        if (!context.options.deterministicCameraOrbit)
        {
            m_qualificationCapturePrepared = true;
            return true;
        }
        if (!m_modelActivated || !m_orbitCamera.IsInitialized())
        {
            outError =
                "Model Viewer cannot prepare a deterministic capture before camera activation";
            return false;
        }

        m_orbitCamera.Reset();
        if (!m_orbitCamera.Apply(context.cameras, context.camera))
        {
            outError = "Model Viewer could not reset its ECS orbit camera";
            return false;
        }
        m_qualificationCapturePrepared = true;
        return true;
    }

    void ModelViewerSample::OnInput(SampleContext& context)
    {
        if (m_modelActivated && m_orbitCamera.IsInitialized() && context.input)
        {
            static_cast<void>(
                m_orbitCamera.Update(*context.input, context.cameras, context.camera));
        }
    }

    void ModelViewerSample::OnViewportResize(SampleContext& context,
                                             uint32 width,
                                             uint32 height)
    {
        if (width == 0 || height == 0)
        {
            return;
        }
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            static_cast<float>(width) / static_cast<float>(height),
            context.cameras,
            context.camera));
        if (!m_orbitCamera.IsInitialized())
        {
            static_cast<void>(context.cameras.SetAspectRatio(
                context.camera,
                static_cast<float>(width) / static_cast<float>(height)));
        }
    }

    void ModelViewerSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("ModelInspection");
            reporter.Enable("RenderPathPolicySelection");
            reporter.ResourceDiagnostic(
                "render path=" +
                std::string(GetSampleRenderPathName(m_renderPath)));
            if (m_renderPath == SampleRenderPath::GPUDriven)
            {
                reporter.Enable("GPUDrivenPathRequested");
            }
            else if (m_renderPath == SampleRenderPath::Direct)
            {
                reporter.Enable("DirectPathRequested");
            }
            if (m_model.IsFullyResident() && m_renderablesEnabled)
            {
                reporter.Enable("ModelRenderReady");
            }
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(m_model.status.modelMetadata.meshAssetIds.size()));
            reporter.ResourceDiagnostic(
                "model materials=" +
                std::to_string(m_model.status.modelMetadata.materialAssetIds.size()));
            reporter.ResourceDiagnostic(
                "model textures=" +
                std::to_string(m_model.status.modelMetadata.textures.size()));
            uint64 materialExtensionFeatureMask = 0;
            for (const auto& material :
                 m_model.status.modelMetadata.materials)
            {
                materialExtensionFeatureMask |=
                    material.featureMask &
                    RVX_MATERIAL_EXTENSION_FEATURE_MASK_V2;
            }
            reporter.ResourceDiagnostic(
                "model material extension feature mask=" +
                std::to_string(materialExtensionFeatureMask));
            reporter.ResourceDiagnostic(
                "model nodes=" +
                std::to_string(m_model.status.modelMetadata.sourceNodeCount));
            if (m_cameraFrame.valid)
            {
                reporter.Enable("BoundsCameraFraming");
                reporter.Enable("OrbitCamera");
                if (m_automationFrameCount != 0)
                {
                    reporter.Enable("DeterministicOrbitCamera");
                    reporter.Enable("OrbitZoomAutomation");
                    reporter.ResourceDiagnostic(
                        "deterministic orbit frames=" +
                        std::to_string(m_automationFrameCount));
                    reporter.ResourceDiagnostic(
                        "deterministic zoom events=" +
                        std::to_string(m_automationZoomEventCount));
                }
                reporter.ResourceDiagnostic(
                    "model bounds min=" + std::to_string(m_bounds.GetMin().x) +
                    "," + std::to_string(m_bounds.GetMin().y) + "," +
                    std::to_string(m_bounds.GetMin().z));
                reporter.ResourceDiagnostic(
                    "model bounds max=" + std::to_string(m_bounds.GetMax().x) +
                    "," + std::to_string(m_bounds.GetMax().y) + "," +
                    std::to_string(m_bounds.GetMax().z));
                reporter.ResourceDiagnostic(
                    "camera distance=" +
                    std::to_string(m_cameraFrame.distance));
                if (m_qualificationCapturePrepared)
                {
                    reporter.Enable("QualificationCaptureReset");
                }
            }
        }
        if (m_environment.IsValid())
        {
            reporter.Enable("EnvironmentResourceLoad");
            reporter.Enable("EnvironmentIBL");
            reporter.Enable("DiffuseIBL");
            reporter.Enable("SpecularIBL");
            reporter.Enable("TextureEnvironmentScene");
            reporter.ResourceDiagnostic(
                "environment path=" + m_environment.sourcePath.string());
            reporter.ResourceDiagnostic(
                "environment cubemap resolution=" +
                std::to_string(m_environment.environmentResolution));
            reporter.ResourceDiagnostic(
                "environment irradiance resolution=" +
                std::to_string(m_environment.irradianceResolution));
            reporter.ResourceDiagnostic(
                "environment prefiltered resolution=" +
                std::to_string(m_environment.prefilteredResolution));
            reporter.ResourceDiagnostic(
                "environment prefiltered mips=" +
                std::to_string(m_environment.prefilteredMipLevels));
            reporter.ResourceDiagnostic(
                "environment brdf lut resolution=" +
                std::to_string(m_environment.brdfLUTResolution));
        }
        else if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
        if (m_cookedRasterExecutionObserved)
        {
            reporter.Enable("CookedModelRasterActualExecutionQualified");
        }
    }

    void ModelViewerSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        const SampleRasterScenarioDiagnostics renderEvidence =
            ProjectSampleRasterScenarioDiagnostics(diagnostics);
        const bool executed =
            HasPresentedOpaqueStandardLitRasterEvidence(renderEvidence) &&
            IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics);
        m_cookedRasterExecutionObserved =
            m_cookedRasterExecutionObserved || executed;
        if (executed)
        {
            PublishCookedModelRasterCapability(
                assessment, m_cookedRasterCapabilityPublished);
        }
    }

    SampleReadiness ModelViewerSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        const SampleRasterScenarioDiagnostics renderEvidence =
            ProjectSampleRasterScenarioDiagnostics(diagnostics);
        if (!m_modelActivationError.empty())
        {
            return SampleReadiness::Failed(m_modelActivationError);
        }
        if (!m_environmentFailure.empty())
        {
            return SampleReadiness::Failed(m_environmentFailure);
        }
        if (!m_modelActivated)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for CPU-ready model activation");
        }
        if (!m_model.IsFullyResident() || !m_renderablesEnabled)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for all model GPU resources");
        }
        if (m_textureEnvironment && !m_environment.IsValid())
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for the selected texture IBL");
        }
        if (renderEvidence.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for at least one visible model object");
        }
        if (m_textureEnvironment && !renderEvidence.textureIBLEnabled)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for the selected texture IBL");
        }
        if (m_renderPath == SampleRenderPath::GPUDriven)
        {
            const bool gpuReady =
                renderEvidence.gpuDrivenPolicyDecisionAvailable &&
                renderEvidence.gpuDrivenRequestedMode == "ForceEnabled" &&
                renderEvidence.gpuDrivenPolicyReason == "None" &&
                renderEvidence.gpuDrivenEnabled &&
                renderEvidence.gpuDrivenGraphPassRecorded &&
                renderEvidence.gpuDrivenExecutionRecorded &&
                renderEvidence.gpuDrivenOpaqueIndirectRequested &&
                renderEvidence.gpuDrivenOpaqueIndirectEligible &&
                renderEvidence.gpuDrivenOpaqueIndirectSubmitted &&
                renderEvidence.gpuDrivenOpaqueIndirectBatchCount > 0;
            if (!gpuReady)
            {
                return SampleReadiness::Pending(
                    "Model Viewer is waiting for forced GPU-driven indirect execution");
            }
        }
        else if (m_renderPath == SampleRenderPath::Direct)
        {
            const bool directReady =
                renderEvidence.gpuDrivenPolicyDecisionAvailable &&
                renderEvidence.gpuDrivenRequestedMode == "ForceDisabled" &&
                renderEvidence.gpuDrivenPolicyReason == "ForcedDisabled" &&
                !renderEvidence.gpuDrivenEnabled &&
                !renderEvidence.gpuDrivenOpaqueIndirectRequested &&
                !renderEvidence.gpuDrivenOpaqueIndirectEligible &&
                !renderEvidence.gpuDrivenOpaqueIndirectSubmitted &&
                renderEvidence.gpuDrivenOpaqueIndirectBatchCount == 0 &&
                renderEvidence.gpuDrivenOpaqueDirectDrawCount > 0 &&
                renderEvidence.opaqueExecutionCompleted &&
                renderEvidence.opaqueExecutedDrawCountAvailable &&
                renderEvidence.opaqueExecutedDrawCount > 0;
            if (!directReady)
            {
                return SampleReadiness::Pending(
                    "Model Viewer is waiting for forced Direct execution");
            }
        }
        if (!HasPresentedOpaqueStandardLitRasterEvidence(renderEvidence))
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for a presented Material V2 StandardLit opaque receipt without fallback");
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for its requested render-path execution qualification");
        }
        return SampleReadiness::Ready();
    }

    bool ModelViewerSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void ModelViewerSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelActivationError = "Failed to cancel the Model Viewer ECS model request.";
        }
        if (m_environment.request.IsValid() &&
            !context.environments.Cancel(m_environment))
        {
            m_environmentFailure =
                "Failed to cancel the Model Viewer ECS environment request.";
        }
        m_bounds.Reset();
        m_cameraFrame = {};
        m_renderPath = SampleRenderPath::Auto;
        m_automationFrameCount = 0;
        m_automationZoomEventCount = 0;
        if (!m_model.request.IsValid())
        {
            m_modelActivationError.clear();
        }
        m_modelActivationAttempted = false;
        m_modelActivated = false;
        m_renderablesEnabled = false;
        m_textureEnvironment = false;
        if (!m_environment.request.IsValid())
        {
            m_environmentFailure.clear();
        }
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_cookedRasterExecutionObserved = false;
        m_cookedRasterCapabilityPublished = false;
    }
} // namespace RVX
