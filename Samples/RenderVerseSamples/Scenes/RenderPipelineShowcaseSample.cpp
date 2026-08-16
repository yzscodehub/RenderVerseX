/** @file RenderPipelineShowcaseSample.cpp @brief Engine pass-chain scene. */

#include "Scenes/RenderPipelineShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Samples/SampleRenderPathPolicy.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <sstream>

namespace RVX
{
    namespace
    {
        const SampleInfo RenderPipelineShowcaseInfo{
            "render-pipeline",
            "Render Pipeline",
            "Shows an engine-scheduled shadow, scene, sky, bloom, and tone-mapping chain",
            "shadow-plane-caster",
            SampleAssetPolicy::Fixed,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::Auto,
        };

        Quat MakeLookRotation(const Vec3& direction, const Vec3& up)
        {
            Vec3 forward = length(direction) < 0.001f
                               ? Vec3(0.0f, -1.0f, 0.0f)
                               : normalize(direction);
            Vec3 right = cross(up, forward);
            right = length(right) < 0.001f ? Vec3(1.0f, 0.0f, 0.0f)
                                           : normalize(right);
            const Vec3 correctedUp = cross(forward, right);

            Mat4 rotation(1.0f);
            rotation[0] = Vec4(right, 0.0f);
            rotation[1] = Vec4(correctedUp, 0.0f);
            rotation[2] = Vec4(-forward, 0.0f);
            return Mat4ToQuat(rotation);
        }

        std::string DescribePipelineState(
            const SampleRenderDiagnostics& diagnostics)
        {
            std::ostringstream stream;
            stream << "graphCompiled=" << diagnostics.graphCompiled
                   << ", graphPasses="
                   << diagnostics.renderGraphTotalPasses
                   << ", requestedPostEffects="
                   << diagnostics.requestedPostProcessEffectCount
                   << ", enabledPostEffects="
                   << diagnostics.enabledPostProcessEffectCount
                   << ", unsupportedPostEffects="
                   << diagnostics.unsupportedPostProcessSkippedCount
                   << ", postProcessGraphPasses="
                   << diagnostics.postProcessGraphPassCount
                   << ", directionalShadow="
                   << diagnostics.directionalShadowSamplingEnabled;
            return stream.str();
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

        [[nodiscard]] SceneECS::Light MakeDirectionalShadowLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.96f, 0.88f),
                .intensity = 5.0f,
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
            return "Render-pipeline ECS model request reached a terminal state.";
        }
    } // namespace

    const SampleInfo& RenderPipelineShowcaseSample::GetInfo() const noexcept
    {
        return RenderPipelineShowcaseInfo;
    }

    bool RenderPipelineShowcaseSample::Setup(SampleContext& context,
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
                context.camera, radians(45.0f), aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 1.35f, 4.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 0.35f, 0.0f)))
        {
            outError = "Failed to configure the render-pipeline ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the render-pipeline ECS skybox";
            return false;
        }
        m_skyboxCreated = true;

        const Vec3 lightDirection =
            normalize(Vec3(-0.25f, -0.55f, -0.65f));
        SceneECS::RuntimeEntityDesc lightDesc;
        lightDesc.localTransform.rotation =
            MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                lightDesc,
                MakeDirectionalShadowLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the render-pipeline ECS light";
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

        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = true;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;
        context.renderSettings.postProcess.toneMappingOperator =
            RenderToneMappingOperator::ACES;
        context.renderSettings.postProcess.exposure = 1.0f;
        context.renderSettings.postProcess.gamma = 2.2f;
        context.renderSettings.postProcess.bloomThreshold = 0.65f;
        context.renderSettings.postProcess.bloomIntensity = 0.35f;
        context.renderSettings.postProcess.bloomRadius = 0.60f;
        return context.models.RequestByAssetId(context.options.assetId, m_model, outError);
    }

    void RenderPipelineShowcaseSample::Update(SampleContext& context,
                                              float deltaTime)
    {
        static_cast<void>(deltaTime);
        const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
            context.models.UpdateReadiness(m_model);
        if (status.IsTerminal())
        {
            m_modelFailure = DescribeModelFailure(status);
        }
    }

    void RenderPipelineShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void RenderPipelineShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
        }
        reporter.Enable("EngineRenderPipelineConfiguration");
        reporter.Enable("DirectionalShadowRequested");
        reporter.Enable("BloomRequested");
        reporter.Enable("ACESToneMappingRequested");
        reporter.ResourceDiagnostic(
            "pipeline ownership=Render/RenderGraph/PostProcessStack");
        reporter.ResourceDiagnostic(
            "sample ownership=scene and frame policy configuration");
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_lightCreated)
        {
            reporter.Enable("DirectionalLightScene");
        }
    }

    SampleReadiness RenderPipelineShowcaseSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
            return SampleReadiness::Failed(m_modelFailure);
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Render-pipeline showcase is waiting for the model to become fully resident");
        }
        const bool ready = diagnostics.visibleObjectCount > 0 &&
                           diagnostics.graphCompiled &&
                           diagnostics.renderGraphTotalPasses >= 8 &&
                           diagnostics.directionalShadowSamplingEnabled &&
                           diagnostics.requestedPostProcessEffectCount >= 2 &&
                           diagnostics.enabledPostProcessEffectCount >= 2 &&
                           diagnostics.unsupportedPostProcessSkippedCount == 0 &&
                           diagnostics.postProcessGraphPassCount >= 2;
        if (!ready)
        {
            return SampleReadiness::Pending(
                "Waiting for the engine-owned render pipeline: " +
                DescribePipelineState(diagnostics));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Render-pipeline showcase is waiting for its requested render-path execution");
        }
        return SampleReadiness::Ready();
    }

    bool RenderPipelineShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void RenderPipelineShowcaseSample::Shutdown(SampleContext& context)
    {
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure = "Failed to cancel the render-pipeline ECS model request.";
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
