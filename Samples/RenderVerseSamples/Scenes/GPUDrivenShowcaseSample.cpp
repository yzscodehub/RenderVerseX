/** @file GPUDrivenShowcaseSample.cpp @brief Direct/GPU-driven parity scene. */

#include "Scenes/GPUDrivenShowcaseSample.h"

#include "Core/MathTypes.h"
#include "RenderContracts/RenderFrameTypes.h"
#include "Samples/SampleCLI.h"
#include "Samples/SampleContext.h"
#include "Scene/ECS/RenderFragments.h"

#include <algorithm>
#include <array>
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
        constexpr float32 GPUDrivenPresentationExtent = 1.15f;
        constexpr float32 GPUDrivenGridSpacing = 1.60f;

        const SampleInfo GPUDrivenShowcaseInfo{
            "gpu-driven",
            "GPU-Driven Rendering",
            "Compares Direct and GPU-driven execution over the same engine scene",
            "water-bottle",
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
                .castsShadows = false,
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
            return "GPU-driven showcase ECS model request reached a terminal state.";
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
        m_orbitCamera.Reset();
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
        if (!context.cameras.SetPerspective(
                context.camera, GPUDrivenVerticalFov, aspect, 0.05f, 10000.0f) ||
            !context.cameras.SetPose(
                context.camera, {.position = Vec3(0.0f, 2.0f, 6.0f)}) ||
            !context.cameras.LookAt(context.camera, Vec3(0.0f)))
        {
            outError = "Failed to configure the GPU-driven showcase ECS camera";
            return false;
        }

        if (!context.sceneLifetime.CreateAndAdoptWithFragments(
                {}, MakeProceduralSky()).IsValid())
        {
            outError = "Failed to create the GPU-driven showcase skybox";
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
            outError = "Failed to create the GPU-driven showcase light";
            return false;
        }
        m_lightCreated = true;

        // Queue every instance against the same catalog identity up front.
        // ResourceManager coalesces the immutable load while each request keeps
        // its own ECS adoption/visibility/retirement transaction. Waiting for
        // the first instance before requesting the rest serializes 24 cache-hit
        // adoptions behind the visual gate's wall-clock budget.
        m_models.reserve(GPUDrivenGridInstanceCount);
        for (uint32 index = 0; index < GPUDrivenGridInstanceCount; ++index)
        {
            LoadedSampleModel instance;
            if (!context.models.RequestByAssetId(
                    context.options.assetId, instance, outError))
            {
                for (LoadedSampleModel& requested : m_models)
                {
                    static_cast<void>(context.models.Cancel(requested));
                }
                m_models.clear();
                outError = "GPU-driven showcase failed to queue shared model instance " +
                           std::to_string(index) + ": " + outError;
                return false;
            }
            m_models.push_back(std::move(instance));
        }
        return true;
    }

    void GPUDrivenShowcaseSample::Update(SampleContext& context,
                                         float deltaTime)
    {
        static_cast<void>(deltaTime);
        if (!m_failureReason.empty())
            return;

        bool everyModelCPUReady = !m_models.empty();
        for (LoadedSampleModel& model : m_models)
        {
            const ResourceSceneAdapters::EcsSceneAssetLoadStatus status =
                context.models.UpdateReadiness(model);
            if (status.IsTerminal())
            {
                FailAndCancel(context, DescribeModelFailure(status));
                return;
            }
            everyModelCPUReady &= model.IsCPUReady();
        }

        if (m_instancesPlaced)
        {
            bool everyModelFullyResident =
                m_models.size() == GPUDrivenGridInstanceCount;
            for (const LoadedSampleModel& model : m_models)
                everyModelFullyResident &= model.IsFullyResident();
            if (everyModelFullyResident && !m_renderablesActivated)
            {
                // The coordinator owns ECS Visibility writes and makes them
                // visible only after the required presentation proof.
                m_renderablesActivated = true;
            }
            return;
        }

        if (!everyModelCPUReady)
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
        if (!context.options.smoke && m_instancesPlaced && context.input)
        {
            static_cast<void>(
                m_orbitCamera.Update(*context.input, context.cameras, context.camera));
        }
    }

    void GPUDrivenShowcaseSample::OnViewportResize(SampleContext& context,
                                                    uint32 width,
                                                    uint32 height)
    {
        if (width == 0 || height == 0)
            return;
        const float32 aspect = static_cast<float32>(width) /
                               static_cast<float32>(height);
        static_cast<void>(
            m_orbitCamera.SetAspectRatio(aspect, context.cameras, context.camera));
    }

    bool GPUDrivenShowcaseSample::PlaceInstances(SampleContext& context,
                                                  std::string& outError)
    {
        outError.clear();
        if (m_models.empty() || !m_models.front().IsCPUReady())
        {
            outError = "GPU-driven showcase source model is not CPU-ready";
            return false;
        }

        if (m_models.size() != GPUDrivenGridInstanceCount)
        {
            outError = "GPU-driven showcase instance request count is invalid";
            return false;
        }

        LoadedSampleModel& sourceModel = m_models.front();

        const AssetId sharedModelAssetId =
            sourceModel.status.modelMetadata.sourceModelAssetId;
        if (!sharedModelAssetId.IsValid())
        {
            outError = "GPU-driven showcase source lacks ECS model metadata";
            return false;
        }

        std::vector<ECS::EntityHandle> roots;
        roots.reserve(GPUDrivenGridInstanceCount);
        for (const LoadedSampleModel& model : m_models)
        {
            if (!model.IsCPUReady() ||
                model.status.modelMetadata.sourceModelAssetId != sharedModelAssetId ||
                model.status.request.assetKey != sourceModel.status.request.assetKey)
            {
                outError =
                    "GPU-driven showcase did not preserve its shared model source";
                return false;
            }
            const auto root = model.GetRootEntityRef();
            if (!root.IsValid() ||
                root.sceneRuntimeId != context.scene.GetSceneRuntimeId() ||
                !context.scene.GetEntityRef(root.entity).IsValid())
            {
                outError = "GPU-driven showcase instance root handle is stale";
                return false;
            }
            roots.push_back(root.entity);
        }

        AABB sourceBounds;
        if (!TryComputeSampleModelRenderableWorldBounds(
                sourceModel, context.scene, sourceBounds))
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

        const float32 presentationScale =
            GPUDrivenPresentationExtent / maximumDimension;
        const float32 centerIndex =
            static_cast<float32>(GPUDrivenGridExtent - 1u) * 0.5f;
        AABB placedBounds;
        placedBounds.Reset();
        for (uint32 index = 0; index < GPUDrivenGridInstanceCount; ++index)
        {
            const uint32 row = index / GPUDrivenGridExtent;
            const uint32 column = index % GPUDrivenGridExtent;
            const ECS::EntityHandle root = roots[index];
            const SceneECS::LocalTransform* authored =
                context.scene.GetRegistry().TryGet<SceneECS::LocalTransform>(root);
            if (authored == nullptr)
            {
                outError = "GPU-driven showcase instance lacks a local transform";
                return false;
            }
            SceneECS::LocalTransform normalizedTransform = *authored;
            const Vec3 authoredPosition = normalizedTransform.translation;
            normalizedTransform.translation = authoredPosition * presentationScale;
            normalizedTransform.scale *= presentationScale;
            if (!context.scene.SetLocalTransform(root, normalizedTransform))
            {
                outError =
                    "GPU-driven showcase could not normalize an ECS model transform";
                return false;
            }
            LoadedSampleModel& instanceModel = m_models[index];
            AABB normalizedBounds;
            if (!TryComputeSampleModelRenderableWorldBounds(
                    instanceModel, context.scene, normalizedBounds))
            {
                outError =
                    "GPU-driven showcase could not normalize shared model bounds";
                return false;
            }
            const Vec3 gridCenter(
                (static_cast<float32>(column) - centerIndex) *
                    GPUDrivenGridSpacing,
                0.0f,
                (static_cast<float32>(row) - centerIndex) *
                     GPUDrivenGridSpacing);
            const Vec3 normalizedCenter = normalizedBounds.GetCenter();
            normalizedTransform.translation =
                authoredPosition * presentationScale +
                Vec3(gridCenter.x - normalizedCenter.x,
                     -normalizedBounds.GetMin().y,
                     gridCenter.z - normalizedCenter.z);
            if (!context.scene.SetLocalTransform(root, normalizedTransform))
            {
                outError =
                    "GPU-driven showcase could not place an ECS model instance";
                return false;
            }
            AABB instanceBounds;
            if (!TryComputeSampleModelRenderableWorldBounds(
                    instanceModel, context.scene, instanceBounds))
            {
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
            outError = "GPU-driven showcase model has no finite renderable bounds";
            return false;
        }

        SampleOrbitCameraSettings orbitSettings;
        orbitSettings.mode = OrbitCameraMode::ExteriorInspect;
        orbitSettings.bounds = placedBounds;
        orbitSettings.pivot = cameraFrame.target;
        orbitSettings.distance = cameraFrame.distance;
        orbitSettings.yaw = 0.62f;
        orbitSettings.pitch = 0.48f;
        orbitSettings.minDistance =
            std::max(cameraFrame.distance * 0.35f, 0.001f);
        orbitSettings.maxDistance =
            std::max(cameraFrame.distance * 4.0f,
                     orbitSettings.minDistance * 2.0f);
        orbitSettings.verticalFovRadians = GPUDrivenVerticalFov;
        orbitSettings.aspectRatio = aspect;
        orbitSettings.fitMargin = 1.16f;
        m_orbitCamera.Initialize(orbitSettings, context.input);
        if (!m_orbitCamera.IsInitialized())
        {
            outError = "GPU-driven showcase camera rig initialization failed";
            return false;
        }

        m_bounds = placedBounds;
        m_cameraFrame = cameraFrame;
        m_sceneInstanceCount = static_cast<uint32>(m_models.size());
        if (!m_orbitCamera.Apply(context.cameras, context.camera))
        {
            outError = "GPU-driven showcase could not apply its ECS orbit camera pose";
            return false;
        }
        m_instancesPlaced = true;
        return true;
    }

    void GPUDrivenShowcaseSample::FailAndCancel(SampleContext& context,
                                                 std::string reason)
    {
        if (m_failureReason.empty())
            m_failureReason = std::move(reason);
        for (LoadedSampleModel& model : m_models)
        {
            if (model.request.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_sceneInstanceCount = 0;
        m_instancesPlaced = false;
        m_renderablesActivated = false;
    }

    void GPUDrivenShowcaseSample::AppendReport(
        SampleFeatureReporter& reporter) const
    {
        const uint32 cpuReadyModelCount = static_cast<uint32>(std::count_if(
            m_models.begin(), m_models.end(),
            [](const LoadedSampleModel& model) { return model.IsCPUReady(); }));
        const uint32 fullyResidentModelCount = static_cast<uint32>(std::count_if(
            m_models.begin(), m_models.end(),
            [](const LoadedSampleModel& model) { return model.IsFullyResident(); }));
        constexpr size_t modelStateCount =
            static_cast<size_t>(ResourceSceneAdapters::EcsSceneAssetLoadState::Recycled) + 1u;
        std::array<uint32, modelStateCount> modelStates{};
        uint32 publishedMetadataCount = 0;
        for (const LoadedSampleModel& model : m_models)
        {
            const size_t state = static_cast<size_t>(model.status.state);
            if (state < modelStates.size())
            {
                ++modelStates[state];
            }
            publishedMetadataCount +=
                model.status.modelMetadata.HasPublishedSource() ? 1u : 0u;
        }
        std::ostringstream modelStateStream;
        modelStateStream << "model instance states=";
        bool firstState = true;
        for (size_t state = 0; state < modelStates.size(); ++state)
        {
            if (modelStates[state] == 0)
            {
                continue;
            }
            modelStateStream << (firstState ? "" : ",") << state << ":"
                             << modelStates[state];
            firstState = false;
        }
        reporter.ResourceDiagnostic(
            "requested model instances=" + std::to_string(m_models.size()));
        reporter.ResourceDiagnostic(
            "CPU-ready model instances=" + std::to_string(cpuReadyModelCount));
        reporter.ResourceDiagnostic(
            "fully resident model instances=" +
            std::to_string(fullyResidentModelCount));
        reporter.ResourceDiagnostic(
            "published model metadata instances=" +
            std::to_string(publishedMetadataCount));
        reporter.ResourceDiagnostic(modelStateStream.str());
        if (!m_models.empty() &&
            m_models.front().status.modelMetadata.HasPublishedSource())
        {
            reporter.Enable("ModelResourceLoad");
            reporter.Enable("SceneInstantiation");
            reporter.Enable("SharedSceneExtractionInput");
            reporter.Enable("SharedModelResourceInstancing");
            reporter.ResourceDiagnostic(
                "model path=" + m_models.front().sourcePath.string());
            reporter.ResourceDiagnostic(
                "model meshes=" +
                std::to_string(
                    m_models.front().status.modelMetadata.meshAssetIds.size()));
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
            if (model.request.IsValid())
                static_cast<void>(context.models.Cancel(model));
        }
        m_models.clear();
        m_bounds.Reset();
        m_cameraFrame = {};
        m_orbitCamera.Reset();
        m_sceneInstanceCount = 0;
        m_renderPath = SampleRenderPath::GPUDriven;
        m_failureReason.clear();
        m_instancesPlaced = false;
        m_renderablesActivated = false;
        m_skyboxCreated = false;
        m_lightCreated = false;
    }
} // namespace RVX
