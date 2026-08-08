/** @file GPUDrivenShowcaseSample.cpp @brief Direct/GPU-driven parity scene. */

#include "Scenes/GPUDrivenShowcaseSample.h"

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
#include <sstream>

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
                   << ", qualification="
                   << diagnostics.gpuDrivenQualification
                   << ", enabled=" << diagnostics.gpuDrivenEnabled
                   << ", graphPassAdded="
                   << diagnostics.gpuDrivenGraphPassAdded
                   << ", graphPassRecorded="
                   << diagnostics.gpuDrivenGraphPassRecorded
                   << ", executionRecorded="
                   << diagnostics.gpuDrivenExecutionRecorded
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
        LoadedSampleModel sourceModel;
        if (!context.models.Load(context.options.modelPath,
                                 context.scene,
                                 sourceModel,
                                 outError))
        {
            return false;
        }

        m_renderPath = context.options.renderPath;
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
        const Vec3 sourceCenter = sourceBounds.GetCenter();
        const float32 spacing = maximumDimension * GPUDrivenGridSpacingScale;
        const float32 centerIndex =
            static_cast<float32>(GPUDrivenGridExtent - 1u) * 0.5f;
        m_models.clear();
        m_models.reserve(GPUDrivenGridInstanceCount);
        m_bounds.Reset();
        for (uint32 row = 0; row < GPUDrivenGridExtent; ++row)
        {
            for (uint32 column = 0; column < GPUDrivenGridExtent; ++column)
            {
                LoadedSampleModel instance;
                if (row == 0 && column == 0)
                {
                    instance = std::move(sourceModel);
                }
                else
                {
                    if (!context.models.Instantiate(
                            sharedResource,
                            sharedSourcePath,
                            context.scene,
                            instance,
                            outError))
                    {
                        return false;
                    }
                }
                if (instance.resource.Get() != sharedResource.Get())
                {
                    outError =
                        "GPU-driven showcase did not preserve one shared model resource";
                    return false;
                }

                const Vec3 gridCenter(
                    (static_cast<float32>(column) - centerIndex) * spacing,
                    (centerIndex - static_cast<float32>(row)) * spacing,
                    0.0f);
                SceneEntity* instanceRoot =
                    instance.ResolveRoot(context.scene);
                if (!instanceRoot)
                {
                    outError =
                        "GPU-driven showcase instance root handle is stale";
                    return false;
                }
                instanceRoot->SetPosition(gridCenter - sourceCenter);
                const AABB placedBounds = instanceRoot->GetWorldBounds();
                if (!placedBounds.IsValid())
                {
                    outError =
                        "GPU-driven showcase shared model instance has invalid bounds";
                    return false;
                }
                m_bounds.Expand(placedBounds);
                m_models.push_back(std::move(instance));
            }
        }
        m_sceneInstanceCount = static_cast<uint32>(m_models.size());
        const float32 aspect =
            static_cast<float32>(context.options.width) /
            static_cast<float32>(std::max(context.options.height, 1u));
        m_cameraFrame = BuildModelCameraFrame(
            m_bounds,
            aspect,
            GPUDrivenVerticalFov,
            1.15f);
        if (!m_cameraFrame.valid)
        {
            outError = "GPU-driven showcase model has no finite renderable bounds";
            return false;
        }

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
        return true;
    }

    void GPUDrivenShowcaseSample::Update(SampleContext& context,
                                         float deltaTime)
    {
        static_cast<void>(context);
        static_cast<void>(deltaTime);
    }

    void GPUDrivenShowcaseSample::OnInput(SampleContext& context)
    {
        static_cast<void>(context);
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
        {
            reporter.Enable("GPUDrivenPathRequested");
        }
        else if (m_renderPath == SampleRenderPath::Direct)
        {
            reporter.Enable("DirectPathRequested");
        }
        else
        {
            reporter.Enable("AutomaticRenderPathRequested");
        }
        if (m_cameraFrame.valid)
        {
            reporter.Enable("BoundsCameraFraming");
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

    bool GPUDrivenShowcaseSample::IsReady(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outPendingReason) const
    {
        if (m_sceneInstanceCount == 0 ||
            diagnostics.visibleObjectCount < m_sceneInstanceCount)
        {
            outPendingReason =
                "GPU-driven showcase is waiting for every shared model instance to become visible";
            return false;
        }
        if (m_renderPath == SampleRenderPath::GPUDriven)
        {
            return IsGPUDrivenReady(diagnostics, outPendingReason);
        }
        if (m_renderPath == SampleRenderPath::Direct)
        {
            return IsDirectReady(diagnostics, outPendingReason);
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
            if (!ready)
            {
                outPendingReason =
                    "Waiting for Auto GPU-driven execution: " +
                    DescribeGPUDrivenState(diagnostics);
            }
            return ready;
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
            if (!ready)
            {
                outPendingReason =
                    "Waiting for honest Auto direct fallback: " +
                    DescribeGPUDrivenState(diagnostics);
            }
            return ready;
        }

        outPendingReason = "Waiting for Auto render-path resolution: " +
                           DescribeGPUDrivenState(diagnostics);
        return false;
    }

    bool GPUDrivenShowcaseSample::ValidateResult(
        const SampleRenderDiagnostics& diagnostics,
        std::string& outError) const
    {
        return IsReady(diagnostics, outError);
    }

    void GPUDrivenShowcaseSample::Shutdown(SampleContext& context)
    {
        static_cast<void>(context);
        m_models.clear();
        m_bounds.Reset();
        m_cameraFrame = {};
        m_sceneInstanceCount = 0;
        m_renderPath = SampleRenderPath::GPUDriven;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
