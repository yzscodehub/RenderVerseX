/** @file RenderPipelineShowcaseSample.cpp @brief Engine pass-chain scene. */

#include "Scenes/RenderPipelineShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFramePacket.h"
#include "Scene/Components/CameraComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

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
    } // namespace

    const SampleInfo& RenderPipelineShowcaseSample::GetInfo() const noexcept
    {
        return RenderPipelineShowcaseInfo;
    }

    bool RenderPipelineShowcaseSample::Setup(SampleContext& context,
                                             std::string& outError)
    {
        if (!context.models.Load(context.options.modelPath,
                                 context.scene,
                                 m_model,
                                 outError))
        {
            return false;
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(context.options.height);
        context.camera.SetPerspective(
            radians(45.0f), aspect, 0.05f, 100.0f);
        context.camera.SetPosition(Vec3(0.0f, 1.35f, 4.0f));
        context.camera.LookAt(Vec3(0.0f, 0.35f, 0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "RenderPipelineSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the render-pipeline skybox";
            return false;
        }
        skybox->SetSkyboxType(SkyboxType::Procedural);
        skybox->SetSunDirection(normalize(Vec3(-0.25f, -0.55f, -0.65f)));
        skybox->SetSunColor(Vec3(1.0f, 0.96f, 0.88f));
        skybox->SetZenithColor(Vec3(0.08f, 0.19f, 0.44f));
        skybox->SetHorizonColor(Vec3(0.58f, 0.68f, 0.82f));
        skybox->SetGroundColor(Vec3(0.06f, 0.07f, 0.08f));
        skybox->SetScatteringIntensity(0.60f);
        skybox->SetContributesToLighting(false);
        m_skyboxCreated = true;

        ActorSpawnParams lightParams;
        lightParams.name = "RenderPipelineSun";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the render-pipeline light";
            return false;
        }
        const Vec3 lightDirection =
            normalize(Vec3(-0.25f, -0.55f, -0.65f));
        lightEntity->SetRotation(
            MakeLookRotation(lightDirection, Vec3(0.0f, 1.0f, 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.96f, 0.88f));
        light->SetIntensity(5.0f);
        light->SetCastsShadow(true);
        light->SetShadowBias(0.0008f);
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
        return true;
    }

    void RenderPipelineShowcaseSample::Update(SampleContext& context,
                                              float deltaTime)
    {
        static_cast<void>(context);
        static_cast<void>(deltaTime);
    }

    void RenderPipelineShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void RenderPipelineShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.resource.IsLoaded())
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

    bool RenderPipelineShowcaseSample::IsReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
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
            outPendingReason =
                "Waiting for the engine-owned render pipeline: " +
                DescribePipelineState(diagnostics);
        }
        return ready;
    }

    bool RenderPipelineShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        return IsReady(diagnostics, outError);
    }

    void RenderPipelineShowcaseSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_model = {};
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
