/** @file ModelViewerSample.cpp @brief Interactive model inspection scene. */

#include "Scenes/ModelViewerSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Scene/Components/CameraComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace RVX
{
    namespace
    {
        constexpr float32 ModelViewerVerticalFov = 0.78539816339f;

        void ConfigureProceduralSky(SkyboxComponent& skybox)
        {
            skybox.SetSkyboxType(SkyboxType::Procedural);
            skybox.SetSunDirection(normalize(Vec3(0.35f, 0.65f, 0.45f)));
            skybox.SetSunColor(Vec3(1.0f, 0.94f, 0.84f));
            skybox.SetZenithColor(Vec3(0.10f, 0.24f, 0.52f));
            skybox.SetHorizonColor(Vec3(0.55f, 0.67f, 0.80f));
            skybox.SetGroundColor(Vec3(0.07f, 0.08f, 0.10f));
            skybox.SetScatteringIntensity(0.65f);
            skybox.SetContributesToLighting(false);
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
    } // namespace

    const SampleInfo& ModelViewerSample::GetInfo() const noexcept
    {
        return ModelViewerInfo;
    }

    bool ModelViewerSample::Setup(SampleContext& context,
                                  std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        const float32 initialAspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        context.camera.SetPerspective(
            ModelViewerVerticalFov, initialAspect, 0.05f, 1000.0f);
        context.camera.SetPosition(Vec3(0.0f, 0.0f, 3.0f));
        context.camera.LookAt(Vec3(0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "ModelViewerSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the model-viewer skybox";
            return false;
        }
        // A selected texture environment must never delay the first Present.
        // The coordinator snapshots this procedural state and atomically binds
        // IBL only after all of its GPU dependencies are resident.
        ConfigureProceduralSky(*skybox);
        m_skyboxCreated = true;

        m_textureEnvironment = !context.options.environmentPath.empty();
        if (m_textureEnvironment)
        {
            SampleEnvironmentLoadOptions environmentOptions;
            environmentOptions.quality = context.options.quality;
            environmentOptions.smoke = context.options.smoke;
            environmentOptions.exposure = 1.0f;
            if (!context.environments.Request(context.options.environmentPath,
                                              environmentOptions,
                                              *skybox,
                                              m_environment,
                                              outError))
            {
                return false;
            }
        }

        ActorSpawnParams lightParams;
        lightParams.name = "ModelViewerSun";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the model-viewer light";
            return false;
        }
        lightEntity->SetRotation(
            QuatFromEuler(Vec3(radians(-50.0f), radians(25.0f), 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.95f, 0.86f));
        light->SetIntensity(m_textureEnvironment ? 1.5f : 4.0f);
        light->SetCastsShadow(true);
        light->SetShadowBias(0.001f);
        m_lightCreated = true;

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            context.options.quality == "low" ? 1024u : 2048u;
        context.renderSettings.shadows.cascadeCount =
            context.options.quality == "low" ? 1u : 3u;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
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
        return context.models.Request(context.options.modelPath,
                                      m_model,
                                      outError);
    }

    bool ModelViewerSample::ActivateModel(SampleContext& context)
    {
        m_modelActivationAttempted = true;
        SceneEntity* modelRoot = m_model.ResolveRoot(context.scene);
        if (!modelRoot)
        {
            m_modelActivationError = "Loaded model root handle is stale";
            return false;
        }
        m_bounds = modelRoot->GetWorldBounds();
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
        orbitSettings.verticalFovRadians = ModelViewerVerticalFov;
        orbitSettings.aspectRatio = aspect;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            m_modelActivationError =
                "Model Viewer could not initialize its orbit camera";
            return false;
        }
        m_orbitCamera.Apply(context.camera);
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
        static_cast<void>(context.models.UpdateReadiness(m_model));
        if (m_textureEnvironment)
        {
            static_cast<void>(
                context.environments.UpdateReadiness(m_environment));
        }

        if (!m_modelActivationAttempted && m_model.IsCPUReady())
        {
            if (!ActivateModel(context))
                static_cast<void>(context.models.Cancel(m_model));
        }
        if (m_model.status.IsFullyResident() &&
            m_model.instance.IsRenderReady())
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
            m_orbitCamera.ApplyInput(input, context.camera);
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
        m_orbitCamera.Apply(context.camera);
        m_qualificationCapturePrepared = true;
        return true;
    }

    void ModelViewerSample::OnInput(SampleContext& context)
    {
        if (m_modelActivated && m_orbitCamera.IsInitialized() && context.input)
        {
            m_orbitCamera.Update(*context.input, context.camera);
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
        m_orbitCamera.SetAspectRatio(
            static_cast<float>(width) / static_cast<float>(height),
            context.camera);
        if (!m_orbitCamera.IsInitialized())
        {
            context.camera.SetAspectRatio(
                static_cast<float>(width) / static_cast<float>(height));
        }
    }

    void ModelViewerSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.resource.IsLoaded())
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
            if (m_model.instance.IsRenderReady() && m_renderablesEnabled)
            {
                reporter.Enable("ModelRenderReady");
            }
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(m_model.resource->GetMeshCount()));
            reporter.ResourceDiagnostic(
                "model materials=" +
                std::to_string(m_model.resource->GetMaterialCount()));
            reporter.ResourceDiagnostic(
                "model nodes=" +
                std::to_string(m_model.resource->GetNodeCount()));
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
    }

    SampleReadiness ModelViewerSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelActivationError.empty())
        {
            return SampleReadiness::Failed(m_modelActivationError);
        }
        if (m_model.status.IsFailed() ||
            m_model.status.lifecycle == SceneAssetLifecycle::Cancelled)
        {
            return SampleReadiness::Failed(
                m_model.status.diagnostic.empty()
                    ? "Model Viewer model request failed or was cancelled"
                    : m_model.status.diagnostic);
        }
        if (m_textureEnvironment &&
            (m_environment.status.IsFailed() ||
             m_environment.status.lifecycle == SceneAssetLifecycle::Cancelled))
        {
            return SampleReadiness::Failed(
                m_environment.status.diagnostic.empty()
                    ? "Model Viewer environment request failed or was cancelled"
                    : m_environment.status.diagnostic);
        }
        if (!m_modelActivated)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for CPU-ready model activation");
        }
        if (!m_model.status.IsFullyResident() ||
            !m_model.instance.IsRenderReady() || !m_renderablesEnabled)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for all model GPU resources");
        }
        if (m_textureEnvironment &&
            (!m_environment.status.IsFullyResident() ||
             !m_environment.IsValid()))
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for the selected texture IBL");
        }
        if (diagnostics.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for at least one visible model object");
        }
        if (m_textureEnvironment && !diagnostics.textureIBLEnabled)
        {
            return SampleReadiness::Pending(
                "Model Viewer is waiting for the selected texture IBL");
        }
        if (m_renderPath == SampleRenderPath::GPUDriven)
        {
            const bool gpuReady =
                diagnostics.gpuDrivenPolicyDecisionAvailable &&
                diagnostics.gpuDrivenRequestedMode == "ForceEnabled" &&
                diagnostics.gpuDrivenPolicyReason == "None" &&
                diagnostics.gpuDrivenEnabled &&
                diagnostics.gpuDrivenGraphPassRecorded &&
                diagnostics.gpuDrivenExecutionRecorded &&
                diagnostics.gpuDrivenOpaqueIndirectRequested &&
                diagnostics.gpuDrivenOpaqueIndirectEligible &&
                diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0;
            if (!gpuReady)
            {
                return SampleReadiness::Pending(
                    "Model Viewer is waiting for forced GPU-driven indirect execution");
            }
        }
        else if (m_renderPath == SampleRenderPath::Direct)
        {
            const bool directReady =
                diagnostics.gpuDrivenPolicyDecisionAvailable &&
                diagnostics.gpuDrivenRequestedMode == "ForceDisabled" &&
                diagnostics.gpuDrivenPolicyReason == "ForcedDisabled" &&
                !diagnostics.gpuDrivenEnabled &&
                !diagnostics.gpuDrivenOpaqueIndirectRequested &&
                !diagnostics.gpuDrivenOpaqueIndirectEligible &&
                !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
                diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
                diagnostics.opaqueExecutionCompleted &&
                diagnostics.opaqueExecutedDrawCountAvailable &&
                diagnostics.opaqueExecutedDrawCount > 0;
            if (!directReady)
            {
                return SampleReadiness::Pending(
                    "Model Viewer is waiting for forced Direct execution");
            }
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
        static_cast<void>(context);
        m_model = {};
        m_environment = {};
        m_bounds.Reset();
        m_cameraFrame = {};
        m_renderPath = SampleRenderPath::Auto;
        m_automationFrameCount = 0;
        m_automationZoomEventCount = 0;
        m_modelActivationError.clear();
        m_modelActivationAttempted = false;
        m_modelActivated = false;
        m_renderablesEnabled = false;
        m_textureEnvironment = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
