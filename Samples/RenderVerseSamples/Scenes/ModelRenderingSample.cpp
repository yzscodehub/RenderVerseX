/** @file ModelRenderingSample.cpp @brief Model rendering pilot scene. */

#include "Scenes/ModelRenderingSample.h"

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

namespace RVX
{
    namespace
    {
        const SampleInfo ModelRenderingInfo{
            "model-rendering",
            "Model Rendering",
            "Loads a model through the production resource and scene pipeline",
            "shadow-plane-caster",
            SampleAssetPolicy::CompatibleOverride,
        };
    } // namespace

    const SampleInfo& ModelRenderingSample::GetInfo() const noexcept
    {
        return ModelRenderingInfo;
    }

    bool ModelRenderingSample::Setup(SampleContext& context,
                                     std::string& outError)
    {
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        context.camera.SetPerspective(radians(45.0f), aspect, 0.05f, 250.0f);
        context.camera.SetPosition(Vec3(0.0f, 2.5f, 5.8f));
        context.camera.LookAt(Vec3(0.0f, 0.45f, 0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "ModelRenderingSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the model-rendering skybox";
            return false;
        }
        skybox->SetSkyboxType(SkyboxType::Procedural);
        skybox->SetSunDirection(normalize(Vec3(0.35f, 0.65f, 0.45f)));
        skybox->SetSunColor(Vec3(1.0f, 0.94f, 0.84f));
        skybox->SetZenithColor(Vec3(0.10f, 0.24f, 0.52f));
        skybox->SetHorizonColor(Vec3(0.55f, 0.67f, 0.80f));
        skybox->SetGroundColor(Vec3(0.07f, 0.08f, 0.10f));
        skybox->SetScatteringIntensity(0.65f);
        skybox->SetContributesToLighting(false);
        m_skyboxCreated = true;

        ActorSpawnParams lightParams;
        lightParams.name = "ModelRenderingSun";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the model-rendering light";
            return false;
        }
        lightEntity->SetRotation(
            QuatFromEuler(Vec3(radians(-50.0f), radians(25.0f), 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.95f, 0.86f));
        light->SetIntensity(4.0f);
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
        return context.models.Request(context.options.modelPath,
                                      m_model,
                                      outError);
    }

    void ModelRenderingSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const SceneAssetStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsFailed() ||
            status.lifecycle == SceneAssetLifecycle::Cancelled)
        {
            m_modelFailure = !status.diagnostic.empty()
                                 ? status.diagnostic
                                 : !status.error.message.empty()
                                       ? status.error.message
                                       : "Model rendering asset activation was cancelled";
        }
    }

    void ModelRenderingSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void ModelRenderingSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.resource.IsLoaded())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
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
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
    }

    SampleReadiness ModelRenderingSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
            return SampleReadiness::Failed(m_modelFailure);
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Model-rendering sample is waiting for the model to become fully resident");
        }
        if (diagnostics.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "Model-rendering sample is waiting for visible geometry");
        }
        return SampleReadiness::Ready();
    }

    bool ModelRenderingSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void ModelRenderingSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_model = {};
        m_modelFailure.clear();
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
