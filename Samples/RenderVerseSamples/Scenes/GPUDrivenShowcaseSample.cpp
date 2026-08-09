/** @file GPUDrivenShowcaseSample.cpp @brief Direct/GPU-driven parity scene. */

#include "Scenes/GPUDrivenShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "ResourceSceneAdapters/SceneAssetInstantiation.h"
#include "Scene/Components/CameraComponent.h"
#include "Scene/Components/LightComponent.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneEntity.h"
#include "Scene/SceneRuntime.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace RVX
{
    namespace
    {
        constexpr float32 GPUDrivenVerticalFov = 0.78539816339f;
        constexpr uint32 GPUDrivenGridExtent = 5;
        constexpr uint32 GPUDrivenGridInstanceCount =
            GPUDrivenGridExtent * GPUDrivenGridExtent;
        constexpr float32 GPUDrivenGridSpacingScale = 1.35f;

        const SampleInfo GPUDrivenShowcaseInfo{
            "gpu-driven",
            "GPU-Driven Rendering",
            "Compares Direct and GPU-driven execution over the same engine scene",
            "r7-triangle",
            SampleAssetPolicy::UserModelOrDefault,
            "",
            SampleEnvironmentPolicy::None,
            true,
            SampleRenderPath::GPUDriven,
        };

        std::string DescribeGPUDrivenState(
            const SampleRenderDiagnostics& diagnostics)
        {
            std::ostringstream stream;
            stream << "requestedMode=" << diagnostics.gpuDrivenRequestedMode
                   << ", policyReason=" << diagnostics.gpuDrivenPolicyReason
                   << ", qualification=" << diagnostics.gpuDrivenQualification
                   << ", enabled=" << diagnostics.gpuDrivenEnabled
                   << ", graphPassAdded=" << diagnostics.gpuDrivenGraphPassAdded
                   << ", graphPassRecorded=" << diagnostics.gpuDrivenGraphPassRecorded
                   << ", executionRecorded=" << diagnostics.gpuDrivenExecutionRecorded
                   << ", inputDrawItems="
                   << diagnostics.gpuDrivenGraphInputDrawItemCount
                   << ", indirectRequested="
                   << diagnostics.gpuDrivenOpaqueIndirectRequested
                   << ", indirectEligible="
                   << diagnostics.gpuDrivenOpaqueIndirectEligible
                   << ", indirectSubmitted="
                   << diagnostics.gpuDrivenOpaqueIndirectSubmitted
                   << ", indirectBatches="
                   << diagnostics.gpuDrivenOpaqueIndirectBatchCount
                   << ", directDraws="
                   << diagnostics.gpuDrivenOpaqueDirectDrawCount
                   << ", directInstancingPackets="
                   << diagnostics.opaqueInstancingExecutedPacketCount
                   << ", directInstancingDraws="
                   << diagnostics.opaqueInstancingSubmittedDrawCount
                   << ", directInstancingInstances="
                   << diagnostics.opaqueInstancingSubmittedInstanceCount
                   << ", fallback="
                   << diagnostics.gpuDrivenOpaqueFallbackReason;
            return stream.str();
        }
    } // namespace

    const SampleInfo& GPUDrivenShowcaseSample::GetInfo() const noexcept
    {
        return GPUDrivenShowcaseInfo;
    }

    bool GPUDrivenShowcaseSample::Setup(SampleContext& context,
                                        std::string& outError)
    {
        m_models.clear();
        m_bounds.Reset();
        m_cameraFrame = {};
        m_sceneInstanceCount = 0;
        m_failureReason.clear();
        m_instancesPlaced = false;
        m_renderablesActivated = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
        m_renderPath = context.options.renderPath;

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
                outError = "GPU-driven showcase received an invalid render path";
                return false;
        }
        context.renderSettings.gpuCulling.enableDistanceCulling = true;
        context.renderSettings.gpuCulling.enableOcclusionCulling = false;
        context.renderSettings.shadows.enabled = false;
        context.renderSettings.postProcess.enabled = true;
        context.renderSettings.postProcess.enableTAA = false;
        context.renderSettings.postProcess.enableBloom = false;
        context.renderSettings.postProcess.enableSSAO = false;
        context.renderSettings.postProcess.enableSSR = false;

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        context.camera.SetPerspective(GPUDrivenVerticalFov,
                                      aspect,
                                      0.05f,
                                      10000.0f);
        context.camera.SetPosition(Vec3(0.0f, 2.0f, 6.0f));
        context.camera.LookAt(Vec3(0.0f));

        ActorSpawnParams skyParams;
        skyParams.name = "GPUDrivenShowcaseSky";
        SceneEntity* skyEntity = context.scene.SpawnActor(skyParams);
        SkyboxComponent* skybox =
            skyEntity ? skyEntity->AddComponent<SkyboxComponent>() : nullptr;
        if (!skybox)
        {
            outError = "Failed to create the GPU-driven showcase skybox";
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
        lightParams.name = "GPUDrivenShowcaseSun";
        SceneEntity* lightEntity = context.scene.SpawnActor(lightParams);
        LightComponent* light =
            lightEntity ? lightEntity->AddComponent<LightComponent>() : nullptr;
        if (!light)
        {
            outError = "Failed to create the GPU-driven showcase light";
            return false;
        }
        lightEntity->SetRotation(
            QuatFromEuler(Vec3(radians(-50.0f), radians(25.0f), 0.0f)));
        light->SetLightType(LightType::Directional);
        light->SetColor(Vec3(1.0f, 0.95f, 0.86f));
        light->SetIntensity(4.0f);
        light->SetCastsShadow(false);
        m_lightCreated = true;

        LoadedSampleModel sourceModel;
        if (!context.models.Request(context.options.modelPath,
                                    sourceModel,
                                    outError,
                                    false))
        {
            outError = "GPU-driven showcase failed to queue its model: " +
                       outError;
            return false;
        }
        m_models.push_back(std::move(sourceModel));
        return true;
    }

    void GPUDrivenShowcaseSample::Update(SampleContext& context,
                                         float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (!m_failureReason.empty())
            return;

        for (LoadedSampleModel& model : m_models)
        {
            const SceneAssetStatus status =
                context.models.UpdateReadiness(model);
            if (status.IsFailed() ||
                status.lifecycle == SceneAssetLifecycle::Cancelled)
            {
                FailAndCancel(
                    context,
                    status.diagnostic.empty()
                        ? "GPU-driven showcase model request failed"
                        : status.diagnostic);
                return;
            }
        }

        if (m_instancesPlaced)
        {
            bool everyModelFullyResident =
                m_models.size() == GPUDrivenGridInstanceCount;
            for (const LoadedSampleModel& model : m_models)
                everyModelFullyResident &= model.IsFullyResident();
            if (everyModelFullyResident && !m_renderablesActivated)
            {
                for (const LoadedSampleModel& model : m_models)
                {
                    if (!SceneAssetInstantiator::SetRenderablesEnabled(
                            context.scene,
                            model.instance,
                            true))
                    {
                        FailAndCancel(
                            context,
                            "GPU-driven showcase could not activate a placed model instance");
                        return;
                    }
                }
                m_renderablesActivated = true;
            }
            return;
        }

        DisableUnplacedInstances(context);
        if (m_models.empty() || !m_models.front().IsCPUReady())
            return;

        std::string error;
        if (!PlaceInstances(context, error))
        {
            FailAndCancel(
                context,
                error.empty()
                    ? "GPU-driven showcase failed to place shared model instances"
                    : std::move(error));
        }
    }

    void GPUDrivenShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
    }

    bool GPUDrivenShowcaseSample::PlaceInstances(SampleContext& context,
                                                  std::string& outError)
    {
        outError.clear();
        if (m_models.size() != 1 || !m_models.front().resource.IsLoaded())
        {
            outError = "GPU-driven showcase source model is not CPU-ready";
            return false;
        }

        LoadedSampleModel& sourceModel = m_models.front();
        SceneEntity* sourceRoot = sourceModel.ResolveRoot(context.scene);
        if (!sourceRoot)
        {
            outError = "GPU-driven showcase model root handle is stale";
            return false;
        }
        const AABB sourceBounds = sourceRoot->GetWorldBounds();
        if (!sourceBounds.IsValid())
        {
            outError = "GPU-driven showcase model has invalid bounds";
            return false;
        }
        const Vec3 sourceSize = sourceBounds.GetSize();
        const float32 maximumDimension =
            std::max({sourceSize.x, sourceSize.y, sourceSize.z});
        if (!std::isfinite(maximumDimension) || maximumDimension <= 0.00001f)
        {
            outError = "GPU-driven showcase model has degenerate bounds";
            return false;
        }

        const auto sharedResource = sourceModel.resource;
        const std::filesystem::path sharedSourcePath = sourceModel.sourcePath;
        std::vector<LoadedSampleModel> stagedModels;
        stagedModels.reserve(GPUDrivenGridInstanceCount - 1u);
        for (uint32 index = 1; index < GPUDrivenGridInstanceCount; ++index)
        {
            LoadedSampleModel instance;
            if (!context.models.Instantiate(sharedResource,
                                            sharedSourcePath,
                                            instance,
                                            outError,
                                            false))
            {
                for (LoadedSampleModel& staged : stagedModels)
                    static_cast<void>(context.models.Cancel(staged));
                return false;
            }
            if (instance.resource.Get() != sharedResource.Get() ||
                !instance.ResolveRoot(context.scene))
            {
                static_cast<void>(context.models.Cancel(instance));
                for (LoadedSampleModel& staged : stagedModels)
                    static_cast<void>(context.models.Cancel(staged));
                outError =
                    "GPU-driven showcase did not preserve one shared model resource";
                return false;
            }
            stagedModels.push_back(std::move(instance));
        }

        std::vector<SceneEntity*> roots;
        roots.reserve(GPUDrivenGridInstanceCount);
        roots.push_back(sourceRoot);
        for (LoadedSampleModel& staged : stagedModels)
        {
            SceneEntity* root = staged.ResolveRoot(context.scene);
            if (!root)
            {
                for (LoadedSampleModel& model : stagedModels)
                    static_cast<void>(context.models.Cancel(model));
                outError = "GPU-driven showcase instance root handle is stale";
                return false;
            }
            roots.push_back(root);
        }

        const float32 spacing = maximumDimension * GPUDrivenGridSpacingScale;
        const float32 centerIndex =
            static_cast<float32>(GPUDrivenGridExtent - 1u) * 0.5f;
        AABB placedBounds;
        placedBounds.Reset();
        for (uint32 index = 0; index < GPUDrivenGridInstanceCount; ++index)
        {
            const uint32 row = index / GPUDrivenGridExtent;
            const uint32 column = index % GPUDrivenGridExtent;
            const Vec3 gridCenter(
                (static_cast<float32>(column) - centerIndex) * spacing,
                (centerIndex - static_cast<float32>(row)) * spacing,
                0.0f);
            roots[index]->SetPosition(gridCenter - sourceBounds.GetCenter());
            const AABB instanceBounds = roots[index]->GetWorldBounds();
            if (!instanceBounds.IsValid())
            {
                for (LoadedSampleModel& model : stagedModels)
                    static_cast<void>(context.models.Cancel(model));
                outError =
                    "GPU-driven showcase shared model instance has invalid bounds";
                return false;
            }
            placedBounds.Expand(instanceBounds);
        }

        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        const ModelCameraFrame cameraFrame = BuildModelCameraFrame(
            placedBounds,
            aspect,
            GPUDrivenVerticalFov,
            1.15f);
        if (!cameraFrame.valid)
        {
            for (LoadedSampleModel& model : stagedModels)
                static_cast<void>(context.models.Cancel(model));
            outError = "GPU-driven showcase model has no finite renderable bounds";
            return false;
        }

        m_models.reserve(GPUDrivenGridInstanceCount);
        for (LoadedSampleModel& staged : stagedModels)
            m_models.push_back(std::move(staged));
        m_bounds = placedBounds;
        m_cameraFrame = cameraFrame;
        m_sceneInstanceCount = static_cast<uint32>(m_models.size());
        context.camera.SetPerspective(GPUDrivenVerticalFov,
                                      aspect,
                                      m_cameraFrame.nearPlane,
                                      m_cameraFrame.farPlane);
        context.camera.SetPosition(
            m_cameraFrame.target +
            Vec3(0.0f,
                 m_cameraFrame.distance * 0.35f,
                 m_cameraFrame.distance * 0.94f));
        context.camera.LookAt(m_cameraFrame.target);
        context.camera.MarkCut();
        m_instancesPlaced = true;
        DisableUnplacedInstances(context);
        return true;
    }

    void GPUDrivenShowcaseSample::DisableUnplacedInstances(
        SampleContext& context) const
    {
        for (const LoadedSampleModel& model : m_models)
        {
            if (model.instance.rootActor.IsValid())
            {
                static_cast<void>(SceneAssetInstantiator::SetRenderablesEnabled(
                    context.scene,
                    model.instance,
                    false));
            }
        }
    }

    void GPUDrivenShowcaseSample::FailAndCancel(SampleContext& context,
                                                 std::string reason)
    {
        if (m_failureReason.empty())
            m_failureReason = std::move(reason);
        for (LoadedSampleModel& model : m_models)
        {
            if (model.loadHandle.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_sceneInstanceCount = 0;
        m_instancesPlaced = false;
        m_renderablesActivated = false;
    }

    void GPUDrivenShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        if (!m_models.empty() && m_models.front().resource.IsLoaded())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("SharedSceneExtractionInput");
            reporter.Enable("SharedModelResourceInstancing");
            reporter.ResourceDiagnostic(
                "model path=" + m_models.front().sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(m_models.front().resource->GetMeshCount()));
            reporter.ResourceDiagnostic(
                "scene model instances=" +
                std::to_string(m_sceneInstanceCount));
            reporter.ResourceDiagnostic("unique model resources=1");
        }
        reporter.Enable("RenderPathPolicySelection");
        reporter.ResourceDiagnostic(
            "render path=" + std::string(GetSampleRenderPathName(m_renderPath)));
        if (m_renderPath == SampleRenderPath::GPUDriven)
            reporter.Enable("GPUDrivenPathRequested");
        else if (m_renderPath == SampleRenderPath::Direct)
            reporter.Enable("DirectPathRequested");
        else
            reporter.Enable("AutomaticRenderPathRequested");
        if (m_cameraFrame.valid)
            reporter.Enable("BoundsCameraFraming");
        if (m_skyboxCreated)
            reporter.Enable("ProceduralSkyScene");
        if (m_lightCreated)
            reporter.Enable("DirectionalLightScene");
    }

    bool GPUDrivenShowcaseSample::IsGPUDrivenReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        const bool ready =
            diagnostics.gpuDrivenPolicyDecisionAvailable &&
            diagnostics.gpuDrivenRequestedMode == "ForceEnabled" &&
            diagnostics.gpuDrivenPolicyReason == "None" &&
            diagnostics.gpuDrivenEnabled &&
            diagnostics.visibleObjectCount >= m_sceneInstanceCount &&
            diagnostics.gpuDrivenGraphInputDrawItemCount >=
                m_sceneInstanceCount &&
            diagnostics.gpuDrivenGraphPassAdded &&
            diagnostics.gpuDrivenGraphPassRecorded &&
            diagnostics.gpuDrivenExecutionRecorded &&
            diagnostics.gpuDrivenOpaqueIndirectRequested &&
            diagnostics.gpuDrivenOpaqueIndirectEligible &&
            diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0 &&
            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound > 0 &&
            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound <
                diagnostics.gpuDrivenGraphInputDrawItemCount;
        if (!ready)
        {
            outPendingReason = "Waiting for GPU-driven indirect execution: " +
                               DescribeGPUDrivenState(diagnostics);
        }
        return ready;
    }

    bool GPUDrivenShowcaseSample::IsDirectReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        const bool ready =
            diagnostics.gpuDrivenPolicyDecisionAvailable &&
            diagnostics.gpuDrivenRequestedMode == "ForceDisabled" &&
            diagnostics.gpuDrivenPolicyReason == "ForcedDisabled" &&
            !diagnostics.gpuDrivenEnabled &&
            !diagnostics.gpuDrivenOpaqueIndirectRequested &&
            !diagnostics.gpuDrivenOpaqueIndirectEligible &&
            !diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
            diagnostics.gpuDrivenOpaqueIndirectBatchCount == 0 &&
            diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound == 0 &&
            diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
            diagnostics.opaqueInstancingPlanAvailable &&
            diagnostics.opaqueInstancingPreflightSucceeded &&
            diagnostics.opaqueInstancingExecutedPacketCount >=
                m_sceneInstanceCount &&
            diagnostics.opaqueInstancingSubmittedInstanceCount ==
                diagnostics.opaqueInstancingExecutedPacketCount &&
            diagnostics.opaqueInstancingSubmittedDrawCount <
                diagnostics.opaqueInstancingSubmittedInstanceCount &&
            diagnostics.opaqueInstancingBatchCount > 0 &&
            diagnostics.opaqueInstancingFallbackBatchCount == 0 &&
            diagnostics.gpuDrivenOpaqueFallbackReason == "Disabled";
        if (!ready)
        {
            outPendingReason = "Waiting for forced Direct execution: " +
                               DescribeGPUDrivenState(diagnostics);
        }
        return ready;
    }

    SampleReadiness GPUDrivenShowcaseSample::GetReadiness(
        const SampleRenderDiagnostics& diagnostics) const
    {
        if (!m_failureReason.empty())
            return SampleReadiness::Failed(m_failureReason);
        if (!m_instancesPlaced ||
            m_models.size() != GPUDrivenGridInstanceCount)
        {
            return SampleReadiness::Pending(
                "Waiting for all shared model instances to become CPU-ready and be placed");
        }
        for (const LoadedSampleModel& model : m_models)
        {
            if (!model.IsFullyResident())
            {
                return SampleReadiness::Pending(
                    "Waiting for every shared model instance to become fully resident");
            }
        }
        if (m_sceneInstanceCount == 0 ||
            diagnostics.visibleObjectCount < m_sceneInstanceCount)
        {
            return SampleReadiness::Pending(
                "GPU-driven showcase is waiting for every shared model instance to become visible");
        }

        std::string pendingReason;
        if (m_renderPath == SampleRenderPath::GPUDriven)
        {
            return IsGPUDrivenReady(diagnostics, pendingReason)
                       ? SampleReadiness::Ready()
                       : SampleReadiness::Pending(std::move(pendingReason));
        }
        if (m_renderPath == SampleRenderPath::Direct)
        {
            return IsDirectReady(diagnostics, pendingReason)
                       ? SampleReadiness::Ready()
                       : SampleReadiness::Pending(std::move(pendingReason));
        }

        if (diagnostics.gpuDrivenEnabled)
        {
            const bool ready =
                diagnostics.gpuDrivenPolicyDecisionAvailable &&
                diagnostics.gpuDrivenRequestedMode == "Auto" &&
                diagnostics.gpuDrivenPolicyReason == "None" &&
                diagnostics.gpuDrivenGraphInputDrawItemCount >=
                    m_sceneInstanceCount &&
                diagnostics.gpuDrivenGraphPassRecorded &&
                diagnostics.gpuDrivenExecutionRecorded &&
                diagnostics.gpuDrivenOpaqueIndirectSubmitted &&
                diagnostics.gpuDrivenOpaqueIndirectBatchCount > 0 &&
                diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound > 0 &&
                diagnostics.gpuDrivenOpaqueIndirectDrawUpperBound <
                    diagnostics.gpuDrivenGraphInputDrawItemCount;
            return ready
                       ? SampleReadiness::Ready()
                       : SampleReadiness::Pending(
                             "Waiting for Auto GPU-driven execution: " +
                             DescribeGPUDrivenState(diagnostics));
        }
        if (diagnostics.gpuDrivenPolicyReason == "BackendNotQualified")
        {
            const bool ready =
                diagnostics.gpuDrivenPolicyDecisionAvailable &&
                diagnostics.gpuDrivenRequestedMode == "Auto" &&
                diagnostics.gpuDrivenOpaqueDirectDrawCount > 0 &&
                diagnostics.opaqueInstancingPreflightSucceeded &&
                diagnostics.opaqueInstancingSubmittedInstanceCount >=
                    m_sceneInstanceCount &&
                diagnostics.opaqueInstancingSubmittedDrawCount <
                    diagnostics.opaqueInstancingSubmittedInstanceCount &&
                diagnostics.opaqueInstancingFallbackBatchCount == 0 &&
                !diagnostics.gpuDrivenOpaqueIndirectSubmitted;
            return ready
                       ? SampleReadiness::Ready()
                       : SampleReadiness::Pending(
                             "Waiting for honest Auto direct fallback: " +
                             DescribeGPUDrivenState(diagnostics));
        }
        return SampleReadiness::Pending(
            "Waiting for Auto render-path resolution: " +
            DescribeGPUDrivenState(diagnostics));
    }

    bool GPUDrivenShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        const SampleReadiness readiness = GetReadiness(diagnostics);
        outError = readiness.reason;
        return readiness.IsReady();
    }

    void GPUDrivenShowcaseSample::Shutdown(SampleContext& context)
    {
        for (LoadedSampleModel& model : m_models)
        {
            if (model.loadHandle.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_models.clear();
        m_bounds.Reset();
        m_cameraFrame = {};
        m_sceneInstanceCount = 0;
        m_renderPath = SampleRenderPath::GPUDriven;
        m_failureReason.clear();
        m_instancesPlaced = false;
        m_renderablesActivated = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
