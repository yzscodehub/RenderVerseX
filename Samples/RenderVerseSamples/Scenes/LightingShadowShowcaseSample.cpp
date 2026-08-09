/** @file LightingShadowShowcaseSample.cpp @brief Directional shadow scene. */

#include "Scenes/LightingShadowShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Scene/Components/CameraComponent.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"

namespace RVX
{
    namespace
    {
        const SampleInfo LightingShadowShowcaseInfo{
            "lighting-shadows",
            "Lighting and Shadows",
            "Shows engine-owned directional lighting and cascaded shadow mapping",
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
    } // namespace

    const SampleInfo& LightingShadowShowcaseSample::GetInfo() const noexcept
    {
        return LightingShadowShowcaseInfo;
    }

    bool LightingShadowShowcaseSample::Setup(SampleContext& context,
                                             std::string& outError)
    {
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(context.options.height);
        context.camera.SetPerspective(
            radians(45.0f), aspect, 0.05f, 100.0f);
        context.camera.SetPosition(Vec3(0.0f, 1.35f, 4.0f));
        context.camera.LookAt(Vec3(0.0f, 0.35f, 0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "LightingShadowSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the lighting-shadow skybox";
            return false;
        }
        skybox->SetSkyboxType(SkyboxType::Procedural);
        skybox->SetSunDirection(normalize(Vec3(-0.25f, -0.55f, -0.65f)));
        skybox->SetSunColor(Vec3(1.0f, 0.96f, 0.88f));
        skybox->SetZenithColor(Vec3(0.10f, 0.20f, 0.42f));
        skybox->SetHorizonColor(Vec3(0.55f, 0.64f, 0.74f));
        skybox->SetGroundColor(Vec3(0.06f, 0.07f, 0.08f));
        skybox->SetScatteringIntensity(0.55f);
        skybox->SetContributesToLighting(false);
        m_skyboxCreated = true;

        ActorSpawnParams lightParams;
        lightParams.name = "LightingShadowSun";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the shadow-casting directional light";
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
        m_shadowLightCreated = true;

        m_shadowAtlasResolution = context.options.quality == "low"
                                      ? 1024u
                                      : context.options.quality == "high"
                                            ? 4096u
                                            : 2048u;
        m_shadowCascadeCount = context.options.quality == "low"
                                   ? 1u
                                   : context.options.quality == "high" ? 4u
                                                                        : 3u;
        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution =
            m_shadowAtlasResolution;
        context.renderSettings.shadows.cascadeCount = m_shadowCascadeCount;
        context.renderSettings.shadows.maxDistance = 50.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;
        context.renderSettings.shadows.filterRadiusTexels = 1.0f;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;
        return context.models.Request(context.options.modelPath,
                                      m_model,
                                      outError);
    }

    void LightingShadowShowcaseSample::Update(SampleContext& context,
                                              float deltaTime)
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
                                       : "Lighting-shadow asset activation was cancelled";
        }
    }

    void LightingShadowShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void LightingShadowShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (m_model.resource.IsLoaded())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("ShadowCasterReceiverFixture");
            reporter.ResourceDiagnostic(
                "model path=" + m_model.sourcePath.string());
        }
        if (m_shadowLightCreated)
        {
            reporter.Enable("DirectionalLightScene");
            reporter.Enable("CascadedDirectionalShadowRequested");
            reporter.ResourceDiagnostic(
                "shadow atlas resolution=" +
                std::to_string(m_shadowAtlasResolution));
            reporter.ResourceDiagnostic(
                "shadow cascade count=" +
                std::to_string(m_shadowCascadeCount));
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
    }

    SampleReadiness LightingShadowShowcaseSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_modelFailure.empty())
            return SampleReadiness::Failed(m_modelFailure);
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for the model to become fully resident");
        }
        if (diagnostics.visibleObjectCount == 0)
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for visible geometry");
        }
        if (diagnostics.renderSceneLightCount == 0)
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for its directional light");
        }
        if (!diagnostics.directionalShadowSamplingEnabled)
        {
            std::string reason =
                "Lighting-shadow showcase is waiting for engine shadow sampling";
            if (!diagnostics.directionalShadowReason.empty())
            {
                reason += ": " + diagnostics.directionalShadowReason;
            }
            return SampleReadiness::Pending(std::move(reason));
        }
        return SampleReadiness::Ready();
    }

    bool LightingShadowShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void LightingShadowShowcaseSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_model = {};
        m_modelFailure.clear();
        m_shadowAtlasResolution = 0;
        m_shadowCascadeCount = 0;
        m_skyboxCreated = false;
        m_shadowLightCreated = false;
    }
} // namespace RVX
