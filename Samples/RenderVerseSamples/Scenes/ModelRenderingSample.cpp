/** @file ModelRenderingSample.cpp @brief Model rendering pilot scene. */

#include "Scenes/ModelRenderingSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

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
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

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

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.95f, 0.86f),
                .intensity = 4.0f,
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
            return "Model-rendering ECS model request reached a terminal state.";
        }
    } // namespace

    const SampleInfo& ModelRenderingSample::GetInfo() const noexcept
    {
        return ModelRenderingInfo;
    }

    bool ModelRenderingSample::Setup(SampleContext& context,
                                     std::string& outError)
    {
        m_renderPath = context.options.renderPath;
        if (!ApplySampleRenderPathPolicy(
                m_renderPath, context.renderSettings.gpuCulling, outError))
        {
            return false;
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, radians(45.0f), aspect, 0.05f, 250.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 2.5f, 5.8f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.45f, 0.0f)))
        {
            outError = "Failed to configure the model-rendering ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the model-rendering ECS skybox";
            return false;
        }
        m_skyboxCreated = true;

        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            QuatFromEuler(Vec3(radians(-50.0f), radians(25.0f), 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the model-rendering ECS light";
            return false;
        }
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
        return context.models.RequestByAssetId(context.options.assetId, m_model, outError);
    }

    void ModelRenderingSample::Update(SampleContext& context, float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
        }
    }

    void ModelRenderingSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void ModelRenderingSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(m_model.status.modelMetadata.meshAssetIds.size()));
            reporter.ResourceDiagnostic(
                "model materials=" +
                std::to_string(m_model.status.modelMetadata.materialAssetIds.size()));
            reporter.ResourceDiagnostic(
                "model nodes=" +
                std::to_string(m_model.status.modelMetadata.sourceNodeCount));
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
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Model-rendering sample is waiting for its requested render-path execution");
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
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure = "Failed to cancel the model-rendering ECS model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_renderPath = SampleRenderPath::Auto;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
