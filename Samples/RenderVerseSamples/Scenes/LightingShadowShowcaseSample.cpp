/** @file LightingShadowShowcaseSample.cpp @brief Directional and local shadow scene. */

#include "Scenes/LightingShadowShowcaseSample.h"

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
        const SampleInfo LightingShadowShowcaseInfo = []
        {
            SampleInfo info;
            info.id = "lighting-shadows";
            info.displayName = "Lighting and Shadows";
            info.description =
                "Shows engine-owned cascaded, point, and spot shadow rendering";
            info.defaultAssetId = "particle-vfx-ground-caster-v1";
            info.assetPolicy = SampleAssetPolicy::Fixed;
            info.environmentPolicy = SampleEnvironmentPolicy::None;
            info.supportsRenderPathSelection = true;
            info.defaultRenderPath = SampleRenderPath::Auto;
            // Runtime-only composition is admitted from the SampleInfo
            // contract, not inferred from the manifest's staged path. Keep
            // the selected primary model bound to its verified CookManifest.
            info.requiredCookedAssetIds = {
                "particle-vfx-ground-caster-v1"};
            return info;
        }();

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

        [[nodiscard]] SceneECS::Skybox MakeProceduralSky()
        {
            return {
                .mode = SceneECS::SkyboxMode::Procedural,
                .sunDirection = normalize(Vec3(-0.25f, -0.55f, -0.65f)),
                .sunColor = Vec3(1.0f, 0.96f, 0.88f),
                .zenithColor = Vec3(0.10f, 0.20f, 0.42f),
                .horizonColor = Vec3(0.55f, 0.64f, 0.74f),
                .groundColor = Vec3(0.06f, 0.07f, 0.08f),
                .scatteringIntensity = 0.55f,
                .contributesToLighting = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalShadowLight()
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

        [[nodiscard]] SceneECS::Light MakePointShadowLight()
        {
            return {
                .type = SceneECS::LightType::Point,
                .color = Vec3(1.0f, 0.52f, 0.28f),
                .intensity = 36.0f,
                .intensityUnit = SceneECS::LightIntensityUnit::Candela,
                .range = 5.5f,
                .shadowBias = 0.001f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] SceneECS::Light MakeSpotShadowLight()
        {
            return {
                .type = SceneECS::LightType::Spot,
                .color = Vec3(0.32f, 0.58f, 1.0f),
                .intensity = 64.0f,
                .intensityUnit = SceneECS::LightIntensityUnit::Candela,
                .range = 8.0f,
                .innerConeRadians = radians(18.0f),
                .outerConeRadians = radians(32.0f),
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
            return "Lighting-shadow ECS model request reached a terminal state.";
        }
    } // namespace

    const SampleInfo& LightingShadowShowcaseSample::GetInfo() const noexcept
    {
        return LightingShadowShowcaseInfo;
    }

    bool LightingShadowShowcaseSample::Setup(SampleContext& context,
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
            outError = "Failed to configure the lighting-shadow ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the lighting-shadow ECS skybox";
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
            outError = "Failed to create the shadow-casting ECS directional light";
            return false;
        }
        m_shadowLightCreated = true;

        SceneECS::RuntimeEntityDesc pointLightDesc;
        pointLightDesc.localTransform.translation = Vec3(-1.35f, 1.45f, 1.0f);
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                pointLightDesc,
                MakePointShadowLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the shadow-casting ECS point light";
            return false;
        }
        m_pointShadowLightCreated = true;

        constexpr Vec3 SpotPosition(1.55f, 2.35f, 1.65f);
        constexpr Vec3 SpotTarget(0.0f, 0.35f, 0.0f);
        SceneECS::RuntimeEntityDesc spotLightDesc;
        spotLightDesc.localTransform.translation = SpotPosition;
        spotLightDesc.localTransform.rotation = MakeLookRotation(
            SpotTarget - SpotPosition, Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                spotLightDesc,
                MakeSpotShadowLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the shadow-casting ECS spot light";
            return false;
        }
        m_spotShadowLightCreated = true;

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
        context.renderSettings.postProcess.antiAliasingMode =
            RenderAntiAliasingMode::FXAA;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSR = false;
        return context.models.RequestByAssetId(context.options.assetId, m_model, outError);
    }

    void LightingShadowShowcaseSample::Update(SampleContext& context,
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

    void LightingShadowShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    void LightingShadowShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        reporter.SetScenarioContractRevision(1);
        reporter.SetScenarioPhase(
            m_executionQualified ? "stable" :
            m_modelFailure.empty() ? "pending" : "failed");
        AppendSampleRenderPathPolicyReport(m_renderPath, reporter);
        if (m_model.status.modelMetadata.HasPublishedSource())
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
        if (m_pointShadowLightCreated)
        {
            reporter.Enable("PointShadowRequested");
        }
        if (m_spotShadowLightCreated)
        {
            reporter.Enable("SpotShadowRequested");
        }
        if (m_pointShadowLightCreated && m_spotShadowLightCreated)
        {
            reporter.Enable("LocalShadowAtlasExecutionRequired");
            reporter.ResourceDiagnostic("local shadow requests=2");
        }
        if (m_skyboxCreated)
        {
            reporter.Enable("ProceduralSkyScene");
        }
        if (m_executionQualified)
        {
            reporter.Enable("LightingShadowActualExecutionQualified");
        }
    }

    void LightingShadowShowcaseSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        static_cast<void>(assessment);
        m_executionQualified = m_executionQualified ||
            GetReadiness(diagnostics).IsReady();
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
        if (diagnostics.renderSceneLightCount < 3)
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for its directional, point, and spot lights");
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
        if (diagnostics.pointShadowRequestedCount != 1 ||
            diagnostics.spotShadowRequestedCount != 1 ||
            diagnostics.localShadowRequestCount != 2)
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for the exact point/spot local-shadow request set");
        }
        if (!diagnostics.localShadowAtlasReady ||
            !diagnostics.pointShadowSupported ||
            !diagnostics.spotShadowSupported ||
            !diagnostics.localShadowFallbackReason.empty())
        {
            std::string reason =
                "Lighting-shadow showcase is waiting for exact local-shadow atlas execution";
            if (!diagnostics.localShadowFallbackReason.empty())
            {
                reason += ": " + diagnostics.localShadowFallbackReason;
            }
            return SampleReadiness::Pending(std::move(reason));
        }
        if (!IsSampleRenderPathExecutionQualified(m_renderPath, diagnostics))
        {
            return SampleReadiness::Pending(
                "Lighting-shadow showcase is waiting for its requested render-path execution");
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
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_modelFailure = "Failed to cancel the lighting-shadow ECS model request.";
        }
        if (!m_model.request.IsValid())
        {
            m_modelFailure.clear();
        }
        m_shadowAtlasResolution = 0;
        m_shadowCascadeCount = 0;
        m_renderPath = SampleRenderPath::Auto;
        m_skyboxCreated = false;
        m_shadowLightCreated = false;
        m_pointShadowLightCreated = false;
        m_spotShadowLightCreated = false;
        m_executionQualified = false;
    }
} // namespace RVX
