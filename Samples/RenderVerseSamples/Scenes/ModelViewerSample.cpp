/** @file ModelViewerSample.cpp @brief Interactive model inspection scene. */

#include "Scenes/ModelViewerSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/StaticMeshComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>
#include <string>

namespace RVX
{
    namespace
    {
        constexpr float32 ModelViewerVerticalFov = 0.78539816339f;

        uint32 SetModelRenderablesEnabled(Scene& scene,
                                          const LoadedSampleModel& model,
                                          bool enabled)
        {
            uint32 changed = 0;
            for (Actor::Handle actor : model.instance.actors)
            {
                for (StaticMeshComponent* primitive :
                     scene.GetComponentsForActorImplementing<
                         StaticMeshComponent>(actor))
                {
                    if (primitive && primitive->IsEnabled() != enabled)
                    {
                        primitive->SetEnabled(enabled);
                        ++changed;
                    }
                }
            }
            return changed;
        }

        const SampleInfo ModelViewerInfo{
            "model-viewer",
            "Model Viewer",
            "Loads, frames, and interactively inspects a catalog or user model",
            "r7-triangle",
            SampleAssetPolicy::UserModelOrDefault,
            "",
            SampleEnvironmentPolicy::Optional,
        };
    } // namespace

    const SampleInfo& ModelViewerSample::GetInfo() const noexcept
    {
        return ModelViewerInfo;
    }

    bool ModelViewerSample::Setup(SampleContext& context,
                                  std::string& outError)
    {
        if (!context.models.Load(context.options.modelPath,
                                 context.scene,
                                 m_model,
                                 outError))
        {
            return false;
        }

        SceneEntity* modelRoot = m_model.ResolveRoot(context.scene);
        if (!modelRoot)
        {
            outError = "Loaded model root handle is stale";
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
            outError = "Loaded model has no finite renderable bounds for camera framing";
            return false;
        }

        context.camera.SetPerspective(ModelViewerVerticalFov,
                                      aspect,
                                      m_cameraFrame.nearPlane,
                                      m_cameraFrame.farPlane);
        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.target = m_cameraFrame.target;
        orbitSettings.distance = m_cameraFrame.distance;
        orbitSettings.pitch = 0.35877067f;
        orbitSettings.minDistance =
            std::max(m_cameraFrame.distance * 0.01f, 0.001f);
        orbitSettings.maxDistance =
            std::max(m_cameraFrame.distance * 20.0f,
                     orbitSettings.minDistance * 2.0f);
        orbitSettings.zoomSpeed =
            std::max(m_cameraFrame.distance * 0.08f, 0.001f);
        orbitSettings.verticalFovRadians = ModelViewerVerticalFov;
        orbitSettings.aspectRatio = aspect;
        orbitSettings.boundsRadius = glm::length(m_bounds.GetExtent());
        m_orbitCamera.Initialize(orbitSettings, context.input);
        m_orbitCamera.Apply(context.camera);

        // Keep partially uploaded models out of extraction. A real asset can
        // span several bounded upload iterations; publishing its primitives
        // early would turn ordinary GPUUploadPending state into invalid draw
        // packets and could let readiness pass after only one mesh appears.
        static_cast<void>(
            SetModelRenderablesEnabled(context.scene, m_model, false));
        m_renderablesEnabled = false;

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
        m_textureEnvironment = !context.options.environmentPath.empty();
        if (m_textureEnvironment)
        {
            SampleEnvironmentLoadOptions environmentOptions;
            environmentOptions.quality = context.options.quality;
            environmentOptions.smoke = context.options.smoke;
            environmentOptions.exposure = 1.0f;
            if (!context.environments.Load(context.options.environmentPath,
                                           environmentOptions,
                                           m_environment,
                                           outError) ||
                !context.environments.BindToSkybox(*skybox,
                                                   m_environment,
                                                   environmentOptions.exposure,
                                                   outError))
            {
                return false;
            }
        }
        else
        {
            skybox->SetSkyboxType(SkyboxType::Procedural);
            skybox->SetSunDirection(normalize(Vec3(0.35f, 0.65f, 0.45f)));
            skybox->SetSunColor(Vec3(1.0f, 0.94f, 0.84f));
            skybox->SetZenithColor(Vec3(0.10f, 0.24f, 0.52f));
            skybox->SetHorizonColor(Vec3(0.55f, 0.67f, 0.80f));
            skybox->SetGroundColor(Vec3(0.07f, 0.08f, 0.10f));
            skybox->SetScatteringIntensity(0.65f);
            skybox->SetContributesToLighting(false);
        }
        m_skyboxCreated = true;

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
        return true;
    }

    void ModelViewerSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const SceneAssetReadiness readiness =
            context.models.UpdateReadiness(context.scene, m_model);
        if (readiness == SceneAssetReadiness::RenderReady &&
            !m_renderablesEnabled)
        {
            static_cast<void>(
                SetModelRenderablesEnabled(context.scene, m_model, true));
            m_renderablesEnabled = true;
        }
    }

    void ModelViewerSample::OnInput(SampleContext& context)
    {
        if (context.input)
        {
            m_orbitCamera.Update(*context.input, context.camera);
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

    bool ModelViewerSample::IsReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        if (!m_model.instance.IsRenderReady() || !m_renderablesEnabled)
        {
            outPendingReason =
                m_model.instance.readiness == SceneAssetReadiness::Failed &&
                        !m_model.instance.diagnostic.empty()
                    ? m_model.instance.diagnostic
                    : "Model Viewer is waiting for all model GPU resources";
            return false;
        }
        if (diagnostics.visibleObjectCount == 0)
        {
            outPendingReason =
                "Model Viewer is waiting for at least one visible model object";
            return false;
        }
        if (m_textureEnvironment && !diagnostics.textureIBLEnabled)
        {
            outPendingReason =
                "Model Viewer is waiting for the selected texture IBL";
            return false;
        }
        return true;
    }

    bool ModelViewerSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        return IsReady(diagnostics, outError);
    }

    void ModelViewerSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_model = {};
        m_environment = {};
        m_bounds.Reset();
        m_cameraFrame = {};
        m_renderablesEnabled = false;
        m_textureEnvironment = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
