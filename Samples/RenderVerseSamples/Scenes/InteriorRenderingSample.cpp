/**
 * @file InteriorRenderingSample.cpp
 * @brief Backend-neutral P0-A interior rendering product qualification scene.
 */

#include "Scenes/InteriorRenderingSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "ResourceSceneAdapters/ECS/PreparedModelBatch.h"
#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr uint32 InteriorDirectionalCascadeCount = 3;
        constexpr uint32 InteriorShadowMapSize = 2048;
        constexpr uint32 InteriorPointLightCount = 8;
        constexpr uint32 InteriorSpotLightCount = 4;
        constexpr uint32 InteriorTransparentDrawCount = 2;
        constexpr uint32 InteriorRequiredStableTransparentFrames = 2;
        constexpr uint32 InteriorMissingDiagnosticsFrameAllowance = 2;
        constexpr float32 InteriorCorridorExtent = 25.0f;
        constexpr float32 InteriorVerticalFov = radians(75.0f);
        constexpr float32 InteriorInitialOrbitDistance = 8.05f;
        constexpr float32 InteriorMinimumOrbitDistance = 3.75f;
        constexpr float32 InteriorOrbitDistanceTolerance = 0.001f;
        constexpr float32 InteriorPlacementGroundTolerance = 0.025f;
        constexpr float32 InteriorPlacementExtentTolerance = 0.04f;

        const SampleInfo InteriorRenderingInfo{
            "interior-rendering",
            "Interior Rendering",
            "Qualifies production Sponza directional shadows, local lighting, transparent glass, IBL, and clustered lighting",
            "crytek-sponza",
            SampleAssetPolicy::Fixed,
            "kloofendal-48d-partly-cloudy-pure-sky",
            SampleEnvironmentPolicy::Required,
            true,
            SampleRenderPath::Auto,
            {"interior-rendering-p0a"}};

        const AssessmentAction InteriorRenderPathAction{
            AssessmentCode("RENDER.ACTION.INTERIOR_RENDER_PATH_SELECTED"),
            "The sample selected its Direct, GPU-driven, or Auto policy only through frame settings."};
        const AssessmentAction InteriorQualificationAction{
            AssessmentCode("RENDER.ACTION.INTERIOR_PRODUCT_FRAME_QUALIFIED"),
            "One fully resident interior frame satisfied the directional-shadow, local-lighting, transparency, IBL, and material contracts."};

        const AssessmentInvariant DirectionalCSMInvariant{
            AssessmentCode("RENDER.INTERIOR.DIRECTIONAL_CSM_EXACT"),
            "The completed frame requested, produced, and resolved three directional shadow cascades at 2048 pixels with casters, draws, and sampling."};
        const AssessmentInvariant LocalLightingInvariant{
            AssessmentCode("RENDER.INTERIOR.LOCAL_LIGHT_ADMISSION_EXACT"),
            "The completed frame admitted all eight point lights and four spot lights without overflow."};
        const AssessmentInvariant TransparentInvariant{
            AssessmentCode("RENDER.INTERIOR.TRANSPARENT_ORDER_STABLE"),
            "At least two consecutive completed frames executed two or more finite, ordered transparent packets with one stable order hash."};
        const AssessmentInvariant LightingInvariant{
            AssessmentCode("RENDER.INTERIOR.IBL_CLUSTERED_LIGHTING_ACTIVE"),
            "Texture IBL and clustered lighting were active in the completed frame."};
        const AssessmentInvariant MaterialInvariant{
            AssessmentCode("RENDER.INTERIOR.MATERIAL_FALLBACK_ABSENT"),
            "Opaque and transparent material binding diagnostics reported no fallback binding or fallback texture flags."};
        const AssessmentInvariant RenderSceneRevisionInvariant{
            AssessmentCode("RENDER.INTERIOR.PRESENTED_SCENE_REVISION_CONVERGED"),
            "The qualified completed frame covered the required Scene revision at the Engine-to-Render boundary."};
        const AssessmentInvariant RenderPathInvariant{
            AssessmentCode("RENDER.INTERIOR.RENDER_PATH_EXECUTION_EXACT"),
            "The qualified completed frame executed the explicitly requested Direct or GPU-driven render path without fallback."};

        const AssessmentMetric ResolvedCascadeMetric{
            AssessmentCode("RENDER.METRIC.INTERIOR_RESOLVED_CASCADES"),
            "count",
            "Directional shadow cascades resolved by the qualified frame."};
        const AssessmentMetric TransparentOrderHashMetric{
            AssessmentCode("RENDER.METRIC.INTERIOR_TRANSPARENT_ORDER_HASH"),
            "hash",
            "Stable transparent ordering hash observed across consecutive completed frames."};
        const AssessmentMetric PointLightsAdmittedMetric{
            AssessmentCode("RENDER.METRIC.INTERIOR_POINT_LIGHTS_ADMITTED"),
            "count",
            "Point lights admitted by the qualified frame."};
        const AssessmentMetric SpotLightsAdmittedMetric{
            AssessmentCode("RENDER.METRIC.INTERIOR_SPOT_LIGHTS_ADMITTED"),
            "count",
            "Spot lights admitted by the qualified frame."};
        const AssessmentMetric ActiveClustersMetric{
            AssessmentCode("RENDER.METRIC.INTERIOR_ACTIVE_CLUSTERS"),
            "count",
            "Active clustered-lighting clusters observed by the qualified frame."};

        const AssessmentCapability DirectionalCSMCapability{
            AssessmentCode("RENDER.CAPABILITY.DIRECTIONAL_CSM"),
            "Directional cascaded-shadow capability was observed in a completed interior frame.",
            true,
            "Interior rendering requires directional CSM support."};
        const AssessmentCapability TransparentPassCapability{
            AssessmentCode("RENDER.CAPABILITY.TRANSPARENT_PASS"),
            "Transparent-pass execution diagnostics were observed in a completed interior frame.",
            true,
            "Interior rendering requires transparent-pass diagnostics."};
        const AssessmentCapability PointShadowCapability{
            AssessmentCode("RENDER.CAPABILITY.POINT_SHADOW"),
            "Point shadows are an explicitly unsupported P0 interior-rendering scope boundary.",
            false,
            "P0 interior-rendering does not request local point shadows."};
        const AssessmentCapability SpotShadowCapability{
            AssessmentCode("RENDER.CAPABILITY.SPOT_SHADOW"),
            "Spot shadows are an explicitly unsupported P0 interior-rendering scope boundary.",
            false,
            "P0 interior-rendering does not request local spot shadows."};
        const AssessmentCapability HZBCapability{
            AssessmentCode("RENDER.CAPABILITY.HZB_OCCLUSION"),
            "HZB occlusion is an explicitly unsupported P0 interior-rendering scope boundary.",
            false,
            "P0 interior-rendering keeps its occlusion contract to real geometry and depth ordering."};

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

        struct InteriorPointLightDesc
        {
            Vec3 position;
            Vec3 color;
            float32 intensity = 0.0f;
        };

        struct InteriorSpotLightDesc
        {
            Vec3 position;
            Vec3 target;
            Vec3 color;
            float32 intensity = 0.0f;
        };

        const std::array<InteriorPointLightDesc, InteriorPointLightCount>
            InteriorPointLights{{
                {Vec3(-3.2f, 2.6f, -1.4f), Vec3(1.0f, 0.32f, 0.20f), 18.0f},
                {Vec3(3.0f, 2.8f, -2.1f), Vec3(0.24f, 0.52f, 1.0f), 16.0f},
                {Vec3(-1.8f, 1.4f, -5.8f), Vec3(1.0f, 0.74f, 0.30f), 12.0f},
                {Vec3(1.9f, 1.7f, -6.5f), Vec3(0.38f, 1.0f, 0.65f), 12.0f},
                {Vec3(-4.2f, 1.1f, -7.0f), Vec3(0.55f, 0.28f, 1.0f), 10.0f},
                {Vec3(4.0f, 1.2f, -7.5f), Vec3(1.0f, 0.42f, 0.72f), 10.0f},
                {Vec3(-0.6f, 3.0f, -8.8f), Vec3(0.30f, 0.84f, 1.0f), 14.0f},
                {Vec3(0.9f, 0.8f, -3.8f), Vec3(1.0f, 0.88f, 0.48f), 9.0f}}};

        const std::array<InteriorSpotLightDesc, InteriorSpotLightCount>
            InteriorSpotLights{{
                {Vec3(-3.6f, 3.8f, 2.2f), Vec3(-1.7f, 1.1f, -4.8f), Vec3(1.0f, 0.62f, 0.34f), 24.0f},
                {Vec3(3.4f, 3.6f, 1.5f), Vec3(1.5f, 0.9f, -5.2f), Vec3(0.30f, 0.58f, 1.0f), 22.0f},
                {Vec3(-2.8f, 3.4f, -6.8f), Vec3(-0.8f, 0.7f, -8.8f), Vec3(0.70f, 0.30f, 1.0f), 18.0f},
                {Vec3(2.7f, 3.5f, -7.3f), Vec3(0.8f, 0.8f, -9.2f), Vec3(0.32f, 1.0f, 0.66f), 18.0f}}};

        template<typename StatusType>
        std::string DescribeStatusFailure(const StatusType& status,
                                          std::string_view fallback)
        {
            if (!status.diagnostic.empty())
                return status.diagnostic;
            if (!status.request.error.message.empty())
                return status.request.error.message;
            return std::string(fallback);
        }

        [[nodiscard]] SceneECS::Light MakeDirectionalLight()
        {
            return {
                .type = SceneECS::LightType::Directional,
                .color = Vec3(1.0f, 0.92f, 0.78f),
                .intensity = 3.5f,
                .shadowBias = 0.0008f,
                .castsShadows = true,
            };
        }

        [[nodiscard]] SceneECS::Light MakePointLight(
            const InteriorPointLightDesc& descriptor)
        {
            return {
                .type = SceneECS::LightType::Point,
                .color = descriptor.color,
                .intensity = descriptor.intensity,
                .range = 5.5f,
                .castsShadows = false,
            };
        }

        [[nodiscard]] SceneECS::Light MakeSpotLight(
            const InteriorSpotLightDesc& descriptor)
        {
            return {
                .type = SceneECS::LightType::Spot,
                .color = descriptor.color,
                .intensity = descriptor.intensity,
                .range = 12.0f,
                .innerConeRadians = radians(18.0f),
                .outerConeRadians = radians(32.0f),
                .castsShadows = false,
            };
        }
    } // namespace

    const SampleInfo& InteriorRenderingSample::GetInfo() const noexcept
    {
        return InteriorRenderingInfo;
    }

    SampleAssessmentContract InteriorRenderingSample::GetAssessmentContract() const
    {
        SampleAssessmentContract contract;
        contract.code = AssessmentCode("SAMPLE.INTERIOR_RENDERING");
        contract.revision = "2";
        contract.checkpoints = {
            AssessmentCheckpoints::EngineBaseline,
            AssessmentCheckpoints::ScenarioSetup,
            AssessmentCheckpoints::ActionRequested,
            AssessmentCheckpoints::ActionApplied,
            AssessmentCheckpoints::ScenarioStable,
            AssessmentCheckpoints::TeardownBefore,
            AssessmentCheckpoints::TeardownSceneComplete,
            AssessmentCheckpoints::TeardownRenderDrained,
            AssessmentCheckpoints::EngineShutdownComplete};
        contract.actions = {
            InteriorRenderPathAction,
            InteriorQualificationAction};
        contract.invariants = {
            DirectionalCSMInvariant,
            LocalLightingInvariant,
            TransparentInvariant,
            LightingInvariant,
            MaterialInvariant,
            RenderSceneRevisionInvariant,
            RenderPathInvariant};
        contract.metrics = {
            ResolvedCascadeMetric,
            TransparentOrderHashMetric,
            PointLightsAdmittedMetric,
            SpotLightsAdmittedMetric,
            ActiveClustersMetric};
        contract.capabilities = {
            DirectionalCSMCapability,
            TransparentPassCapability,
            PointShadowCapability,
            SpotShadowCapability,
            HZBCapability};
        return contract;
    }

    bool InteriorRenderingSample::Setup(SampleContext& context,
                                        std::string& outError)
    {
        m_model = {};
        m_glassPanelTemplate = {};
        m_environment = {};
        m_interiorBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_renderPath = context.options.renderPath;
        m_failure.clear();
        ResetDiagnosticState();

        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                context.renderSettings.gpuCulling.mode = RenderGPUDrivenMode::Auto;
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
                outError = "Interior rendering received an invalid render-path policy";
                return false;
        }
        const float32 aspect = static_cast<float32>(context.options.width) /
                               static_cast<float32>(
                                   std::max(context.options.height, 1u));
        if (!context.cameras.SetPerspective(
                context.camera, InteriorVerticalFov, aspect, 0.05f, 100.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 2.4f, 6.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f, 2.3f, -4.0f)))
        {
            outError = "Failed to configure the interior rendering ECS camera";
            return false;
        }

        context.renderSettings.shadows.enabled = true;
        context.renderSettings.shadows.atlasResolution = InteriorShadowMapSize;
        context.renderSettings.shadows.cascadeCount =
            InteriorDirectionalCascadeCount;
        context.renderSettings.shadows.maxDistance = 60.0f;
        context.renderSettings.shadows.shadowBias = 0.0008f;
        context.renderSettings.shadows.normalBias = 0.02f;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.exposureMode =
            RenderExposureMode::ManualMultiplier;
        context.renderSettings.postProcess.exposure = 1.0f;
        // HZB is intentionally outside the P0 interior contract. Complex
        // occlusion here is produced by Sponza geometry, depth testing, and
        // opaque/transparent ordering rather than an unavailable HZB path.
        context.renderSettings.gpuCulling.enableOcclusionCulling = false;

        const Vec3 directionalDirection =
            normalize(Vec3(-0.30f, -0.65f, -0.45f));
        SceneECS::RuntimeEntityDesc directionalDesc;
        directionalDesc.localTransform.rotation =
            MakeLookRotation(directionalDirection, Vec3(0.0f, 1.0f, 0.0f));
        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                directionalDesc,
                MakeDirectionalLight(),
                SceneECS::Visibility{}).IsValid())
        {
            outError = "Failed to create the interior ECS directional light";
            return false;
        }
        m_directionalLightCreated = true;

        for (const InteriorPointLightDesc& descriptor : InteriorPointLights)
        {
            SceneECS::RuntimeEntityDesc pointDesc;
            pointDesc.localTransform.translation = descriptor.position;
            if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                    pointDesc,
                    MakePointLight(descriptor),
                    SceneECS::Visibility{}).IsValid())
            {
                outError = "Failed to create an interior ECS point light";
                return false;
            }
            ++m_pointLightCount;
        }

        for (const InteriorSpotLightDesc& descriptor : InteriorSpotLights)
        {
            SceneECS::RuntimeEntityDesc spotDesc;
            spotDesc.localTransform.translation = descriptor.position;
            spotDesc.localTransform.rotation = MakeLookRotation(
                descriptor.target - descriptor.position,
                Vec3(0.0f, 1.0f, 0.0f));
            if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                    spotDesc,
                    MakeSpotLight(descriptor),
                    SceneECS::Visibility{}).IsValid())
            {
                outError = "Failed to create an interior ECS spot light";
                return false;
            }
            ++m_spotLightCount;
        }

        SampleEnvironmentLoadOptions environmentOptions;
        environmentOptions.quality = context.options.quality;
        environmentOptions.smoke = context.options.smoke;
        environmentOptions.exposure = 1.0f;
        const bool environmentRequested =
            !context.options.environmentAssetId.empty()
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
        if (!environmentRequested)
        {
            return false;
        }
        if (!context.models.RequestByAssetId(
                context.options.assetId, m_model, outError))
        {
            static_cast<void>(context.environments.Cancel(m_environment));
            return false;
        }
        if (std::find(context.options.modelAssetIds.begin(),
                      context.options.modelAssetIds.end(),
                      "interior-rendering-p0a") ==
            context.options.modelAssetIds.end())
        {
            static_cast<void>(context.models.Cancel(m_model));
            static_cast<void>(context.environments.Cancel(m_environment));
            outError =
                "Interior rendering requires its catalog-backed transparent glass-panel asset";
            return false;
        }
        if (!context.models.RequestByAssetId("interior-rendering-p0a",
                                             m_glassPanelTemplate,
                                             outError))
        {
            static_cast<void>(context.models.Cancel(m_model));
            static_cast<void>(context.environments.Cancel(m_environment));
            return false;
        }
        return true;
    }

    void InteriorRenderingSample::Update(SampleContext& context,
                                         float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (m_model.request.IsValid())
        {
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus modelStatus =
                context.models.UpdateReadiness(m_model);
            if (m_failure.empty() && modelStatus.IsTerminal())
            {
                m_failure = DescribeStatusFailure(
                    modelStatus,
                    "Interior rendering model request reached a terminal state");
            }
        }
        if (m_glassPanelTemplate.request.IsValid())
        {
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus glassPanelStatus =
                context.models.UpdateReadiness(m_glassPanelTemplate);
            if (m_failure.empty() && glassPanelStatus.IsTerminal())
            {
                m_failure = DescribeStatusFailure(
                    glassPanelStatus,
                    "Interior rendering transparent glass-panel request reached a terminal state");
            }
        }
        if (m_environment.request.IsValid())
        {
            const ResourceSceneAdapters::EcsEnvironmentLoadStatus environmentStatus =
                context.environments.UpdateReadiness(m_environment);
            if (m_failure.empty() && environmentStatus.IsTerminal())
            {
                m_failure = DescribeStatusFailure(
                    environmentStatus,
                    "Interior rendering environment request reached a terminal state");
            }
        }
        if (m_failure.empty() &&
            !m_interiorPresentationPrepared && m_model.IsCPUReady())
        {
            std::string presentationError;
            if (!PrepareInteriorPresentation(context, presentationError))
            {
                m_failure = std::move(presentationError);
            }
        }
        if (m_failure.empty() && !m_productionAssetCompositionCaptured &&
            m_model.IsFullyResident())
        {
            std::string compositionError;
            if (!CaptureProductionAssetComposition(compositionError))
            {
                m_failure = std::move(compositionError);
            }
        }
        if (m_failure.empty() && !m_transparentGlassPanelsActivated &&
            m_glassPanelTemplate.IsFullyResident())
        {
            std::string panelError;
            if (!ActivateTransparentGlassPanels(context, panelError))
            {
                m_failure = std::move(panelError);
            }
        }
    }

    void InteriorRenderingSample::OnInput(SampleContext& context)
    {
        if (ShouldConsumeOrbitInput(context.options.smoke,
                                    m_interiorPresentationPrepared,
                                    m_orbitCamera.IsInitialized(),
                                    context.input != nullptr))
        {
            static_cast<void>(m_orbitCamera.Update(
                *context.input, context.cameras, context.camera));
        }
    }

    void InteriorRenderingSample::OnViewportResize(SampleContext& context,
                                                    uint32 width,
                                                    uint32 height)
    {
        if (width == 0 || height == 0)
            return;

        const float32 aspect =
            static_cast<float32>(width) / static_cast<float32>(height);
        static_cast<void>(m_orbitCamera.SetAspectRatio(
            aspect, context.cameras, context.camera));
    }

    void InteriorRenderingSample::ObserveDiagnostics(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (!m_failure.empty())
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.ASSET_REQUEST_FAILED"),
                           AssessmentCode("RENDER.INTERIOR.ASSET_RESIDENCY"),
                           m_failure);
            return;
        }
        if (!HasCompletedFrame(diagnostics))
            return;

        ObserveCapabilities(diagnostics, assessment);
        if (!m_failure.empty())
            return;
        if (!AreAssetsFullyResident())
            return;

        const uint64 completedFrameSequence =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        if (completedFrameSequence != m_lastCompletedFrameSequence)
        {
            m_lastCompletedFrameSequence = completedFrameSequence;
            ++m_completedResidentFrameCount;
        }

        if (!IsRenderScenePresentationCovered(diagnostics))
        {
            if (m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
            {
                PublishFailure(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.PRESENTED_SCENE_REVISION_NOT_CONVERGED"),
                    RenderSceneRevisionInvariant.code,
                    "The completed interior frame did not cover the exact required Scene revision at the Engine-to-Render boundary.",
                    FindingClass::InstrumentationGap);
            }
            return;
        }

        if (!IsRenderPathQualified(diagnostics))
        {
            if (m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
            {
                PublishFailure(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.RENDER_PATH_EXECUTION_INVALID"),
                    RenderPathInvariant.code,
                    "The completed interior frame did not execute the explicitly requested Direct or GPU-driven render path.");
            }
            return;
        }

        if (diagnostics.directionalShadowRequested &&
            !diagnostics.directionalShadowAvailable &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            if (!m_directionalCapabilityPublished)
            {
                PublishCapability(
                    assessment,
                    DirectionalCSMCapability,
                    DiagnosticValue<bool>::Unavailable(
                        "Directional CSM was requested without completed-frame diagnostics."),
                    diagnostics.directionalShadowReason,
                    m_directionalCapabilityPublished);
            }
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.DIRECTIONAL_CSM_MISSING"),
                           DirectionalCSMInvariant.code,
                           "Directional CSM was requested but no completed-frame diagnostic became available.",
                           FindingClass::InstrumentationGap);
            return;
        }
        if (!diagnostics.directionalShadowRequested &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.DIRECTIONAL_CSM_MISSING"),
                           DirectionalCSMInvariant.code,
                           "The completed interior frame did not report the requested directional CSM path.",
                           FindingClass::CapabilityGap);
            return;
        }
        if (diagnostics.directionalShadowAvailable &&
            !diagnostics.directionalShadowSupported)
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.DIRECTIONAL_CSM_UNSUPPORTED"),
                           DirectionalCSMInvariant.code,
                           diagnostics.directionalShadowReason.empty()
                               ? "Directional CSM was observed but is unsupported."
                               : diagnostics.directionalShadowReason,
                           FindingClass::CapabilityGap);
            return;
        }

        if (!diagnostics.transparentAvailable &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            if (!m_transparentCapabilityPublished)
            {
                PublishCapability(
                    assessment,
                    TransparentPassCapability,
                    DiagnosticValue<bool>::Unavailable(
                        "Transparent-pass diagnostics did not become available after completed interior frames."),
                    "Transparent-pass diagnostics were not observed.",
                    m_transparentCapabilityPublished);
            }
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.TRANSPARENT_PASS_MISSING"),
                           TransparentInvariant.code,
                           "The completed interior frame did not expose transparent-pass diagnostics.",
                           FindingClass::InstrumentationGap);
            return;
        }

        if (!diagnostics.localLightingAvailable &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.LOCAL_LIGHTING_DIAGNOSTICS_MISSING"),
                           LocalLightingInvariant.code,
                           "The completed interior frame did not expose local-light admission diagnostics.",
                           FindingClass::InstrumentationGap);
            return;
        }

        if (diagnostics.directionalShadowAvailable &&
            diagnostics.directionalShadowRequested &&
            !IsDirectionalCSMQualified(diagnostics) &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.DIRECTIONAL_CSM_INVALID"),
                           DirectionalCSMInvariant.code,
                           "Directional CSM did not produce the required three sampled 2048-pixel cascades with caster and draw evidence.");
            return;
        }

        if (diagnostics.localLightingAvailable &&
            !IsLocalLightingQualified(diagnostics) &&
            m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
        {
            PublishFailure(assessment,
                           AssessmentCode("RENDER.INTERIOR.LOCAL_LIGHT_ADMISSION_INVALID"),
                           LocalLightingInvariant.code,
                           "The interior point or spot lights were not admitted exactly without overflow.");
            return;
        }

        if (diagnostics.transparentAvailable)
        {
            if (!IsTransparentFrameValid(diagnostics))
            {
                if (m_completedResidentFrameCount >=
                    InteriorMissingDiagnosticsFrameAllowance)
                {
                    PublishFailure(assessment,
                                   AssessmentCode("RENDER.INTERIOR.TRANSPARENT_PASS_INVALID"),
                                   TransparentInvariant.code,
                                   "Transparent execution contained insufficient work, unordered data, non-finite depth, skips, fallback bindings, or a failed preflight/execution.");
                }
                return;
            }
            ObserveTransparentOrder(diagnostics);
        }

        const bool lightingQualified = diagnostics.textureIBLEnabled &&
                                       diagnostics.clusteredLightingInitialized &&
                                       diagnostics.clusteredLightingActiveClusters > 0;
        if (!lightingQualified)
        {
            if (m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
            {
                PublishFailure(assessment,
                               AssessmentCode("RENDER.INTERIOR.IBL_OR_CLUSTERED_LIGHTING_INVALID"),
                               LightingInvariant.code,
                               "The completed interior frame did not have both texture IBL and active clustered lighting.");
            }
            return;
        }

        const bool materialQualified = diagnostics.opaqueMaterialBindingsAvailable &&
                                       diagnostics.opaqueMaterialFallbackBindingCount == 0 &&
                                       diagnostics.opaqueMaterialFallbackTextureFlags == 0 &&
                                       diagnostics.transparentMaterialFallbackBindingCount == 0 &&
                                       diagnostics.transparentMaterialFallbackTextureFlags == 0;
        if (!materialQualified)
        {
            if (m_completedResidentFrameCount >=
                InteriorMissingDiagnosticsFrameAllowance)
            {
                PublishFailure(assessment,
                               AssessmentCode("RENDER.INTERIOR.MATERIAL_FALLBACK_OBSERVED"),
                               MaterialInvariant.code,
                               "Opaque or transparent material diagnostics reported fallback bindings or fallback texture flags.");
            }
            return;
        }

        if (IsFrameQualified(diagnostics))
        {
            m_diagnosticsQualified = true;
            PublishQualification(assessment, diagnostics);
        }
    }

    void InteriorRenderingSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        reporter.SetScenarioContractRevision(2);
        reporter.SetScenarioPhase(
            m_diagnosticsQualified ? "stable"
                                    : (m_failure.empty()
                                           ? "AwaitingQualification"
                                           : "Failed"));
        reporter.Enable("AsynchronousModelRequest");
        reporter.Enable("AsynchronousEnvironmentRequest");
        reporter.Enable("InteriorCameraConfiguration");
        reporter.Enable("DirectionalCSMRequested");
        reporter.Enable("ProductionSponzaAsset");
        reporter.Enable("DeterministicTransparentGlassPanels");
        reporter.Enable("LocalLightsWithoutLocalShadows");
        reporter.Enable("TransparentPassQualificationRequested");
        reporter.Enable("GeometryDepthOcclusionQualificationRequested");
        if (m_pointShadowCapabilityPublished &&
            !m_pointShadowSupportedObserved)
        {
            reporter.Unsupported("PointShadow");
        }
        if (m_spotShadowCapabilityPublished &&
            !m_spotShadowSupportedObserved)
        {
            reporter.Unsupported("SpotShadow");
        }
        if (m_hzbCapabilityPublished && !m_hzbSupportedObserved)
            reporter.Unsupported("HZBOcclusion");
        reporter.ResourceDiagnostic(
            "render path=" + std::string(GetSampleRenderPathName(m_renderPath)));
        reporter.ResourceDiagnostic("directional cascades requested=3");
        reporter.ResourceDiagnostic("directional shadow map requested=2048");
        reporter.ResourceDiagnostic("point lights requested=" +
                                  std::to_string(m_pointLightCount));
        reporter.ResourceDiagnostic("spot lights requested=" +
                                  std::to_string(m_spotLightCount));
        reporter.ResourceDiagnostic("point local-shadow requests=0");
        reporter.ResourceDiagnostic("spot local-shadow requests=0");
        reporter.ResourceDiagnostic("HZB requests=0");
        if (m_productionAssetCompositionCaptured)
        {
            reporter.ResourceDiagnostic(
                "verified production Sponza meshes=" +
                std::to_string(m_productionMeshCount));
            reporter.ResourceDiagnostic(
                "verified production Sponza materials=" +
                std::to_string(m_productionMaterialCount));
            reporter.ResourceDiagnostic(
                "verified production Sponza textures=" +
                std::to_string(m_productionTextureCount));
        }
        reporter.ResourceDiagnostic(
            "activated transparent glass panels=" +
            std::to_string(m_transparentGlassPanelCount));

        if (m_model.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.ResourceDiagnostic("model path=" +
                                      m_model.sourcePath.string());
        }
        if (m_glassPanelTemplate.status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("TransparentGlassPanelResourceLoad");
            reporter.ResourceDiagnostic("glass panel source path=" +
                                      m_glassPanelTemplate.sourcePath.string());
        }
        if (m_environment.IsValid())
        {
            reporter.Enable("EnvironmentResourceLoad");
            reporter.Enable("EnvironmentIBL");
            reporter.ResourceDiagnostic("environment path=" +
                                      m_environment.sourcePath.string());
        }
        if (m_directionalLightCreated)
            reporter.Enable("DirectionalLightScene");
        if (m_directionalCSMObserved)
            reporter.Enable("DirectionalCSMObserved");
        if (m_transparentObserved)
            reporter.Enable("TransparentPassObserved");
        if (m_localLightingObserved)
            reporter.Enable("LocalLightingAdmissionObserved");
        if (m_pointShadowCapabilityPublished)
        {
            reporter.ResourceDiagnostic(
                "point shadow support observed=" +
                std::string(m_pointShadowSupportedObserved ? "true" : "false"));
        }
        if (m_spotShadowCapabilityPublished)
        {
            reporter.ResourceDiagnostic(
                "spot shadow support observed=" +
                std::string(m_spotShadowSupportedObserved ? "true" : "false"));
        }
        if (m_hzbCapabilityPublished)
        {
            reporter.ResourceDiagnostic(
                "HZB occlusion observed=" +
                std::string(m_hzbSupportedObserved ? "true" : "false"));
        }
        if (m_diagnosticsQualified)
        {
            reporter.Enable("InteriorRenderingDiagnosticsQualified");
            reporter.Enable("OpaquePassObserved");
            reporter.Enable("ClusteredLightingObserved");
            reporter.Enable("TextureIBLEnabled");
            static_cast<void>(reporter.AppendScenarioAction({
                "PresentQualifiedInteriorFrame",
                m_qualifiedSceneRevision,
                m_qualifiedPresentationSequence,
                m_qualifiedAppliedSceneRevision,
                true}));
        }
        static_cast<void>(reporter.AppendScenarioInvariant({
            "VerifiedProductionAssetComposition",
            m_productionAssetCompositionCaptured,
            "meshes=" + std::to_string(m_productionMeshCount) +
                ";materials=" + std::to_string(m_productionMaterialCount) +
                ";textures=" + std::to_string(m_productionTextureCount)}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "DeterministicTransparentGlassPanels",
            m_transparentGlassPanelsActivated &&
                m_transparentGlassPanelCount == InteriorTransparentDrawCount,
            "activatedPanels=" + std::to_string(m_transparentGlassPanelCount)}));
        static_cast<void>(reporter.AppendScenarioInvariant({
            "CompletedInteriorRenderContract",
            m_diagnosticsQualified,
            "presentedFrame=" +
                std::to_string(m_qualifiedPresentationSequence)}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"productionMeshCount", m_productionMeshCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"productionMaterialCount", m_productionMaterialCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"productionTextureCount", m_productionTextureCount}));
        static_cast<void>(reporter.AppendScenarioMetric(
            {"transparentGlassPanelCount", m_transparentGlassPanelCount}));
    }

    SampleReadiness InteriorRenderingSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failure.empty())
            return SampleReadiness::Failed(m_failure);
        if (!m_model.IsFullyResident())
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for production Sponza to become fully resident");
        }
        if (!m_productionAssetCompositionCaptured)
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for production Sponza composition evidence");
        }
        if (!m_glassPanelTemplate.IsFullyResident() ||
            !m_transparentGlassPanelsActivated)
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for deterministic transparent glass panels");
        }
        if (!m_environment.IsValid())
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for the environment to become fully resident");
        }
        if (!HasCompletedFrame(diagnostics))
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for completed-frame diagnostics");
        }
        if (!IsRenderScenePresentationCovered(diagnostics))
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for the required Scene revision to be covered by a completed frame");
        }
        if (!IsRenderPathQualified(diagnostics))
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for the explicitly requested Direct or GPU-driven execution path");
        }
        if (!m_diagnosticsQualified)
        {
            return SampleReadiness::Pending(
                "Interior rendering is waiting for qualified directional, local-light, transparent, IBL, and material diagnostics");
        }
        return SampleReadiness::Ready();
    }

    bool InteriorRenderingSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void InteriorRenderingSample::Shutdown(SampleContext& context)
    {
        bool cancellationFailed = false;
        if (m_model.request.IsValid() && !context.models.Cancel(m_model))
        {
            m_failure = "Failed to cancel the interior ECS model request.";
            cancellationFailed = true;
        }
        if (m_glassPanelTemplate.request.IsValid() &&
            !context.models.Cancel(m_glassPanelTemplate))
        {
            m_failure = "Failed to cancel the interior ECS glass-panel request.";
            cancellationFailed = true;
        }
        if (m_environment.request.IsValid() &&
            !context.environments.Cancel(m_environment))
        {
            m_failure = "Failed to cancel the interior ECS environment request.";
            cancellationFailed = true;
        }
        m_interiorBounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_renderPath = SampleRenderPath::Auto;
        if (!cancellationFailed)
        {
            m_failure.clear();
        }
        ResetDiagnosticState();
    }

    bool InteriorRenderingSample::AreAssetsFullyResident() const noexcept
    {
        return m_model.IsFullyResident() &&
               m_glassPanelTemplate.IsFullyResident() &&
               m_transparentGlassPanelsActivated &&
               m_productionAssetCompositionCaptured &&
               m_environment.IsValid();
    }

    bool InteriorRenderingSample::HasCompletedFrame(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return diagnostics.available && diagnostics.rendered &&
               diagnostics.lastPresentedFrameSequence.IsAvailable() &&
               *diagnostics.lastPresentedFrameSequence.GetValue() != 0;
    }

    bool InteriorRenderingSample::IsRenderScenePresentationCovered(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return diagnostics.renderSceneValuesAvailable &&
               diagnostics.renderSceneAppliedRevision ==
                   diagnostics.renderSceneRequiredRevision &&
               diagnostics.engineRenderRuntimeAvailable &&
               diagnostics.engineRequiredSceneRevision <=
                   diagnostics.renderSceneAppliedRevision;
    }

    bool InteriorRenderingSample::IsRenderPathQualified(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        switch (m_renderPath)
        {
            case SampleRenderPath::Auto:
                // Auto is intentionally policy-selected by the engine. The P0
                // qualification matrix uses explicit Direct/GPU-driven modes.
                return true;
            case SampleRenderPath::GPUDriven:
                return diagnostics.gpuDrivenPolicyDecisionAvailable &&
                       diagnostics.gpuDrivenRequestedMode == "ForceEnabled" &&
                       diagnostics.gpuDrivenPolicyReason == "None" &&
                       diagnostics.gpuDrivenEnabled &&
                       diagnostics.gpuDrivenGraphPassRecorded &&
                       diagnostics.gpuDrivenExecutionRecorded &&
                       diagnostics.gpuDrivenOpaqueIndirectRequested &&
                       diagnostics.gpuDrivenOpaqueIndirectEligible &&
                       diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                       diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0;
            case SampleRenderPath::Direct:
                return diagnostics.gpuDrivenPolicyDecisionAvailable &&
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
            default:
                return false;
        }
    }

    bool InteriorRenderingSample::IsDirectionalCSMQualified(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return diagnostics.directionalShadowRequested &&
               diagnostics.directionalShadowSupported &&
               diagnostics.directionalShadowOutputReady &&
               diagnostics.directionalShadowSamplingEnabled &&
               diagnostics.directionalShadowRequestedCascadeCount ==
                   InteriorDirectionalCascadeCount &&
               diagnostics.directionalShadowProducedCascadeCount ==
                   InteriorDirectionalCascadeCount &&
               diagnostics.directionalShadowResolvedCascadeCount ==
                   InteriorDirectionalCascadeCount &&
               diagnostics.directionalShadowMapSize == InteriorShadowMapSize &&
               diagnostics.directionalShadowCasterCount > 0 &&
               diagnostics.directionalShadowDrawCount > 0;
    }

    bool InteriorRenderingSample::IsLocalLightingQualified(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return diagnostics.pointLightRequestedCount == InteriorPointLightCount &&
               diagnostics.pointLightAdmittedCount == InteriorPointLightCount &&
               diagnostics.pointLightOverflowCount == 0 &&
               diagnostics.pointShadowRequestedCount == 0 &&
               diagnostics.spotLightRequestedCount == InteriorSpotLightCount &&
               diagnostics.spotLightAdmittedCount == InteriorSpotLightCount &&
               diagnostics.spotLightOverflowCount == 0 &&
               diagnostics.spotShadowRequestedCount == 0;
    }

    bool InteriorRenderingSample::IsTransparentFrameValid(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return diagnostics.transparentOrderValid &&
               diagnostics.transparentRejectedNonFiniteDepthCount == 0 &&
               diagnostics.transparentCandidateDrawItemCount >=
                   InteriorTransparentDrawCount &&
               diagnostics.transparentPreparedDrawItemCount >=
                   InteriorTransparentDrawCount &&
               diagnostics.transparentExecutedPacketCount >=
                   InteriorTransparentDrawCount &&
               diagnostics.transparentExecutedDrawCount >=
                   InteriorTransparentDrawCount &&
               diagnostics.transparentSkippedMaterialBindingCount == 0 &&
               diagnostics.transparentSkippedResourceCount == 0 &&
               diagnostics.transparentSkippedExecutionDrawCount == 0 &&
               diagnostics.transparentMaterialBindingCount >=
                   InteriorTransparentDrawCount &&
               diagnostics.transparentMaterialFallbackBindingCount == 0 &&
               diagnostics.transparentMaterialFallbackTextureFlags == 0 &&
               !diagnostics.transparentNoWork &&
               !diagnostics.transparentPreflightFailed &&
               !diagnostics.transparentExecutionFailed;
    }

    bool InteriorRenderingSample::IsFrameQualified(
        const SampleRenderDiagnostics& diagnostics) const noexcept
    {
        return IsRenderScenePresentationCovered(diagnostics) &&
               IsRenderPathQualified(diagnostics) &&
               IsDirectionalCSMQualified(diagnostics) &&
               IsLocalLightingQualified(diagnostics) &&
               IsTransparentFrameValid(diagnostics) &&
               m_transparentStableFrameCount >=
                   InteriorRequiredStableTransparentFrames &&
               diagnostics.textureIBLEnabled &&
               diagnostics.clusteredLightingInitialized &&
               diagnostics.clusteredLightingActiveClusters > 0 &&
               diagnostics.opaqueExecutionCompleted &&
               diagnostics.opaqueMaterialBindingsAvailable &&
               diagnostics.opaqueMaterialFallbackBindingCount == 0 &&
               diagnostics.opaqueMaterialFallbackTextureFlags == 0;
    }

    bool InteriorRenderingSample::PrepareInteriorPresentation(
        SampleContext& context,
        std::string& outError)
    {
        outError.clear();
        const SceneECS::SceneEntityRef root = m_model.GetRootEntityRef();
        if (!root.IsValid() || root.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
            context.scene.GetEntityRef(root.entity) != root)
        {
            outError = "Interior rendering Sponza root is stale during ECS presentation placement";
            return false;
        }
        const ECS::Registry& registry = context.scene.GetRegistry();
        const SceneECS::ParentRelation* rootParent =
            registry.TryGet<SceneECS::ParentRelation>(root.entity);
        const SceneECS::LocalTransform* rootTransform =
            registry.TryGet<SceneECS::LocalTransform>(root.entity);
        if (rootTransform == nullptr ||
            (rootParent != nullptr && rootParent->parent.IsValid()))
        {
            outError = "Interior rendering requires a top-level imported ECS Sponza root for presentation placement";
            return false;
        }

        AABB sourceBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                m_model, context.scene, sourceBounds))
        {
            outError = "Interior rendering Sponza has invalid world bounds";
            return false;
        }
        const Vec3 sourceSize = sourceBounds.GetSize();
        const float32 sourceCorridorExtent =
            std::max(sourceSize.x, sourceSize.z);
        if (!sourceBounds.IsValid() || !std::isfinite(sourceCorridorExtent) ||
            sourceCorridorExtent <= 0.00001f)
        {
            outError = "Interior rendering Sponza has invalid world bounds";
            return false;
        }

        // Keep the coordinator-owned root top-level so its Scene identity can
        // cross the retirement handoff during host teardown. Compose the same
        // display transform directly: a uniform unit conversion followed by
        // the quarter turn that aligns Sponza's long source axis with the
        // camera's -Z corridor used by the glass and local-light layout.
        const float32 presentationScale =
            InteriorCorridorExtent / sourceCorridorExtent;
        const Quat presentationRotation =
            QuatFromEuler(Vec3(0.0f, radians(90.0f), 0.0f));
        const SceneECS::LocalTransform authoredTransform = *rootTransform;
        SceneECS::LocalTransform transformed = authoredTransform;
        transformed.translation = presentationRotation *
                                  (authoredTransform.translation * presentationScale);
        transformed.rotation = normalize(
            presentationRotation * authoredTransform.rotation);
        transformed.scale = authoredTransform.scale * presentationScale;
        if (!context.scene.SetLocalTransform(root.entity, transformed))
        {
            outError = "Interior rendering could not apply the ECS Sponza presentation transform";
            return false;
        }

        AABB rotatedScaledBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                m_model, context.scene, rotatedScaledBounds))
        {
            outError = "Interior rendering could not transform the Sponza world bounds";
            return false;
        }
        const Vec3 transformedCenter = rotatedScaledBounds.GetCenter();
        transformed.translation +=
            Vec3(-transformedCenter.x,
                 -rotatedScaledBounds.GetMin().y,
                 -transformedCenter.z);
        if (!context.scene.SetLocalTransform(root.entity, transformed))
        {
            outError = "Interior rendering could not center the ECS Sponza presentation transform";
            return false;
        }
        if (!TryComputeSampleModelRenderableWorldBounds(
                m_model, context.scene, m_interiorBounds))
        {
            outError = "Interior rendering could not resolve the placed Sponza bounds";
            return false;
        }
        if (!IsNormalizedInteriorPlacement(sourceBounds,
                                           m_interiorBounds,
                                           InteriorCorridorExtent))
        {
            outError = "Interior rendering Sponza bounds are not normalized, centered, and grounded";
            return false;
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(m_interiorBounds,
                                              aspect,
                                              InteriorVerticalFov,
                                              1.10f);
        if (!m_cameraFrame.valid)
        {
            outError = "Interior rendering could not derive a finite camera frame for normalized Sponza";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        if (!BuildInteriorOrbitSettings(m_interiorBounds,
                                        aspect,
                                        m_cameraFrame.distance,
                                        orbitSettings))
        {
            outError = "Interior rendering could not configure a finite corridor orbit camera";
            return false;
        }
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            outError = "Interior rendering orbit camera initialization failed";
            return false;
        }
        const OrbitCameraRigPose fittedPose = m_orbitCamera.GetPose();
        if (!fittedPose.valid)
        {
            outError = "Interior rendering orbit camera could not produce a fitted corridor pose";
            return false;
        }

        // The controller deliberately fits every newly initialized rig.  This
        // interior walk-through instead starts at the authored indoor corridor
        // distance, so derive the exact exponential zoom intent from that fit.
        SampleOrbitCameraInput corridorStart;
        if (!TryBuildInteriorStartInput(fittedPose,
                                        m_orbitCamera.GetSettings(),
                                        corridorStart))
        {
            outError = "Interior rendering could not derive a finite authored corridor start";
            return false;
        }
        if (!m_orbitCamera.ApplyInput(
                corridorStart, context.cameras, context.camera))
        {
            outError = "Interior rendering could not apply the ECS corridor camera pose";
            return false;
        }
        const OrbitCameraRigPose orbitPose = m_orbitCamera.GetPose();
        if (!orbitPose.valid ||
            std::abs(orbitPose.distance - InteriorInitialOrbitDistance) >
                InteriorOrbitDistanceTolerance)
        {
            outError = "Interior rendering orbit camera produced an invalid corridor pose";
            return false;
        }
        m_orbitCamera.CaptureResetAnchor();
        m_cameraFrame.target = orbitPose.pivot;
        m_cameraFrame.distance = orbitPose.distance;
        m_cameraFrame.nearPlane = orbitPose.nearPlane;
        m_cameraFrame.farPlane = orbitPose.farPlane;
        m_interiorPresentationPrepared = true;
        return true;
    }

    bool InteriorRenderingSample::IsNormalizedInteriorPlacement(
        const AABB& sourceBounds,
        const AABB& placedBounds,
        float32 targetCorridorExtent) noexcept
    {
        if (!sourceBounds.IsValid() || !placedBounds.IsValid() ||
            !std::isfinite(targetCorridorExtent) ||
            targetCorridorExtent <= 0.0f)
        {
            return false;
        }
        const Vec3 sourceSize = sourceBounds.GetSize();
        const Vec3 placedSize = placedBounds.GetSize();
        const float32 sourceHorizontalExtent =
            std::max(sourceSize.x, sourceSize.z);
        const float32 placedHorizontalExtent =
            std::max(placedSize.x, placedSize.z);
        const Vec3 placedCenter = placedBounds.GetCenter();
        return std::isfinite(sourceHorizontalExtent) &&
               sourceHorizontalExtent > 0.00001f &&
               std::isfinite(placedHorizontalExtent) &&
               std::abs(placedHorizontalExtent - targetCorridorExtent) <=
                   targetCorridorExtent * InteriorPlacementExtentTolerance &&
               std::abs(placedBounds.GetMin().y) <=
                   InteriorPlacementGroundTolerance &&
               std::abs(placedCenter.x) <= InteriorPlacementGroundTolerance &&
               std::abs(placedCenter.z) <= InteriorPlacementGroundTolerance;
    }

    bool InteriorRenderingSample::ShouldConsumeOrbitInput(
        bool smoke,
        bool presentationReady,
        bool orbitInitialized,
        bool inputAvailable) noexcept
    {
        return !smoke && presentationReady && orbitInitialized &&
               inputAvailable;
    }

    bool InteriorRenderingSample::BuildInteriorOrbitSettings(
        const AABB& bounds,
        float32 aspectRatio,
        float32 framingDistance,
        SampleOrbitCameraSettings& outSettings) noexcept
    {
        outSettings = {};
        if (!bounds.IsValid() ||
            !std::isfinite(bounds.GetMin().x) ||
            !std::isfinite(bounds.GetMin().y) ||
            !std::isfinite(bounds.GetMin().z) ||
            !std::isfinite(bounds.GetMax().x) ||
            !std::isfinite(bounds.GetMax().y) ||
            !std::isfinite(bounds.GetMax().z) ||
            !std::isfinite(aspectRatio) || aspectRatio <= 0.0f ||
            !std::isfinite(framingDistance) || framingDistance <= 0.0f)
        {
            return false;
        }

        outSettings.mode = OrbitCameraMode::FreeOrbit;
        outSettings.bounds = bounds;
        outSettings.pivot = Vec3(0.0f, 2.85f, -5.0f);
        outSettings.distance = InteriorInitialOrbitDistance;
        outSettings.yaw = 0.0f;
        outSettings.pitch = 0.12f;
        outSettings.minDistance = InteriorMinimumOrbitDistance;
        outSettings.maxDistance = std::max(framingDistance * 3.0f,
                                           InteriorInitialOrbitDistance * 2.0f);
        outSettings.zoomExponent = 0.08f;
        outSettings.verticalFovRadians = InteriorVerticalFov;
        outSettings.aspectRatio = aspectRatio;
        return outSettings.minDistance < outSettings.distance &&
               outSettings.maxDistance >= outSettings.distance;
    }

    bool InteriorRenderingSample::TryBuildInteriorStartInput(
        const OrbitCameraRigPose& fittedPose,
        const SampleOrbitCameraSettings& settings,
        SampleOrbitCameraInput& outInput) noexcept
    {
        outInput = {};
        if (!fittedPose.valid || !std::isfinite(fittedPose.distance) ||
            fittedPose.distance <= 0.0f ||
            !std::isfinite(settings.zoomExponent) ||
            settings.zoomExponent <= 0.0f ||
            !std::isfinite(settings.minDistance) ||
            !std::isfinite(settings.maxDistance) ||
            settings.minDistance > InteriorInitialOrbitDistance ||
            settings.maxDistance < InteriorInitialOrbitDistance)
        {
            return false;
        }

        const float32 logDistanceRatio = std::log(
            fittedPose.distance / InteriorInitialOrbitDistance);
        if (!std::isfinite(logDistanceRatio) ||
            std::abs(logDistanceRatio) > 20.0f)
        {
            return false;
        }

        outInput.scrollDelta = logDistanceRatio / settings.zoomExponent;
        return std::isfinite(outInput.scrollDelta);
    }

    bool InteriorRenderingSample::CaptureProductionAssetComposition(
        std::string& outError)
    {
        outError.clear();
        const ResourceSceneAdapters::EcsModelAssetMetadata& metadata =
            m_model.status.modelMetadata;
        if (!m_model.IsFullyResident() || !metadata.HasPublishedSource())
        {
            outError =
                "Production Sponza ECS metadata was unavailable while collecting composition evidence";
            return false;
        }

        std::unordered_set<uint64> textureAssetIds;
        for (const AssetId mesh : metadata.meshAssetIds)
        {
            if (!mesh.IsValid())
            {
                outError =
                    "Production Sponza ECS metadata exposed an invalid mesh identity";
                return false;
            }
        }

        for (const ResourceSceneAdapters::EcsModelAssetMetadata::Material& material :
             metadata.materials)
        {
            if (!material.materialAssetId.IsValid())
            {
                outError =
                    "Production Sponza ECS metadata exposed an invalid material identity";
                return false;
            }
            for (const ResourceSceneAdapters::EcsModelAssetMetadata::TextureSlot& slot :
                 material.textureSlots)
            {
                if (slot.slot.empty() || !slot.textureAssetId.IsValid())
                {
                    outError =
                        "Production Sponza ECS metadata exposed an invalid material texture identity";
                    return false;
                }
                textureAssetIds.insert(slot.textureAssetId.value);
            }
        }

        if (metadata.meshAssetIds.empty() || metadata.materials.empty() ||
            textureAssetIds.empty())
        {
            outError =
                "Production Sponza composition evidence requires non-zero verified meshes, materials, and textures";
            return false;
        }

        m_productionMeshCount = static_cast<uint32>(metadata.meshAssetIds.size());
        m_productionMaterialCount = static_cast<uint32>(metadata.materials.size());
        m_productionTextureCount = static_cast<uint32>(textureAssetIds.size());
        m_productionAssetCompositionCaptured = true;
        return true;
    }

    bool InteriorRenderingSample::ActivateTransparentGlassPanels(
        SampleContext& context,
        std::string& outError)
    {
        outError.clear();
        const ResourceSceneAdapters::EcsModelAssetMetadata& metadata =
            m_glassPanelTemplate.status.modelMetadata;
        if (!m_glassPanelTemplate.IsFullyResident() ||
            !metadata.HasPublishedSource())
        {
            outError =
                "Transparent glass-panel ECS support asset was not ready for deterministic activation";
            return false;
        }

        struct GlassPanelSpec
        {
            std::string_view sourceName;
            std::string_view materialName;
        };
        constexpr std::array<GlassPanelSpec, InteriorTransparentDrawCount>
            GlassPanels{{
                {"Transparent_Portal_Left", "Transparent_GlassA"},
                {"Transparent_Portal_Right", "Transparent_GlassB"},
            }};

        std::unordered_map<std::string_view, AssetId> blendMaterials;
        for (const ResourceSceneAdapters::EcsModelAssetMetadata::Material& material :
             metadata.materials)
        {
            if (!material.materialAssetId.IsValid())
            {
                outError = "Transparent glass-panel support asset contains an invalid material";
                return false;
            }
            if (material.alphaMode != Resource::MaterialAlphaMode::Blend)
            {
                continue;
            }
            if (!blendMaterials.emplace(material.sourceName, material.materialAssetId).second)
            {
                outError = "Transparent glass-panel support asset contains duplicate BLEND material names";
                return false;
            }
        }

        const ECS::Registry& registry = context.scene.GetRegistry();
        uint32 activatedPanels = 0;
        for (const GlassPanelSpec& panel : GlassPanels)
        {
            const auto material = blendMaterials.find(panel.materialName);
            if (material == blendMaterials.end())
            {
                outError = "Transparent glass-panel support asset is missing its BLEND material '" +
                           std::string(panel.materialName) + "'";
                return false;
            }

            const ResourceSceneAdapters::PreparedModelEntityMapping* panelMapping = nullptr;
            for (const ResourceSceneAdapters::PreparedModelEntityMapping& mapping :
                 m_glassPanelTemplate.status.entityMappings)
            {
                // A source node with exactly one mesh owns its Mesh fragment
                // directly; only multi-primitive source nodes produce derived
                // mappings. Select the authoritative renderable entity rather
                // than assuming every primitive is represented by a child.
                if (mapping.sourceName != panel.sourceName ||
                    registry.TryGet<SceneECS::Mesh>(mapping.entity) == nullptr)
                {
                    continue;
                }
                if (panelMapping != nullptr)
                {
                    outError = "Transparent glass-panel support asset has ambiguous renderable source mapping '" +
                               std::string(panel.sourceName) + "'";
                    return false;
                }
                panelMapping = &mapping;
            }
            if (panelMapping == nullptr || !panelMapping->entity.IsValid() ||
                !context.scene.GetEntityRef(panelMapping->entity).IsValid())
            {
                outError = "Transparent glass-panel support asset is missing ECS renderable panel '" +
                           std::string(panel.sourceName) + "'";
                return false;
            }

            const SceneECS::Mesh* mesh = registry.TryGet<SceneECS::Mesh>(panelMapping->entity);
            const SceneECS::MaterialSlots* currentSlots =
                registry.TryGet<SceneECS::MaterialSlots>(panelMapping->entity);
            const SceneECS::Visibility* currentVisibility =
                registry.TryGet<SceneECS::Visibility>(panelMapping->entity);
            if (mesh == nullptr || currentSlots == nullptr || currentVisibility == nullptr ||
                !mesh->meshAssetId.IsValid() || mesh->submeshCount == 0 ||
                currentSlots->count == 0)
            {
                outError = "Transparent glass-panel support asset has an invalid ECS renderable panel '" +
                           std::string(panel.sourceName) + "'";
                return false;
            }

            SceneECS::MaterialSlots updatedSlots = *currentSlots;
            updatedSlots.values[0].materialAssetId = material->second;
            updatedSlots.values[0].materialMode = RenderMaterialMode::Transparent;
            SceneECS::Visibility updatedVisibility = *currentVisibility;
            updatedVisibility.visible = true;
            updatedVisibility.castsShadow = false;
            if (!context.scene.SetFragment(panelMapping->entity, updatedSlots) ||
                !context.scene.SetFragment(panelMapping->entity, updatedVisibility))
            {
                outError = "Transparent glass-panel ECS activation was not retained";
                return false;
            }
            ++activatedPanels;
        }

        if (activatedPanels != InteriorTransparentDrawCount)
        {
            outError = "Transparent glass-panel activation did not produce exactly two panels";
            return false;
        }
        m_transparentGlassPanelCount = activatedPanels;
        m_transparentGlassPanelsActivated = true;
        return true;
    }

    void InteriorRenderingSample::ObserveCapabilities(
        const SampleRenderDiagnostics& diagnostics,
        SampleAssessmentChannel& assessment)
    {
        if (diagnostics.directionalShadowRequested &&
            diagnostics.directionalShadowAvailable &&
            !m_directionalCapabilityPublished)
        {
            PublishCapability(assessment,
                              DirectionalCSMCapability,
                              DiagnosticValue<bool>::Available(
                                  diagnostics.directionalShadowSupported),
                              diagnostics.directionalShadowReason,
                              m_directionalCapabilityPublished);
            m_directionalCSMObserved = true;
        }
        if (diagnostics.transparentAvailable &&
            !m_transparentCapabilityPublished)
        {
            PublishCapability(assessment,
                              TransparentPassCapability,
                              DiagnosticValue<bool>::Available(true),
                              "Transparent-pass diagnostics were observed in a completed frame.",
                              m_transparentCapabilityPublished);
            m_transparentObserved = true;
        }
        if (diagnostics.localLightingAvailable &&
            !m_pointShadowCapabilityPublished)
        {
            PublishCapability(assessment,
                              PointShadowCapability,
                              DiagnosticValue<bool>::Available(
                                  diagnostics.pointShadowSupported),
                              diagnostics.pointShadowReason,
                              m_pointShadowCapabilityPublished);
            m_pointShadowSupportedObserved = diagnostics.pointShadowSupported;
            if (diagnostics.pointShadowSupported)
            {
                PublishFailure(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.POINT_SHADOW_SUPPORTED_MISMATCH"),
                    PointShadowCapability.code,
                    "Point-shadow diagnostics reported supported although the P0 interior contract explicitly requires PointShadow=Unsupported.",
                    FindingClass::CapabilityGap);
            }
            else
            {
                PublishCapabilityGap(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.POINT_SHADOW_CAPABILITY_GAP"),
                    "Point shadows are explicitly unsupported for P0 interior-rendering.",
                    m_pointShadowGapPublished);
            }
        }
        if (diagnostics.localLightingAvailable &&
            !m_spotShadowCapabilityPublished)
        {
            PublishCapability(assessment,
                              SpotShadowCapability,
                              DiagnosticValue<bool>::Available(
                                  diagnostics.spotShadowSupported),
                              diagnostics.spotShadowReason,
                              m_spotShadowCapabilityPublished);
            m_spotShadowSupportedObserved = diagnostics.spotShadowSupported;
            if (diagnostics.spotShadowSupported)
            {
                PublishFailure(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.SPOT_SHADOW_SUPPORTED_MISMATCH"),
                    SpotShadowCapability.code,
                    "Spot-shadow diagnostics reported supported although the P0 interior contract explicitly requires SpotShadow=Unsupported.",
                    FindingClass::CapabilityGap);
            }
            else
            {
                PublishCapabilityGap(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.SPOT_SHADOW_CAPABILITY_GAP"),
                    "Spot shadows are explicitly unsupported for P0 interior-rendering.",
                    m_spotShadowGapPublished);
            }
        }
        if (!m_hzbCapabilityPublished)
        {
            PublishCapability(assessment,
                              HZBCapability,
                              DiagnosticValue<bool>::Available(
                                  diagnostics.hzbSupported),
                              diagnostics.hzbReason,
                              m_hzbCapabilityPublished);
            m_hzbSupportedObserved = diagnostics.hzbSupported;
            if (diagnostics.hzbSupported)
            {
                PublishFailure(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.HZB_SUPPORTED_MISMATCH"),
                    HZBCapability.code,
                    "HZB diagnostics reported supported although the P0 interior contract explicitly requires HZBOcclusion=Unsupported.",
                    FindingClass::CapabilityGap);
            }
            else
            {
                PublishCapabilityGap(
                    assessment,
                    AssessmentCode("RENDER.INTERIOR.HZB_CAPABILITY_GAP"),
                    "HZB occlusion is explicitly unsupported for P0 interior-rendering.",
                    m_hzbGapPublished);
            }
        }
        m_localLightingObserved |= diagnostics.localLightingAvailable;
    }

    void InteriorRenderingSample::ObserveTransparentOrder(
        const SampleRenderDiagnostics& diagnostics) noexcept
    {
        const uint64 frameSequence =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        if (frameSequence == m_lastTransparentFrameSequence)
            return;

        if (m_lastTransparentFrameSequence != 0 &&
            diagnostics.transparentOrderHash == m_lastTransparentOrderHash)
        {
            ++m_transparentStableFrameCount;
        }
        else
        {
            m_transparentStableFrameCount = 1;
        }
        m_lastTransparentFrameSequence = frameSequence;
        m_lastTransparentOrderHash = diagnostics.transparentOrderHash;
    }

    void InteriorRenderingSample::PublishCapability(
        SampleAssessmentChannel& assessment,
        const AssessmentCapability& capability,
        DiagnosticValue<bool> value,
        std::string reason,
        bool& published)
    {
        if (published)
            return;

        if (reason.empty())
        {
            if (!value.IsAvailable())
            {
                reason = value.GetReason();
            }
            else if (value.GetValue().value_or(false))
            {
                reason =
                    "Capability was observed in completed-frame diagnostics.";
            }
            else
            {
                reason =
                    "Capability was observed as unsupported or inactive in completed-frame diagnostics.";
            }
        }

        published = assessment.MarkCapabilityObservation(
            {capability,
             AssessmentCheckpoints::ActionApplied,
             std::move(value),
             std::move(reason)});
    }

    void InteriorRenderingSample::PublishCapabilityGap(
        SampleAssessmentChannel& assessment,
        AssessmentCode code,
        std::string detail,
        bool& published)
    {
        if (published)
            return;
        Finding finding;
        finding.code = std::move(code);
        finding.subsystemCode = AssessmentCode("RENDER.INTERIOR");
        finding.invariantCode = AssessmentCode("RENDER.INTERIOR.OPTIONAL_CAPABILITY");
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = FindingClass::CapabilityGap;
        finding.severity = FindingSeverity::Warning;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "An optional interior-rendering capability is unavailable.";
        finding.detail = std::move(detail);
        finding.expected = "Optional P0-A capabilities are reported without changing scene meaning.";
        finding.observed = finding.detail;
        finding.gating = false;
        static_cast<void>(assessment.TryPublish(std::move(finding)));
        published = true;
    }

    void InteriorRenderingSample::PublishFailure(
        SampleAssessmentChannel& assessment,
        AssessmentCode code,
        AssessmentCode invariantCode,
        std::string detail,
        FindingClass classification)
    {
        if (m_failurePublished)
            return;
        if (m_failure.empty())
            m_failure = detail;

        Finding finding;
        finding.code = std::move(code);
        finding.subsystemCode = AssessmentCode("RENDER.INTERIOR");
        finding.invariantCode = std::move(invariantCode);
        finding.checkpoint = AssessmentCheckpoints::ActionApplied;
        finding.classification = classification;
        finding.severity = FindingSeverity::Error;
        finding.confidence = FindingConfidence::Confirmed;
        finding.summary = "The interior-rendering product contract was not qualified.";
        finding.detail = std::move(detail);
        finding.expected = "The completed interior frame satisfies every gating rendering contract.";
        finding.observed = finding.detail;
        finding.gating = true;
        finding.capabilityBlocking =
            classification == FindingClass::CapabilityGap ||
            classification == FindingClass::InstrumentationGap;
        finding.blockingReason = "Interior rendering requires completed-frame contract evidence.";
        static_cast<void>(assessment.TryPublish(std::move(finding)));
        m_failurePublished = true;
    }

    void InteriorRenderingSample::PublishQualification(
        SampleAssessmentChannel& assessment,
        const SampleRenderDiagnostics& diagnostics)
    {
        if (m_qualificationPublished)
            return;
        m_qualifiedSceneRevision = diagnostics.renderSceneAppliedRevision;
        m_qualifiedAppliedSceneRevision = diagnostics.renderSceneAppliedRevision;
        m_qualifiedPresentationSequence =
            *diagnostics.lastPresentedFrameSequence.GetValue();
        // Both assessment actions are receipts for this already-presented
        // frame. Selecting a render policy during Setup alone is not evidence
        // that the renderer consumed the requested scene meaning.
        static_cast<void>(assessment.MarkAction(InteriorRenderPathAction));
        static_cast<void>(assessment.MarkAction(InteriorQualificationAction));
        for (const AssessmentInvariant& invariant :
             {DirectionalCSMInvariant,
              LocalLightingInvariant,
               TransparentInvariant,
               LightingInvariant,
               MaterialInvariant,
               RenderSceneRevisionInvariant,
               RenderPathInvariant})
        {
            static_cast<void>(assessment.TryPublish(
                AssessmentInvariantObservation{
                    invariant, AssessmentCheckpoints::ActionApplied}));
        }
        static_cast<void>(assessment.TryPublish(AssessmentSnapshot{
            AssessmentCheckpoints::ActionApplied,
            {
                {ResolvedCascadeMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(
                         diagnostics.directionalShadowResolvedCascadeCount))},
                {TransparentOrderHashMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     diagnostics.transparentOrderHash)},
                {PointLightsAdmittedMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.pointLightAdmittedCount))},
                {SpotLightsAdmittedMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(diagnostics.spotLightAdmittedCount))},
                {ActiveClustersMetric,
                 DiagnosticValue<AssessmentScalar>::Available(
                     static_cast<uint64>(
                         diagnostics.clusteredLightingActiveClusters))}}}));
        static_cast<void>(assessment.MarkCheckpoint(
            AssessmentCheckpoints::ActionApplied));
        m_qualificationPublished = true;
    }

    void InteriorRenderingSample::ResetDiagnosticState() noexcept
    {
        m_lastCompletedFrameSequence = 0;
        m_lastTransparentFrameSequence = 0;
        m_lastTransparentOrderHash = 0;
        m_completedResidentFrameCount = 0;
        m_transparentStableFrameCount = 0;
        m_directionalLightCreated = false;
        m_pointLightCount = 0;
        m_spotLightCount = 0;
        m_productionMeshCount = 0;
        m_productionMaterialCount = 0;
        m_productionTextureCount = 0;
        m_transparentGlassPanelCount = 0;
        m_interiorPresentationPrepared = false;
        m_qualifiedSceneRevision = 0;
        m_qualifiedPresentationSequence = 0;
        m_qualifiedAppliedSceneRevision = 0;
        m_productionAssetCompositionCaptured = false;
        m_transparentGlassPanelsActivated = false;
        m_directionalCapabilityPublished = false;
        m_transparentCapabilityPublished = false;
        m_pointShadowCapabilityPublished = false;
        m_spotShadowCapabilityPublished = false;
        m_hzbCapabilityPublished = false;
        m_pointShadowGapPublished = false;
        m_spotShadowGapPublished = false;
        m_hzbGapPublished = false;
        m_failurePublished = false;
        m_diagnosticsQualified = false;
        m_qualificationPublished = false;
        m_directionalCSMObserved = false;
        m_transparentObserved = false;
        m_localLightingObserved = false;
        m_pointShadowSupportedObserved = false;
        m_spotShadowSupportedObserved = false;
        m_hzbSupportedObserved = false;
    }
} // namespace RVX
